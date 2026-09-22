#include "ReadingHubActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "LibraryBookInput.h"
#include "LibraryPerformance.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SeriesMetadata.h"
#include "activities/ActivityManager.h"
#include "activities/home/BookStatsActivity.h"
#include "activities/home/EpubProgressUtil.h"
#include "activities/home/XtcProgressUtil.h"
#include "activities/library/BookActionsActivity.h"
#include "activities/library/BookRatingActivity.h"
#include "components/UITheme.h"

namespace {
constexpr char LIBRARY_INDEX_LOG_TAG[] = "HUB";
constexpr char LIBRARY_CACHE_JSON[] = "/.crosspoint/library_cache.json";
constexpr unsigned long LONG_PRESS_MS = 1000;

std::string fallbackTitle(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end = dot == std::string::npos || dot <= start ? path.size() : dot;
  return path.substr(start, end - start);
}

int progressForBook(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book.path));
    return EpubProgress::percentFromCache(cachePath, LIBRARY_INDEX_LOG_TAG);
  }
  if (FsHelpers::hasXtcExtension(book.path)) return XtcProgress::percentForBook(book.path);
  return -1;
}

std::string shelfName(const std::string_view path) {
  if (path == "/") return "Unsorted";
  const size_t slash = path.find_last_of('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}
}  // namespace

ReadingHubActivity::ReadingHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const HomeMenuItem initialMenuItemValue, const bool cleanInitialRefreshValue)
    : Activity("Home", renderer, mappedInput),
      initialMenuItem(initialMenuItemValue),
      cleanInitialRefresh(cleanInitialRefreshValue) {}

void ReadingHubActivity::onEnter() {
  Activity::onEnter();

  switch (initialMenuItem) {
    case HomeMenuItem::LIBRARY:
    case HomeMenuItem::FILE_BROWSER:
      section = Section::LIBRARY;
      break;
    case HomeMenuItem::READING_STATS:
    case HomeMenuItem::COMPLETED_LIBRARY:
      section = Section::READ;
      break;
    case HomeMenuItem::READING_QUEUE:
      section = Section::QUEUE;
      break;
    default:
      section = Section::NOW;
      break;
  }

  queueStore.clear();
  queueStore.loadFromFile();

  library::LibraryIndexFile index;
  libraryCountKnown = index.open(library::libraryIndexPath());
  libraryBookCount = libraryCountKnown ? index.bookCount() : 0;

  shelvesLoaded = false;
  queueFullyLoaded = false;
  completedBooksLoaded = false;
  loadCompletionIndex();
  loadQueuePreview(section == Section::QUEUE ? MAX_QUEUE_PREVIEW : 1);
  ensureSectionLoaded();

  selectedIndex = 0;
  menuOpen = false;
  firstPaint = true;
  requestUpdate();
}

void ReadingHubActivity::onExit() {
  currentBook = {};
  hasCurrentBook = false;
  queueStore.clear();
  shelves = {};
  queueBooks = {};
  completedBooks = {};
  completedPaths = {};
  Activity::onExit();
}

const char* ReadingHubActivity::sectionLabel() const {
  static constexpr StrId SECTION_LABELS[SECTION_COUNT] = {StrId::STR_HUB_NOW, StrId::STR_HUB_LIBRARY,
                                                          StrId::STR_HUB_QUEUE, StrId::STR_HUB_READ};
  return I18N.get(SECTION_LABELS[static_cast<int>(section)]);
}

const char* ReadingHubActivity::menuItemLabel(const int index) const {
  static constexpr StrId MENU_LABELS[MENU_ITEM_COUNT] = {StrId::STR_BROWSE_FILES, StrId::STR_FILE_TRANSFER,
                                                         StrId::STR_STATS, StrId::STR_SETTINGS_TITLE};
  return index >= 0 && index < MENU_ITEM_COUNT ? I18N.get(MENU_LABELS[index]) : "";
}

int ReadingHubActivity::selectionCount() const {
  if (menuOpen) return MENU_ITEM_COUNT;
  switch (section) {
    case Section::NOW:
      return 3;
    case Section::LIBRARY:
      return std::max(1, shelfPreviewCount + (shelfTotalCount > shelfPreviewCount ? 1 : 0));
    case Section::QUEUE:
      return std::max(1, queuePreviewCount);
    case Section::READ:
      return 1 + completedPreviewCount;  // Show All is always the first action.
  }
  return 1;
}

