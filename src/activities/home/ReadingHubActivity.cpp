#include "ReadingHubActivity.h"

#include <ArduinoJson.h>
#include <BufferedFile.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "LibraryBookInput.h"
#include "LibraryCachePolicy.h"
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

class HubJsonFileReader {
 public:
  explicit HubJsonFileReader(HalFile& file) : input_(file, 512) {}

  int read() {
    uint8_t value = 0;
    return input_.read(&value, 1) == 1 ? value : -1;
  }

  size_t readBytes(char* output, const size_t length) { return input_.read(output, length); }

 private:
  serialization::BufferedFileReader input_;
};

// The outer array parser consumes the opening '{' so it can distinguish a record from a comma
// or the closing ']'. ArduinoJson still needs that byte; this adapter gives it back exactly once
// and then delegates to the same sequential 512-byte reader.
class PrefixedJsonObjectReader {
 public:
  explicit PrefixedJsonObjectReader(HubJsonFileReader& input) : input_(input) {}

  int read() {
    if (prefixPending_) {
      prefixPending_ = false;
      return '{';
    }
    return input_.read();
  }

  size_t readBytes(char* output, const size_t length) {
    if (length == 0) return 0;
    size_t written = 0;
    if (prefixPending_) {
      output[written++] = '{';
      prefixPending_ = false;
    }
    return written + input_.readBytes(output + written, length - written);
  }

 private:
  HubJsonFileReader& input_;
  bool prefixPending_ = true;
};

