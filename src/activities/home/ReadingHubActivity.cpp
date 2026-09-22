#include "ReadingHubActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/home/EpubProgressUtil.h"
#include "activities/home/XtcProgressUtil.h"
#include "components/UITheme.h"

namespace {
constexpr char LIBRARY_INDEX_LOG_TAG[] = "HUB";

std::string fallbackTitle(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end = dot == std::string::npos || dot <= start ? path.size() : dot;
  return path.substr(start, end - start);
}

int progressForBook(const RecentBook* book) {
  if (!book) return -1;
  if (FsHelpers::hasEpubExtension(book->path)) {
    const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book->path));
    return EpubProgress::percentFromCache(cachePath, LIBRARY_INDEX_LOG_TAG);
  }
  if (FsHelpers::hasXtcExtension(book->path)) return XtcProgress::percentForBook(book->path);
  return -1;
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

  currentBook = nullptr;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!RecentBooksStore::isMissing(book)) {
      currentBook = &book;
      break;
    }
  }
  currentProgress = progressForBook(currentBook);

  queueStore.clear();
  queueStore.loadFromFile();

  library::LibraryIndexFile index;
  libraryCountKnown = index.open(library::libraryIndexPath());
  libraryBookCount = libraryCountKnown ? index.bookCount() : 0;

  loadCompletedPreview();
  loadLibraryPreview();
  loadQueuePreview();

  selectedIndex = 0;
  menuOpen = false;
  firstPaint = true;
  requestUpdate();
}

void ReadingHubActivity::onExit() {
  currentBook = nullptr;
  queueStore.clear();
  libraryBooks = {};
  queueBooks = {};
  completedBooks = {};
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
      return 1 + libraryPreviewCount;
    case Section::QUEUE:
      return std::max(1, queuePreviewCount);
    case Section::READ:
      return std::max(1, completedPreviewCount);
  }
  return 1;
}

bool ReadingHubActivity::isInCompletedPreview(const std::string& path) const {
  return std::any_of(completedBooks.begin(), completedBooks.begin() + completedPreviewCount,
                     [&](const RecentBook& book) { return book.path == path; });
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

void ReadingHubActivity::loadCompletedPreview() {
  completedPreviewCount = 0;
  completedRatings.fill(0);
  std::vector<FinishedBookPreview> previews;
  if (!ReadingStatsStore::readFinishedPreviewFromFile(previews, MAX_COMPLETED_PREVIEW, completedBookCount,
                                                      ratedBookCount, ratingSum)) {
    completedBookCount = 0;
    ratedBookCount = 0;
    ratingSum = 0;
    return;
  }
  for (const auto& preview : previews) {
    if (completedPreviewCount >= MAX_COMPLETED_PREVIEW) break;
    completedBooks[completedPreviewCount] = resolveBook(preview.path);
    completedRatings[completedPreviewCount] = preview.rating;
    completedPreviewCount++;
  }
}

void ReadingHubActivity::loadLibraryPreview() {
  libraryPreviewCount = 0;
  const auto& recents = RECENT_BOOKS.getBooks();
  std::array<bool, RecentBooksStore::MAX_RECENT_BOOKS> used{};

  for (size_t first = 0; first < recents.size() && first < used.size() && libraryPreviewCount < MAX_LIBRARY_PREVIEW;
       first++) {
    if (used[first] || RecentBooksStore::isMissing(recents[first]) || isInCompletedPreview(recents[first].path)) {
      continue;
    }

    used[first] = true;
    libraryBooks[libraryPreviewCount++] = recents[first];
    if (recents[first].series.empty()) continue;

    for (size_t member = first + 1;
         member < recents.size() && member < used.size() && libraryPreviewCount < MAX_LIBRARY_PREVIEW; member++) {
      if (used[member] || recents[member].series != recents[first].series ||
          RecentBooksStore::isMissing(recents[member]) || isInCompletedPreview(recents[member].path)) {
        continue;
      }
      size_t insertAt = static_cast<size_t>(libraryPreviewCount);
      while (insertAt > 0 && libraryBooks[insertAt - 1].series == recents[member].series &&
             recents[member].seriesPosition > 0 &&
             (libraryBooks[insertAt - 1].seriesPosition == 0 ||
              recents[member].seriesPosition < libraryBooks[insertAt - 1].seriesPosition)) {
        libraryBooks[insertAt] = std::move(libraryBooks[insertAt - 1]);
        insertAt--;
      }
      libraryBooks[insertAt] = recents[member];
      libraryPreviewCount++;
      used[member] = true;
    }
  }
}

void ReadingHubActivity::loadQueuePreview() {
  queuePreviewCount = 0;
  for (const auto& path : queueStore.queue().paths()) {
    if (queuePreviewCount >= MAX_QUEUE_PREVIEW) break;
    queueBooks[queuePreviewCount++] = resolveBook(path);
  }
}

void ReadingHubActivity::stepSection(const int delta) {
  if (menuOpen) return;
  section = reading_hub::stepSection(section, delta);
  selectedIndex = 0;
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
      if (selectedIndex == 0 && currentBook) {
        activityManager.goToReader(currentBook->path, true);
      } else if (selectedIndex == 0) {
        activityManager.goToLibrary();
      } else if (selectedIndex == 1) {
        activityManager.goToReadingQueue();
      } else {
        activityManager.goToCompletedLibrary();
      }
      return;
    case Section::LIBRARY:
      if (selectedIndex == 0) {
        activityManager.goToShelves();
      } else if (selectedIndex - 1 < libraryPreviewCount) {
        activityManager.goToReader(libraryBooks[selectedIndex - 1].path, true);
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
      if (completedPreviewCount == 0) {
        activityManager.goToCompletedLibrary();
      } else if (selectedIndex < completedPreviewCount) {
        activityManager.goToReader(completedBooks[selectedIndex].path, true);
      }
      return;
  }
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
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelection();
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
        .currentBook = currentBook,
        .currentProgress = currentProgress,
        .nextBook = queuePreviewCount > 0 ? &queueBooks[0] : nullptr,
        .libraryBooks = libraryBooks.data(),
        .libraryPreviewCount = libraryPreviewCount,
        .libraryTotalCount = libraryCountKnown ? libraryBookCount : libraryPreviewCount,
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

  const auto labels = mappedInput.mapLabels(menuOpen ? tr(STR_BACK) : tr(STR_HUB_MENU), tr(STR_OPEN), tr(STR_HUB_MOVE),
                                            tr(STR_HUB_MOVE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(cleanInitialRefresh && firstPaint ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstPaint = false;
}