RecentBook ReadingHubActivity::resolveBook(const std::string& path) const {
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto recent =
      std::find_if(recents.begin(), recents.end(), [&](const RecentBook& book) { return book.path == path; });
  RecentBook resolved = recent == recents.end() ? RECENT_BOOKS.getDataFromBook(path) : *recent;
  if (resolved.path.empty()) resolved.path = path;
  if (resolved.title.empty()) resolved.title = fallbackTitle(path);
  return resolved;
}

std::string ReadingHubActivity::cachedCoverPath(const std::string& templatePath, const int preferredHeight) {
  if (templatePath.empty()) return {};
  const std::string wanted = UITheme::getCoverThumbPath(templatePath, preferredHeight);
  if (Storage.exists(wanted.c_str())) return wanted;
  const std::string sibling = UITheme::findSiblingCoverThumb(wanted);
  return sibling.empty() ? templatePath : sibling;
}

void ReadingHubActivity::cacheCoverPath(RecentBook& book, const int preferredHeight) {
  book.coverBmpPath = cachedCoverPath(book.coverBmpPath, preferredHeight);
}

void ReadingHubActivity::selectCurrentBook(const std::vector<uint8_t>& recentCompleted) {
  currentBook = {};
  hasCurrentBook = false;
  const auto& recents = RECENT_BOOKS.getBooks();
  for (size_t index = 0; index < recents.size(); index++) {
    if ((index < recentCompleted.size() && recentCompleted[index] != 0) ||
        RecentBooksStore::isMissing(recents[index])) {
      continue;
    }
    currentBook = recents[index];
    cacheCoverPath(currentBook, 264);
    hasCurrentBook = true;
    break;
  }
  currentProgress = hasCurrentBook ? progressForBook(currentBook) : -1;
}

void ReadingHubActivity::loadCompletionIndex() {
  completedPathCount = 0;
  completedPaths = {};
  completedPathRatings.fill(0);

  std::vector<std::string> recentPaths;
  recentPaths.reserve(RECENT_BOOKS.getBooks().size());
  for (const auto& book : RECENT_BOOKS.getBooks()) recentPaths.push_back(book.path);
  std::vector<uint8_t> recentCompleted;
  std::vector<FinishedBookPreview> previews;
  if (!ReadingStatsStore::readFinishedPreviewFromFile(previews, MAX_COMPLETED_PREVIEW, completedBookCount,
                                                      ratedBookCount, ratingSum, &recentPaths, &recentCompleted)) {
    completedBookCount = 0;
    ratedBookCount = 0;
    ratingSum = 0;
    recentCompleted.assign(recentPaths.size(), 0);
  }
  for (const auto& preview : previews) {
    if (completedPathCount >= MAX_COMPLETED_PREVIEW) break;
    completedPaths[completedPathCount] = preview.path;
    completedPathRatings[completedPathCount] = preview.rating;
    completedPathCount++;
  }
  completedBooksLoaded = false;
  selectCurrentBook(recentCompleted);
}

void ReadingHubActivity::loadShelves() {
  shelves = {};
  shelfPreviewCount = 0;
  shelfTotalCount = 0;

  RecentBooksStore cache;
  std::vector<RecentBook> catalog;
  if (cache.loadFromPath(LIBRARY_CACHE_JSON)) {
    catalog = cache.takeBooks();
  } else {
    catalog = RECENT_BOOKS.getBooks();
  }
  if (!libraryCountKnown) libraryBookCount = static_cast<uint16_t>(std::min<size_t>(catalog.size(), UINT16_MAX));
  // One uint16 ordinal per cached book lets us group folders without copying the catalog's
  // strings. The vector is bounded by RecentBooksStore's 2048-entry cache cap and dies as soon as
  // the seven-row shelf model has been derived.
  const auto order =
      makeLibraryFolderOrder(catalog, [](const RecentBook& book) -> std::string_view { return book.path; });
  std::string previousFolder;
  int storedShelf = -1;
  for (const uint16_t bookIndex : order) {
    if (bookIndex >= catalog.size()) continue;
    const RecentBook& book = catalog[bookIndex];
    const std::string folder(libraryFolderPath(book.path));
    if (shelfTotalCount == 0 || folder != previousFolder) {
      previousFolder = folder;
      shelfTotalCount++;
      storedShelf = -1;
      if (shelfPreviewCount < MAX_SHELF_PREVIEW) {
        storedShelf = shelfPreviewCount++;
        shelves[storedShelf].path = folder;
        shelves[storedShelf].name = shelfName(folder);
        shelves[storedShelf].coverBmpPath = cachedCoverPath(book.coverBmpPath, 72);
        shelves[storedShelf].bookCount = 1;
      }
    } else if (storedShelf >= 0 && shelves[storedShelf].bookCount < UINT16_MAX) {
      shelves[storedShelf].bookCount++;
      if (shelves[storedShelf].coverBmpPath.empty()) {
        shelves[storedShelf].coverBmpPath = cachedCoverPath(book.coverBmpPath, 72);
      }
    }
  }
  shelvesLoaded = true;
}