int readNonWhitespace(HubJsonFileReader& input) {
  int value = -1;
  do {
    value = input.read();
  } while (value == ' ' || value == '\t' || value == '\r' || value == '\n');
  return value;
}

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
  if (path == "/") return tr(STR_HUB_UNSORTED);
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
  // Keep only a concrete file. Returning the unresolved template makes every e-ink repaint scan
  // the cache directory again, even though Reading Hub never generates covers itself.
  return sibling;
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
  shelfSummaryAvailable = false;

  // Read the same catalog CoverLibraryActivity opens, one filtered JSON object at a time. This
  // includes converted manga folders (which CLX deliberately does not index) while bounding heap
  // to one path/cover record, seven shelf models and at most 4KB of completion hashes.
  std::unique_ptr<uint64_t[]> finishedHashes;
  uint16_t finishedHashCount = 0;
  HalFile file;
  if (!ReadingStatsStore::readFinishedPathHashesFromFile(finishedHashes, finishedHashCount) ||
      !Storage.openFileForRead("HUB", LIBRARY_CACHE_JSON, file)) {
    shelfTotalCount = 1;
    shelvesLoaded = true;
    return;
  }

  HubJsonFileReader input(file);
  constexpr char PREFIX[] = "{\"books\":[";
  bool parseOk = true;
  for (size_t index = 0; index < sizeof(PREFIX) - 1; index++) {
    if (input.read() != PREFIX[index]) {
      parseOk = false;
      break;
    }
  }

  JsonDocument filter;
  filter["path"] = true;
  filter["coverBmpPath"] = true;
  JsonDocument record;
  bool first = true;
  bool hasMoreShelves = false;
  size_t admittedCacheBooks = 0;
  uint16_t catalogBookCount = 0;
  std::array<uint8_t, RecentBooksStore::MAX_RECENT_BOOKS> recentSeen{};
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto addBook = [&](const std::string_view path, const std::string_view cover) {
    if (catalogBookCount < UINT16_MAX) catalogBookCount++;
    const uint64_t pathHash = ReadingStatsStore::finishedPathHash(path);
    if (finishedHashCount > 0 &&
        std::binary_search(finishedHashes.get(), finishedHashes.get() + finishedHashCount, pathHash)) {
      return;
    }

    const std::string_view folder = libraryFolderPath(path);
    int shelfIndex = -1;
    for (int candidate = 0; candidate < shelfPreviewCount; candidate++) {
      if (std::string_view{shelves[candidate].path} == folder) {
        shelfIndex = candidate;
        break;
      }
    }
    if (shelfIndex >= 0) {
      if (shelves[shelfIndex].bookCount < UINT16_MAX) shelves[shelfIndex].bookCount++;
      if (shelves[shelfIndex].coverBmpPath.empty() && !cover.empty() && cover.size() <= 500) {
        shelves[shelfIndex].coverBmpPath = cachedCoverPath(std::string(cover), 72);
      }
      return;
    }
    if (shelfPreviewCount >= MAX_SHELF_PREVIEW) {
      hasMoreShelves = true;
      return;
    }

    shelfIndex = shelfPreviewCount++;
    shelves[shelfIndex].path.assign(folder);
    shelves[shelfIndex].name = shelfName(folder);
    shelves[shelfIndex].bookCount = 1;
    if (!cover.empty() && cover.size() <= 500) {
      shelves[shelfIndex].coverBmpPath = cachedCoverPath(std::string(cover), 72);
    }
  };
  while (parseOk) {
    int token = readNonWhitespace(input);
    if (token == ']') {
      parseOk = readNonWhitespace(input) == '}';
      break;
    }
    if (!first) {
      if (token != ',') {
        parseOk = false;
        break;
      }
      token = readNonWhitespace(input);
    }
    if (token != '{') {
      parseOk = false;
      break;
    }

    record.clear();
    PrefixedJsonObjectReader objectInput(input);
    const DeserializationError error =
        deserializeJson(record, objectInput, DeserializationOption::Filter(filter.as<JsonVariantConst>()));
    if (error) {
      LOG_ERR("HUB", "Library cache record parse failed: %s", error.c_str());
      parseOk = false;
      break;
    }
    first = false;
    const char* path = record["path"] | "";
    const char* cover = record["coverBmpPath"] | "";
    const size_t pathLength = strnlen(path, library_cache::MAX_PATH_LENGTH + 1);
    if (pathLength > library_cache::MAX_PATH_LENGTH || !library_cache::admitsPath(std::string_view{path, pathLength})) {
      continue;
    }
    if (admittedCacheBooks >= library_cache::MAX_BOOKS) continue;
    admittedCacheBooks++;
    const std::string_view pathView{path, pathLength};
    for (size_t recentIndex = 0; recentIndex < recents.size() && recentIndex < recentSeen.size(); recentIndex++) {
      if (std::string_view{recents[recentIndex].path} == pathView) recentSeen[recentIndex] = 1;
    }
    const size_t coverLength = strnlen(cover, library_cache::MAX_PATH_LENGTH + 1);
    addBook(pathView,
            coverLength <= library_cache::MAX_PATH_LENGTH ? std::string_view{cover, coverLength} : std::string_view{});
  }
  file.close();
  if (!parseOk) {
    shelves = {};
    shelfPreviewCount = 0;
    shelfTotalCount = 1;
    shelvesLoaded = true;
    return;
  }

  // CoverLibraryActivity merges the ten-item recent list over the persisted scan cache for its
  // first frame. Mirror that bounded merge so a newly opened book is neither missing from the Hub
  // count nor sent to a shelf that disagrees with the destination while the background scan runs.
  for (size_t index = 0; index < recents.size() && index < recentSeen.size(); index++) {
    if (!recentSeen[index]) addBook(recents[index].path, recents[index].coverBmpPath);
  }

  std::sort(shelves.begin(), shelves.begin() + shelfPreviewCount,
            [](const ReadingHubShelf& left, const ReadingHubShelf& right) { return left.name < right.name; });
  shelfTotalCount = shelfPreviewCount + (hasMoreShelves ? 1 : 0);
  libraryBookCount = catalogBookCount;
  libraryCountKnown = true;
  shelfSummaryAvailable = true;
  shelvesLoaded = true;
}