void ReadingHubActivity::loadQueuePreview(const int limit) {
  queuePreviewCount = 0;
  std::vector<std::string> stale;
  stale.reserve(MAX_QUEUE_PREVIEW);
  for (const auto& path : queueStore.queue().paths()) {
    if (queuePreviewCount >= std::min(limit, MAX_QUEUE_PREVIEW)) break;
    if (!Storage.exists(path.c_str())) {
      stale.push_back(path);
      continue;
    }
    queueBooks[queuePreviewCount] = resolveBook(path);
    cacheCoverPath(queueBooks[queuePreviewCount], 96);
    queuePreviewCount++;
  }
  bool changed = false;
  for (const auto& path : stale) changed = queueStore.queue().remove(path) || changed;
  if (changed) queueStore.saveToFile();
  queueFullyLoaded = limit >= MAX_QUEUE_PREVIEW;
}

void ReadingHubActivity::loadCompletedBooks() {
  completedPreviewCount = 0;
  completedRatings.fill(0);
  std::vector<RecentBook> books;
  std::vector<uint8_t> ratings;
  books.reserve(completedPathCount);
  ratings.reserve(completedPathCount);
  for (int index = 0; index < completedPathCount; index++) {
    if (!Storage.exists(completedPaths[index].c_str())) continue;
    RecentBook book = resolveBook(completedPaths[index]);
    cacheCoverPath(book, 200);
    books.push_back(std::move(book));
    ratings.push_back(completedPathRatings[index]);
  }

  std::vector<uint16_t> order(books.size());
  for (size_t index = 0; index < order.size(); index++) order[index] = static_cast<uint16_t>(index);
  series_metadata::groupIndices(books, order);
  for (const uint16_t index : order) {
    if (completedPreviewCount >= MAX_COMPLETED_PREVIEW) break;
    completedBooks[completedPreviewCount] = std::move(books[index]);
    completedRatings[completedPreviewCount] = ratings[index];
    completedPreviewCount++;
  }
  completedBooksLoaded = true;
}

void ReadingHubActivity::ensureSectionLoaded() {
  if (section == Section::LIBRARY && !shelvesLoaded) loadShelves();
  if (section == Section::QUEUE && !queueFullyLoaded) loadQueuePreview(MAX_QUEUE_PREVIEW);
  if (section == Section::READ && !completedBooksLoaded) loadCompletedBooks();
}

void ReadingHubActivity::stepSection(const int delta) {
  if (menuOpen) return;
  section = reading_hub::stepSection(section, delta);
  selectedIndex = 0;
  ensureSectionLoaded();
  requestUpdate();
}

void ReadingHubActivity::stepSelection(const int delta) {
  selectedIndex = reading_hub::stepRow(selectedIndex, delta, selectionCount());
  requestUpdate();
}

void ReadingHubActivity::activateSelection() {
  if (menuOpen) {
    switch (selectedIndex) {
      case 0:
        activityManager.goToFileBrowser();
        return;
      case 1:
        activityManager.goToFileTransfer();
        return;
      case 2:
        activityManager.goToReadingStats();
        return;
      case 3:
        activityManager.goToSettings();
        return;
      default:
        return;
    }
  }

  switch (section) {
    case Section::NOW:
      if (selectedIndex == 0 && hasCurrentBook) {
        activityManager.goToReader(currentBook.path, true);
      } else if (selectedIndex == 0) {
        activityManager.goToLibrary();
      } else if (selectedIndex == 1) {
        activityManager.goToReadingQueue();
      } else {
        activityManager.goToCompletedLibrary();
      }
      return;
    case Section::LIBRARY:
      if (shelfPreviewCount == 0 || selectedIndex >= shelfPreviewCount) {
        activityManager.goToShelves();
      } else {
        activityManager.goToShelf(shelves[selectedIndex].path, shelves[selectedIndex].completed);
      }
      return;
    case Section::QUEUE:
      if (queuePreviewCount == 0) {
        activityManager.goToReadingQueue();
      } else if (selectedIndex < queuePreviewCount) {
        activityManager.goToReader(queueBooks[selectedIndex].path, true);
      }
      return;
    case Section::READ:
      if (selectedIndex == 0) {
        activityManager.goToCompletedLibrary();
      } else if (selectedIndex - 1 < completedPreviewCount) {
        activityManager.goToReader(completedBooks[selectedIndex - 1].path, true);
      }
      return;
  }
}

const RecentBook* ReadingHubActivity::selectedBook() const {
  if (menuOpen) return nullptr;
  switch (section) {
    case Section::NOW:
      if (selectedIndex == 0 && hasCurrentBook) return &currentBook;
      if (selectedIndex == 1 && queuePreviewCount > 0) return &queueBooks[0];
      return nullptr;
    case Section::LIBRARY:
      return nullptr;
    case Section::QUEUE:
      return selectedIndex < queuePreviewCount ? &queueBooks[selectedIndex] : nullptr;
    case Section::READ:
      return selectedIndex > 0 && selectedIndex - 1 < completedPreviewCount ? &completedBooks[selectedIndex - 1]
                                                                            : nullptr;
  }
  return nullptr;
}

void ReadingHubActivity::showBookActions(const RecentBook& book) {
  READING_STATS_STORE = ReadingStatsStore{};
  READING_STATS_STORE.loadFromFile();
  const int queueIndex = queueStore.queue().indexOf(book.path);
  auto activity = makeUniqueNoThrow<BookActionsActivity>(renderer, mappedInput, book.title, queueIndex >= 0,
                                                         READING_STATS_STORE.isBookFinished(book.path), queueIndex,
                                                         static_cast<int>(queueStore.queue().size()));
  if (!activity) {
    LOG_ERR("HUB", "OOM: book actions");
    return;
  }
  const std::string path = book.path;
  const std::string title = book.title;
  auto handler = [this, path, title](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<IntervalResult>(result.data)) return;
    applyBookAction(static_cast<BookAction>(std::get<IntervalResult>(result.data).value), path, title);
  };
  startActivityForResult(std::move(activity), std::move(handler));
}

void ReadingHubActivity::applyBookAction(const BookAction action, const std::string& path, const std::string& title) {
  bool changed = false;
  bool rateAfterAction = false;
  switch (action) {
    case BookAction::VIEW_STATS:
      showBookStats(path, title);
      return;
    case BookAction::ADD_TO_QUEUE:
      changed = queueStore.queue().add(path);
      if (changed) queueStore.saveToFile();
      break;
    case BookAction::REMOVE_FROM_QUEUE:
      changed = queueStore.queue().remove(path);
      if (changed) queueStore.saveToFile();
      break;
    case BookAction::MARK_COMPLETED:
      changed = READING_STATS_STORE.setBookFinished(path, true);
      if (changed) READING_STATS_STORE.saveToFile();
      if (queueStore.queue().remove(path)) {
        queueStore.saveToFile();
        changed = true;
      }
      rateAfterAction = true;
      break;
    case BookAction::MARK_UNFINISHED:
      changed = READING_STATS_STORE.setBookFinished(path, false);
      if (changed) READING_STATS_STORE.saveToFile();
      break;
    case BookAction::RATE_BOOK:
      showBookRating(path, title);
      return;
    case BookAction::MOVE_EARLIER:
      changed = queueStore.queue().moveEarlier(path);
      if (changed) queueStore.saveToFile();
      break;
    case BookAction::MOVE_LATER:
      changed = queueStore.queue().moveLater(path);
      if (changed) queueStore.saveToFile();
      break;
  }
  if (changed) {
    refreshAfterBookAction();
    if (section == Section::QUEUE) {
      const int newQueueIndex = queueStore.queue().indexOf(path);
      if (newQueueIndex >= 0 && newQueueIndex < queuePreviewCount) selectedIndex = newQueueIndex;
    }
  }
  if (rateAfterAction) showBookRating(path, title);
}