void ReadingHubActivity::loadQueuePreview(const int limit) {
  queuePreviewCount = 0;
  bool changed = false;
  size_t queueIndex = 0;
  while (queueIndex < queueStore.queue().paths().size()) {
    const std::string& path = queueStore.queue().paths()[queueIndex];
    if (!Storage.exists(path.c_str())) {
      // Copy only the stale entry: remove() invalidates the reference and the queue is capped at
      // 32 paths. Valid entries do not get duplicated merely for cleanup.
      const std::string stalePath = path;
      changed = queueStore.queue().remove(stalePath) || changed;
      continue;
    }
    if (queuePreviewCount < std::min(limit, MAX_QUEUE_PREVIEW)) {
      queueBooks[queuePreviewCount] = resolveBook(path);
      cacheCoverPath(queueBooks[queuePreviewCount], 96);
      queuePreviewCount++;
    }
    queueIndex++;
  }
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
  std::unique_ptr<uint64_t[]> finishedHashes;
  uint16_t finishedHashCount = 0;
  if (!ReadingStatsStore::readFinishedPathHashesFromFile(finishedHashes, finishedHashCount)) {
    LOG_ERR("HUB", "Cannot read completion status; actions suppressed");
    return;
  }
  const bool isFinished =
      finishedHashCount > 0 && std::binary_search(finishedHashes.get(), finishedHashes.get() + finishedHashCount,
                                                  ReadingStatsStore::finishedPathHash(book.path));
  const int queueIndex = queueStore.queue().indexOf(book.path);
  const int actionQueueCount = section == Section::QUEUE ? std::min<int>(queueStore.queue().size(), MAX_QUEUE_PREVIEW)
                                                         : static_cast<int>(queueStore.queue().size());
  auto activity = makeUniqueNoThrow<BookActionsActivity>(renderer, mappedInput, book.title, queueIndex >= 0, isFinished,
                                                         queueIndex, actionQueueCount);
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
      READING_STATS_STORE = ReadingStatsStore{};
      if (!READING_STATS_STORE.loadFromFileForMutation()) {
        LOG_ERR("HUB", "Completion history is unreadable; refusing to overwrite it");
        READING_STATS_STORE = ReadingStatsStore{};
        return;
      }
      changed = READING_STATS_STORE.setBookFinished(path, true);
      if (changed && !READING_STATS_STORE.saveToFile()) {
        LOG_ERR("HUB", "Failed to save completed status");
        READING_STATS_STORE = ReadingStatsStore{};
        return;
      }
      READING_STATS_STORE = ReadingStatsStore{};
      if (queueStore.queue().remove(path)) {
        queueStore.saveToFile();
        changed = true;
      }
      rateAfterAction = true;
      break;
    case BookAction::MARK_UNFINISHED:
      READING_STATS_STORE = ReadingStatsStore{};
      if (!READING_STATS_STORE.loadFromFileForMutation()) {
        LOG_ERR("HUB", "Completion history is unreadable; refusing to overwrite it");
        READING_STATS_STORE = ReadingStatsStore{};
        return;
      }
      changed = READING_STATS_STORE.setBookFinished(path, false);
      if (changed && !READING_STATS_STORE.saveToFile()) {
        LOG_ERR("HUB", "Failed to save unfinished status");
        READING_STATS_STORE = ReadingStatsStore{};
        return;
      }
      READING_STATS_STORE = ReadingStatsStore{};
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
  READING_STATS_STORE = ReadingStatsStore{};
  if (!READING_STATS_STORE.loadFromFileForMutation()) {
    LOG_ERR("HUB", "Completion history is unreadable; refusing to overwrite it");
    READING_STATS_STORE = ReadingStatsStore{};
    return;
  }
  const uint8_t initialRating = READING_STATS_STORE.getBookRating(path);
  READING_STATS_STORE = ReadingStatsStore{};
  auto activity = makeUniqueNoThrow<BookRatingActivity>(renderer, mappedInput, title, initialRating);
  if (!activity) {
    LOG_ERR("HUB", "OOM: book rating");
    return;
  }
  auto handler = [this, path](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<IntervalResult>(result.data)) {
      const auto rating = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
      READING_STATS_STORE = ReadingStatsStore{};
      if (!READING_STATS_STORE.loadFromFileForMutation()) {
        LOG_ERR("HUB", "Completion history is unreadable; refusing to overwrite it");
        READING_STATS_STORE = ReadingStatsStore{};
        refreshAfterBookAction();
        return;
      }
      if (READING_STATS_STORE.setBookRating(path, rating) && !READING_STATS_STORE.saveToFile()) {
        LOG_ERR("HUB", "Failed to save book rating");
      }
      READING_STATS_STORE = ReadingStatsStore{};
    }
    refreshAfterBookAction();
  };
  startActivityForResult(std::move(activity), std::move(handler));
}

void ReadingHubActivity::refreshAfterBookAction() {
  queueStore.clear();
  queueStore.loadFromFile();
  queueFullyLoaded = false;
  shelvesLoaded = false;
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
        .shelfSummaryAvailable = shelfSummaryAvailable,
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