void ReadingHubActivity::showBookStats(const std::string& path, const std::string& title) {
  auto activity = makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, path, title);
  if (!activity) {
    LOG_ERR("HUB", "OOM: book stats");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}

void ReadingHubActivity::showBookRating(const std::string& path, const std::string& title) {
  auto activity =
      makeUniqueNoThrow<BookRatingActivity>(renderer, mappedInput, title, READING_STATS_STORE.getBookRating(path));
  if (!activity) {
    LOG_ERR("HUB", "OOM: book rating");
    return;
  }
  auto handler = [this, path](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<IntervalResult>(result.data)) {
      const auto rating = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
      if (READING_STATS_STORE.setBookRating(path, rating)) READING_STATS_STORE.saveToFile();
    }
    refreshAfterBookAction();
  };
  startActivityForResult(std::move(activity), std::move(handler));
}

void ReadingHubActivity::refreshAfterBookAction() {
  queueStore.clear();
  queueStore.loadFromFile();
  queueFullyLoaded = false;
  completedBooksLoaded = false;
  loadCompletionIndex();
  loadQueuePreview(section == Section::QUEUE ? MAX_QUEUE_PREVIEW : 1);
  ensureSectionLoaded();
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, selectionCount() - 1));
  requestUpdate();
}

void ReadingHubActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    menuOpen = !menuOpen;
    selectedIndex = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    stepSection(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    stepSection(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    stepSelection(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    stepSelection(1);
    return;
  }
  if (const RecentBook* book = selectedBook()) {
    const auto action = pollLibraryBookInput(mappedInput, LONG_PRESS_MS, false);
    if (action == LibraryBookInputAction::ShowActions) {
      showBookActions(*book);
      return;
    }
    if (action == LibraryBookInputAction::OpenBook) {
      activateSelection();
      return;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void ReadingHubActivity::render(RenderLock&&) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  selectedIndex = std::clamp(selectedIndex, 0, std::max(0, selectionCount() - 1));

  char title[40] = {0};
  if (menuOpen) {
    snprintf(title, sizeof(title), "%s", tr(STR_HUB_MENU));
  } else {
    snprintf(title, sizeof(title), tr(STR_HUB_SECTION_POSITION), sectionLabel(), static_cast<unsigned>(section) + 1);
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, title);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect content{metrics.contentSidePadding, contentTop, width - metrics.contentSidePadding * 2,
                     std::max(0, contentBottom - contentTop)};

  if (menuOpen) {
    GUI.drawList(
        renderer, content, MENU_ITEM_COUNT, selectedIndex,
        [this](const int index) { return std::string(menuItemLabel(index)); }, nullptr, nullptr, nullptr, false,
        nullptr, false);
  } else {
    const ReadingHubScreen screen{
        .section = static_cast<uint8_t>(section),
        .selectedIndex = selectedIndex,
        .currentBook = hasCurrentBook ? &currentBook : nullptr,
        .currentProgress = currentProgress,
        .nextBook = queuePreviewCount > 0 ? &queueBooks[0] : nullptr,
        .shelves = shelves.data(),
        .shelfPreviewCount = shelfPreviewCount,
        .shelfTotalCount = shelfTotalCount,
        .libraryTotalCount = libraryBookCount,
        .queueBooks = queueBooks.data(),
        .queuePreviewCount = queuePreviewCount,
        .queueTotalCount = static_cast<int>(queueStore.queue().size()),
        .completedBooks = completedBooks.data(),
        .completedRatings = completedRatings.data(),
        .completedPreviewCount = completedPreviewCount,
        .completedTotalCount = completedBookCount,
        .ratedBookCount = ratedBookCount,
        .averageRatingTenths =
            ratedBookCount > 0 ? static_cast<int>((ratingSum * 10u + ratedBookCount / 2u) / ratedBookCount) : 0};
    GUI.drawReadingHubScreen(renderer, content, screen);
  }

  const char* confirmLabel =
      !menuOpen && section == Section::READ && selectedIndex == 0 ? tr(STR_HUB_SHOW_ALL) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(menuOpen ? tr(STR_BACK) : tr(STR_HUB_MENU), confirmLabel, "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(cleanInitialRefresh && firstPaint ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstPaint = false;
}
