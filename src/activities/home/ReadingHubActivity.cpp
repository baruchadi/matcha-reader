#include "ReadingHubActivity.h"

#include <ArduinoJson.h>
#include <BufferedFile.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
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

#include "CrossPointSettings.h"
#include "LibraryBookInput.h"
#include "LibraryCachePolicy.h"
#include "LibraryPerformance.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
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

enum class CatalogReconcileState : uint8_t { NOT_ATTEMPTED, SUCCEEDED, FAILED };
CatalogReconcileState catalogReconcileState = CatalogReconcileState::NOT_ATTEMPTED;
enum class CacheValidationState : uint8_t { UNKNOWN, VALID, INVALID };
CacheValidationState cacheValidationState = CacheValidationState::UNKNOWN;

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
      consumed_++;
      return '{';
    }
    if (consumed_ >= library_cache::MAX_RECORD_BYTES) return -1;
    const int value = input_.read();
    if (value >= 0) consumed_++;
    return value;
  }

  size_t readBytes(char* output, const size_t length) {
    if (length == 0) return 0;
    size_t written = 0;
    if (prefixPending_) {
      output[written++] = '{';
      prefixPending_ = false;
      consumed_++;
    }
    const size_t remaining =
        consumed_ < library_cache::MAX_RECORD_BYTES ? library_cache::MAX_RECORD_BYTES - consumed_ : 0;
    const size_t wanted = std::min(length - written, remaining);
    const size_t read = input_.readBytes(output + written, wanted);
    consumed_ += read;
    return written + read;
  }

 private:
  HubJsonFileReader& input_;
  bool prefixPending_ = true;
  size_t consumed_ = 0;
};

int readNonWhitespace(HubJsonFileReader& input) {
  int value = -1;
  do {
    value = input.read();
  } while (value == ' ' || value == '\t' || value == '\r' || value == '\n');
  return value;
}

int progressForBook(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book.path));
    return EpubProgress::percentFromCache(cachePath, LIBRARY_INDEX_LOG_TAG);
  }
  if (FsHelpers::hasXtcExtension(book.path)) return XtcProgress::percentForBook(book.path);
  return -1;
}

std::string coverTemplateForBook(const std::string& path) {
  const bool epub = FsHelpers::hasEpubExtension(path);
  const bool xtc = FsHelpers::hasXtcExtension(path);
  const bool text = FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
  if (text) return {};
  const char* prefix = epub ? "epub_" : (xtc ? "xtc_" : "manga_");
  return std::string("/.crosspoint/") + prefix + std::to_string(std::hash<std::string>{}(path)) + "/thumb_[HEIGHT].bmp";
}

std::string shelfName(const std::string_view path) {
  if (path == "/") return tr(STR_HUB_UNSORTED);
  const size_t slash = path.find_last_of('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

uint64_t coverAttemptKey(const std::string& path, const int height) {
  return library::clixPathHash(path.data(), path.size()) ^
         (static_cast<uint64_t>(static_cast<uint32_t>(height)) * 0x9E3779B97F4A7C15ULL);
}

}  // namespace

ReadingHubActivity::ReadingHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const HomeMenuItem initialMenuItemValue, const bool cleanInitialRefreshValue)
    : Activity("Home", renderer, mappedInput),
      initialMenuItem(initialMenuItemValue),
      cleanInitialRefresh(cleanInitialRefreshValue) {}

void ReadingHubActivity::onEnter() {
  Activity::onEnter();
  page = Page::ROOT;

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
  lastInputMs = millis();
  coverAttemptCount = 0;
  coverWorker.start("HubCover");

  library::LibraryIndexFile index;
  libraryCountKnown = index.open(library::libraryIndexPath());
  libraryBookCount = libraryCountKnown ? index.bookCount() : 0;

  shelvesLoaded = false;
  indexBuildAttempted = false;
  collectionTotalKnown = false;
  queueFullyLoaded = false;
  completedBooksLoadLimit = 0;
  loadCompletionIndex();
  loadQueuePreview(section == Section::QUEUE ? MAX_QUEUE_PREVIEW : 1);
  ensureSectionLoaded();

  selectedIndex = 0;
  menuOpen = false;
  firstPaint = true;
  requestUpdate();
}

void ReadingHubActivity::onExit() {
  coverWorker.stop();
  currentBook = {};
  hasCurrentBook = false;
  queueStore.clear();
  shelves = {};
  collectionShelves = {};
  queueBooks = {};
  completedBooks = {};
  completedPaths = {};
  completedHashes.reset();
  completedHashCount = 0;
  completedHashesValidated = false;
  collectionBooks = {};
  collectionRatings.fill(0);
  collectionTitle.clear();
  collectionShelfPath.clear();
  collectionShelfId = UINT16_MAX;
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
      return reading_hub::readSelectionCount(completedPreviewCount);
  }
  return 1;
}

RecentBook ReadingHubActivity::resolveBook(const std::string& path) const {
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto recent =
      std::find_if(recents.begin(), recents.end(), [&](const RecentBook& book) { return book.path == path; });
  if (recent != recents.end()) return *recent;
  // Keep path-only records cheap. The visible page is hydrated in one CLX scan and one optional
  // cache pass; resolving each book through RecentBooksStore would reopen every EPUB here.
  return RecentBook{path, "", "", ""};
}

std::string ReadingHubActivity::cachedCoverPath(const std::string& templatePath, const int preferredHeight) {
  if (templatePath.empty()) return {};
  const std::string wanted = UITheme::getCoverThumbPath(templatePath, preferredHeight);
  if (cover_thumbnail::heightValid(wanted, preferredHeight)) return wanted;
  const std::string sibling = UITheme::findSiblingCoverThumb(wanted);
  // Keep only a concrete file. Returning the unresolved template makes every e-ink repaint scan
  // the cache directory again, even though Reading Hub never generates covers itself.
  return sibling;
}

void ReadingHubActivity::cacheCoverPath(RecentBook& book, const int preferredHeight) {
  const std::string canonical = coverTemplateForBook(book.path);
  book.coverBmpPath = cachedCoverPath(canonical.empty() ? book.coverBmpPath : canonical, preferredHeight);
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
  completedHashes.reset();
  completedHashCount = 0;
  completedHashesValidated = false;

  std::unique_ptr<uint64_t[]> storedHashes;
  uint16_t storedHashCount = 0;
  if (ReadingStatsStore::readFinishedPathHashesFromFile(storedHashes, storedHashCount)) {
    library::LibraryIndexFile catalog;
    // Before the Hub's one-per-boot reconciliation, an existing CLX may describe the previous
    // card state. Direct existence checks are slower but authoritative for the first Now/Read
    // frame; a successful Library reconciliation calls this again and enables the fast hash set.
    if (catalogReconcileState == CatalogReconcileState::SUCCEEDED && catalog.open(library::libraryIndexPath())) {
      auto valid = makeUniqueNoThrow<uint64_t[]>(storedHashCount == 0 ? 1 : storedHashCount);
      if (valid) {
        struct CompletionContext {
          const uint64_t* stored;
          uint16_t storedCount;
          uint64_t* valid;
          uint16_t validCount = 0;
        } context{storedHashes.get(), storedHashCount, valid.get()};
        const auto collect = [](void* raw, const uint16_t, const library::ClixRecord& record) {
          auto& value = *static_cast<CompletionContext*>(raw);
          if (value.storedCount > 0 &&
              std::binary_search(value.stored, value.stored + value.storedCount, record.pathHash)) {
            value.valid[value.validCount++] = record.pathHash;
          }
          return true;
        };
        if (catalog.scanRecords(collect, &context)) {
          std::sort(valid.get(), valid.get() + context.validCount);
          completedHashes = std::move(valid);
          completedHashCount = context.validCount;
          completedHashesValidated = true;
        }
      }
    }
    if (!completedHashesValidated) {
      completedHashes = std::move(storedHashes);
      completedHashCount = storedHashCount;
    }
  }

  std::vector<std::string> recentPaths;
  recentPaths.reserve(RECENT_BOOKS.getBooks().size());
  for (const auto& book : RECENT_BOOKS.getBooks()) recentPaths.push_back(book.path);
  std::vector<uint8_t> recentCompleted;
  std::vector<FinishedBookPreview> previews;
  if (!ReadingStatsStore::readFinishedPreviewFromFile(
          previews, MAX_COMPLETED_CANDIDATES, completedBookCount, ratedBookCount, ratingSum, &recentPaths,
          &recentCompleted, 0, true, completedHashesValidated ? completedHashes.get() : nullptr, completedHashCount)) {
    completedBookCount = 0;
    ratedBookCount = 0;
    ratingSum = 0;
    recentCompleted.assign(recentPaths.size(), 0);
  }
  for (const auto& preview : previews) {
    if (completedPathCount >= MAX_COMPLETED_CANDIDATES) break;
    completedPaths[completedPathCount] = preview.path;
    completedPathRatings[completedPathCount] = preview.rating;
    completedPathCount++;
  }
  completedBooksLoadLimit = 0;
  selectCurrentBook(recentCompleted);
}

bool ReadingHubActivity::openCatalogIndex(library::LibraryIndexFile& index) {
  if (catalogReconcileState == CatalogReconcileState::NOT_ATTEMPTED && !indexBuildAttempted) {
    indexBuildAttempted = true;
    library::BuildStats stats;
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    catalogReconcileState = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0)
                                ? CatalogReconcileState::SUCCEEDED
                                : CatalogReconcileState::FAILED;
    if (catalogReconcileState == CatalogReconcileState::SUCCEEDED) loadCompletionIndex();
    if (catalogReconcileState == CatalogReconcileState::FAILED) {
      LOG_ERR("HUB", "Native catalog reconciliation failed; not retrying this boot");
    }
  }
  // The builder intentionally preserves the previous index when reconciliation fails. It is a
  // recovery artifact, not an authoritative view of the current card, so never expose it in the
  // Hub after an SD/I/O/memory failure this boot.
  if (catalogReconcileState != CatalogReconcileState::SUCCEEDED || !index.open(library::libraryIndexPath())) {
    libraryBookCount = 0;
    libraryCountKnown = false;
    return false;
  }
  libraryBookCount = index.bookCount();
  libraryCountKnown = true;
  return true;
}

RecentBook ReadingHubActivity::readCatalogBook(library::LibraryIndexFile& index, const uint16_t ordinal,
                                               const std::string* knownFolderPath) const {
  library::ClixRecord record{};
  RecentBook book;
  if (!index.readRecord(ordinal, record)) return book;
  if (knownFolderPath) {
    std::string name;
    if (!index.readName(record, name)) return book;
    book.path = library::joinLibraryPath(*knownFolderPath, name);
  } else if (!index.readPath(record, book.path)) {
    return book;
  }
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto recent = std::find_if(recents.begin(), recents.end(),
                                   [&](const RecentBook& candidate) { return candidate.path == book.path; });
  if (recent != recents.end()) return *recent;
  if (!index.readTitle(record, book.title)) index.readName(record, book.title);
  index.readAuthor(record, book.author);
  if (book.title.empty()) book.title = bookTitleFromPath(book.path);
  book.coverBmpPath = coverTemplateForBook(book.path);
  book.seriesMetadataScanned = !FsHelpers::hasEpubExtension(book.path);
  return book;
}

void ReadingHubActivity::enrichBooksFromIndex(RecentBook* books, const int count) const {
  if (!books || count <= 0 || count > MAX_COMPLETED_PREVIEW) return;
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) return;

  struct MatchContext {
    RecentBook* books;
    int count;
    int remaining;
    std::array<uint16_t, MAX_COMPLETED_PREVIEW> ordinals;
  } context{books, count, count, {}};
  context.ordinals.fill(UINT16_MAX);
  const auto match = [](void* raw, const uint16_t ordinal, const library::ClixRecord& record) {
    auto& value = *static_cast<MatchContext*>(raw);
    for (int position = 0; position < value.count; position++) {
      if (value.ordinals[position] != UINT16_MAX) continue;
      const auto& path = value.books[position].path;
      if (record.pathHash != library::clixPathHash(path.data(), path.size())) continue;
      value.ordinals[position] = ordinal;
      value.remaining--;
      break;
    }
    return value.remaining > 0;
  };
  if (!index.scanRecords(match, &context)) return;

  for (int position = 0; position < count; position++) {
    if (context.ordinals[position] == UINT16_MAX) continue;
    library::ClixRecord record{};
    if (!index.readRecord(context.ordinals[position], record)) return;
    std::string value;
    if (index.readTitle(record, value)) books[position].title = std::move(value);
    if (books[position].title.empty() && index.readName(record, value)) {
      books[position].title = bookTitleFromPath(value);
    }
    if (books[position].author.empty() && index.readAuthor(record, value)) books[position].author = std::move(value);
    if (books[position].coverBmpPath.empty()) books[position].coverBmpPath = coverTemplateForBook(books[position].path);
    if (!FsHelpers::hasEpubExtension(books[position].path)) books[position].seriesMetadataScanned = true;
  }
}

void ReadingHubActivity::enrichBooksFromCache(RecentBook* books, const int count) const {
  if (!books || count <= 0 || count > MAX_COMPLETED_PREVIEW) return;
  const auto scanCache = [&](RecentBook* destination, const bool validateWholeFile) {
    HalFile file;
    if (!Storage.openFileForRead("HUB", LIBRARY_CACHE_JSON, file) || file.size() > library_cache::MAX_FILE_BYTES) {
      return false;
    }
    std::array<uint8_t, MAX_COMPLETED_PREVIEW> matched{};
    int matchedCount = 0;
    HubJsonFileReader input(file);
    constexpr char PREFIX[] = "{\"books\":[";
    for (size_t index = 0; index < sizeof(PREFIX) - 1; index++) {
      if (input.read() != PREFIX[index]) return false;
    }

    JsonDocument filter;
    filter["path"] = true;
    filter["title"] = true;
    filter["author"] = true;
    filter["coverBmpPath"] = true;
    filter["series"] = true;
    filter["seriesPosition"] = true;
    filter["seriesMetadataScanned"] = true;
    JsonDocument record;
    bool first = true;
    size_t records = 0;
    while (true) {
      int token = readNonWhitespace(input);
      if (token == ']') return readNonWhitespace(input) == '}' && readNonWhitespace(input) == -1;
      if (!first) {
        if (token != ',') return false;
        token = readNonWhitespace(input);
      }
      if (token != '{' || ++records > library_cache::MAX_BOOKS) return false;
      first = false;
      record.clear();
      PrefixedJsonObjectReader objectInput(input);
      const DeserializationError error =
          deserializeJson(record, objectInput, DeserializationOption::Filter(filter.as<JsonVariantConst>()));
      if (error) return false;

      const char* path = record["path"] | "";
      const char* title = record["title"] | "";
      const char* author = record["author"] | "";
      const char* cover = record["coverBmpPath"] | "";
      const char* series = record["series"] | "";
      const size_t pathLength = strnlen(path, library_cache::MAX_PATH_LENGTH + 1);
      if (!library_cache::admitsPath(std::string_view{path, pathLength}) ||
          strnlen(title, library_cache::MAX_TITLE_LENGTH + 1) > library_cache::MAX_TITLE_LENGTH ||
          strnlen(author, library_cache::MAX_AUTHOR_LENGTH + 1) > library_cache::MAX_AUTHOR_LENGTH ||
          strnlen(cover, library_cache::MAX_COVER_PATH_LENGTH + 1) > library_cache::MAX_COVER_PATH_LENGTH ||
          strnlen(series, library_cache::MAX_SERIES_LENGTH + 1) > library_cache::MAX_SERIES_LENGTH) {
        return false;
      }
      for (int index = 0; index < count; index++) {
        if (destination[index].path != path) continue;
        destination[index].title = title;
        destination[index].author = author;
        destination[index].coverBmpPath = cover;
        destination[index].series = series;
        destination[index].seriesPosition = record["seriesPosition"] | 0;
        destination[index].seriesMetadataScanned = record["seriesMetadataScanned"] | false;
        if (!matched[index]) {
          matched[index] = 1;
          matchedCount++;
        }
        break;
      }
      if (!validateWholeFile && matchedCount == count) return true;
    }
  };

  // On first use, stage just the six visible records while validating the whole stream. This
  // preserves fail-closed publication without reading a valid cache twice.
  if (cacheValidationState == CacheValidationState::UNKNOWN) {
    auto staged = makeUniqueNoThrow<RecentBook[]>(count);
    if (!staged) return;
    for (int index = 0; index < count; index++) staged[index] = books[index];
    if (scanCache(staged.get(), true)) {
      cacheValidationState = CacheValidationState::VALID;
      for (int index = 0; index < count; index++) books[index] = std::move(staged[index]);
    } else {
      cacheValidationState = CacheValidationState::INVALID;
    }
    return;
  }
  if (cacheValidationState == CacheValidationState::VALID) scanCache(books, false);
}

void ReadingHubActivity::hydrateBookPage(RecentBook* books, const int count) const {
  enrichBooksFromIndex(books, count);
  enrichBooksFromCache(books, count);
  for (int position = 0; position < count; position++) {
    if (books[position].title.empty()) books[position].title = bookTitleFromPath(books[position].path);
    if (books[position].coverBmpPath.empty()) books[position].coverBmpPath = coverTemplateForBook(books[position].path);
  }
}

bool ReadingHubActivity::loadShelfPage(const int pageNumber, std::array<ReadingHubShelf, MAX_SHELF_PREVIEW>& output,
                                       int& visibleCount, int& totalCount) {
  output = {};
  visibleCount = 0;
  totalCount = 0;
  library::LibraryIndexFile index;
  if (!openCatalogIndex(index)) return false;

  struct FolderSummary {
    uint16_t bookCount;
    uint16_t firstOrdinal;
  };
  const uint16_t folderCount = index.header().folderCount;
  auto summaries = makeUniqueNoThrow<FolderSummary[]>(folderCount == 0 ? 1 : folderCount);
  if (!summaries) return false;
  for (uint16_t folder = 0; folder < folderCount; folder++) summaries[folder] = {0, UINT16_MAX};

  struct SummaryContext {
    FolderSummary* summaries;
    uint16_t folderCount;
    const uint64_t* finishedHashes;
    uint16_t finishedHashCount;
  } context{summaries.get(), folderCount, completedHashes.get(), completedHashCount};
  const auto summarize = [](void* raw, const uint16_t ordinal, const library::ClixRecord& record) {
    auto& value = *static_cast<SummaryContext*>(raw);
    if (record.folderId >= value.folderCount ||
        (value.finishedHashCount > 0 &&
         std::binary_search(value.finishedHashes, value.finishedHashes + value.finishedHashCount, record.pathHash))) {
      return true;
    }
    auto& folder = value.summaries[record.folderId];
    if (folder.bookCount == 0) folder.firstOrdinal = ordinal;
    if (folder.bookCount < UINT16_MAX) folder.bookCount++;
    return true;
  };
  if (!index.scanRecords(summarize, &context)) return false;

  const int offset = pageNumber * MAX_SHELF_PREVIEW;
  std::array<uint16_t, MAX_SHELF_PREVIEW> firstOrdinals{};
  int activeOrdinal = 0;
  for (uint16_t folder = 0; folder < folderCount; folder++) {
    if (summaries[folder].bookCount == 0) continue;
    if (activeOrdinal >= offset && visibleCount < MAX_SHELF_PREVIEW) {
      auto& shelf = output[visibleCount];
      shelf.catalogId = folder;
      shelf.bookCount = summaries[folder].bookCount;
      firstOrdinals[visibleCount] = summaries[folder].firstOrdinal;
      visibleCount++;
    }
    activeOrdinal++;
  }
  totalCount = activeOrdinal;
  summaries.reset();

  std::array<uint16_t, MAX_SHELF_PREVIEW> folderIds{};
  auto folderPaths = makeUniqueNoThrow<std::string[]>(visibleCount == 0 ? 1 : visibleCount);
  if (!folderPaths) return false;
  for (int position = 0; position < visibleCount; position++) folderIds[position] = output[position].catalogId;
  if (visibleCount > 0 && !index.readFolderPaths(folderIds.data(), visibleCount, folderPaths.get())) return false;
  for (int position = 0; position < visibleCount; position++) {
    auto& shelf = output[position];
    shelf.path = std::move(folderPaths[position]);
    shelf.name = shelfName(shelf.path);
    RecentBook firstBook = readCatalogBook(index, firstOrdinals[position], &shelf.path);
    shelf.coverBookPath = firstBook.path;
    shelf.coverBmpPath = cachedCoverPath(firstBook.coverBmpPath, 72);
  }
  return true;
}

void ReadingHubActivity::loadShelves() {
  shelfSummaryAvailable = loadShelfPage(0, shelves, shelfPreviewCount, shelfTotalCount);
  if (!shelfSummaryAvailable) {
    shelves = {};
    shelfPreviewCount = 0;
    shelfTotalCount = 0;
  }
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
      queuePreviewCount++;
    }
    queueIndex++;
  }
  if (changed) queueStore.saveToFile();
  hydrateBookPage(queueBooks.data(), queuePreviewCount);
  for (int index = 0; index < queuePreviewCount; index++) cacheCoverPath(queueBooks[index], 96);
  queueFullyLoaded = limit >= MAX_QUEUE_PREVIEW;
}

void ReadingHubActivity::loadCompletedBooks(const int limit) {
  completedPreviewCount = 0;
  completedRatings.fill(0);
  // readFinishedPreviewFromFile() already returns newest first. Copy directly: applying the
  // Library's series grouping here would make an older volume appear more recently completed.
  const int visibleCount = std::min({limit, MAX_COMPLETED_PREVIEW, completedPathCount});
  for (int index = 0; index < visibleCount; index++) {
    completedBooks[completedPreviewCount] = resolveBook(completedPaths[index]);
    completedRatings[completedPreviewCount] = completedPathRatings[index];
    completedPreviewCount++;
  }
  hydrateBookPage(completedBooks.data(), completedPreviewCount);
  for (int index = 0; index < completedPreviewCount; index++) cacheCoverPath(completedBooks[index], 200);
  completedBooksLoadLimit = limit;
}

int ReadingHubActivity::collectionPageCount() const {
  const int pageSize = page == Page::ALL_SHELVES ? COLLECTION_SHELVES_PER_PAGE : COLLECTION_BOOKS_PER_PAGE;
  return reading_hub::collectionPageCount(collectionTotalCount, pageSize);
}

void ReadingHubActivity::openShelfCollection(const ReadingHubShelf& shelf) {
  if (shelf.catalogId == UINT16_MAX) return;
  if (page == Page::ALL_SHELVES) {
    collectionParentPage = page;
    collectionParentPageNumber = collectionPageNumber;
    collectionParentSelectedIndex = collectionSelectedIndex;
  } else {
    collectionParentPage = Page::ROOT;
  }
  page = Page::SHELF_BOOKS;
  collectionTitle = shelf.name;
  collectionShelfPath = shelf.path;
  collectionShelfId = shelf.catalogId;
  collectionPageNumber = 0;
  collectionSelectedIndex = 0;
  collectionTotalKnown = false;
  loadCollectionPage();
  requestUpdate();
}

void ReadingHubActivity::openAllShelvesCollection() {
  collectionParentPage = Page::ROOT;
  page = Page::ALL_SHELVES;
  collectionTitle = tr(STR_HUB_YOUR_SHELVES);
  collectionShelfPath.clear();
  collectionShelfId = UINT16_MAX;
  collectionPageNumber = 0;
  collectionSelectedIndex = 0;
  collectionTotalKnown = false;
  loadCollectionPage();
  requestUpdate();
}

void ReadingHubActivity::openCompletedCollection() {
  collectionParentPage = Page::ROOT;
  page = Page::COMPLETED_BOOKS;
  collectionTitle = tr(STR_HUB_COMPLETED_BOOKS);
  collectionShelfPath.clear();
  collectionShelfId = UINT16_MAX;
  collectionPageNumber = 0;
  collectionSelectedIndex = 0;
  collectionTotalKnown = false;
  loadCollectionPage();
  requestUpdate();
}

void ReadingHubActivity::closeCollection() {
  if (page == Page::SHELF_BOOKS && collectionParentPage == Page::ALL_SHELVES) {
    page = Page::ALL_SHELVES;
    collectionTitle = tr(STR_HUB_YOUR_SHELVES);
    collectionShelfPath.clear();
    collectionShelfId = UINT16_MAX;
    collectionPageNumber = collectionParentPageNumber;
    collectionSelectedIndex = collectionParentSelectedIndex;
    collectionParentPage = Page::ROOT;
    loadCollectionPage();
    requestUpdate();
    return;
  }
  section = reading_hub::parentSection(page);
  page = Page::ROOT;
  collectionBooks = {};
  collectionRatings.fill(0);
  collectionTitle.clear();
  collectionShelfPath.clear();
  collectionShelfId = UINT16_MAX;
  collectionBookCount = 0;
  collectionShelfCount = 0;
  collectionTotalCount = 0;
  collectionSelectedIndex = 0;
  collectionPageNumber = 0;
  collectionParentPage = Page::ROOT;
  collectionTotalKnown = false;
  requestUpdate();
}

void ReadingHubActivity::stepCollectionPage(const int delta) {
  collectionPageNumber = reading_hub::stepCollectionPage(
      collectionPageNumber, delta, collectionTotalCount,
      page == Page::ALL_SHELVES ? COLLECTION_SHELVES_PER_PAGE : COLLECTION_BOOKS_PER_PAGE);
  collectionSelectedIndex = 0;
  loadCollectionPage();
  requestUpdate();
}

void ReadingHubActivity::loadCollectionPage() {
  // A mutation can empty the last page. Two bounded passes are enough: the first learns the new
  // total and clamps, the second loads that final valid page without recursive stack growth.
  for (int attempt = 0; attempt < 2; attempt++) {
    collectionBooks = {};
    collectionShelves = {};
    collectionRatings.fill(0);
    collectionBookCount = 0;
    collectionShelfCount = 0;
    if (page == Page::ALL_SHELVES) {
      if (!loadShelfPage(collectionPageNumber, collectionShelves, collectionShelfCount, collectionTotalCount)) {
        collectionShelfCount = 0;
        collectionTotalCount = 0;
      }
    } else if (page == Page::SHELF_BOOKS) {
      loadShelfCollectionPage();
    } else if (page == Page::COMPLETED_BOOKS) {
      loadCompletedCollectionPage();
    }
    const int lastPage = collectionPageCount() - 1;
    if (collectionPageNumber <= lastPage) break;
    collectionPageNumber = lastPage;
  }
  const int visibleCount = page == Page::ALL_SHELVES ? collectionShelfCount : collectionBookCount;
  collectionSelectedIndex = std::clamp(collectionSelectedIndex, 0, std::max(0, visibleCount - 1));
}

void ReadingHubActivity::loadShelfCollectionPage() {
  library::LibraryIndexFile index;
  if (!openCatalogIndex(index) || collectionShelfId == UINT16_MAX) {
    collectionTotalCount = 0;
    collectionTotalKnown = true;
    return;
  }
  struct ShelfPageContext {
    const uint64_t* finishedHashes;
    uint16_t finishedHashCount;
    uint16_t shelfId;
    int offset;
    int matched = 0;
    bool totalKnown;
    std::array<uint16_t, ReadingHubActivity::COLLECTION_BOOKS_PER_PAGE> ordinals{};
    int visible = 0;
  } context{completedHashes.get(), completedHashCount, collectionShelfId,
            collectionPageNumber * COLLECTION_BOOKS_PER_PAGE, collectionTotalKnown};

  const auto collectBook = [](void* raw, const uint16_t ordinal, const library::ClixRecord& record) {
    auto& load = *static_cast<ShelfPageContext*>(raw);
    if (record.folderId != load.shelfId) return true;
    if (load.finishedHashCount > 0 &&
        std::binary_search(load.finishedHashes, load.finishedHashes + load.finishedHashCount, record.pathHash)) {
      return true;
    }
    if (load.matched >= load.offset && load.visible < ReadingHubActivity::COLLECTION_BOOKS_PER_PAGE) {
      load.ordinals[load.visible++] = ordinal;
    }
    load.matched++;
    return !(load.totalKnown && load.visible >= ReadingHubActivity::COLLECTION_BOOKS_PER_PAGE);
  };

  if (!index.scanRecords(collectBook, &context)) {
    collectionTotalCount = 0;
    collectionTotalKnown = true;
    return;
  }
  if (!collectionTotalKnown) {
    collectionTotalCount = context.matched;
    collectionTotalKnown = true;
  }
  for (int visible = 0; visible < context.visible; visible++) {
    RecentBook book = readCatalogBook(index, context.ordinals[visible], &collectionShelfPath);
    if (book.path.empty()) continue;
    collectionBooks[collectionBookCount++] = std::move(book);
  }
  enrichBooksFromCache(collectionBooks.data(), collectionBookCount);
  for (int visible = 0; visible < collectionBookCount; visible++) cacheCoverPath(collectionBooks[visible], 200);
}

void ReadingHubActivity::loadCompletedCollectionPage() {
  std::vector<FinishedBookPreview> previews;
  uint16_t total = 0;
  uint16_t rated = 0;
  uint32_t sum = 0;
  const size_t offset = static_cast<size_t>(collectionPageNumber) * COLLECTION_BOOKS_PER_PAGE;
  if (!ReadingStatsStore::readFinishedPreviewFromFile(
          previews, COLLECTION_BOOKS_PER_PAGE, total, rated, sum, nullptr, nullptr, offset, true,
          completedHashesValidated ? completedHashes.get() : nullptr, completedHashCount)) {
    collectionTotalCount = 0;
    return;
  }
  collectionTotalCount = total;
  for (const auto& preview : previews) {
    if (collectionBookCount >= COLLECTION_BOOKS_PER_PAGE) continue;
    RecentBook book = resolveBook(preview.path);
    collectionBooks[collectionBookCount] = std::move(book);
    collectionRatings[collectionBookCount] = preview.rating;
    collectionBookCount++;
  }
  hydrateBookPage(collectionBooks.data(), collectionBookCount);
  for (int visible = 0; visible < collectionBookCount; visible++) cacheCoverPath(collectionBooks[visible], 200);
  collectionTotalKnown = true;
}

const RecentBook* ReadingHubActivity::selectedCollectionBook() const {
  if ((page != Page::SHELF_BOOKS && page != Page::COMPLETED_BOOKS) || collectionSelectedIndex < 0 ||
      collectionSelectedIndex >= collectionBookCount) {
    return nullptr;
  }
  return &collectionBooks[collectionSelectedIndex];
}

void ReadingHubActivity::ensureSectionLoaded() {
  if (section == Section::LIBRARY && !shelvesLoaded) loadShelves();
  if (section == Section::QUEUE && !queueFullyLoaded) loadQueuePreview(MAX_QUEUE_PREVIEW);
  const int completedLimit = section == Section::NOW    ? MAX_NOW_COMPLETED_PREVIEW
                             : section == Section::READ ? MAX_COMPLETED_PREVIEW
                                                        : 0;
  if (completedBooksLoadLimit < completedLimit) loadCompletedBooks(completedLimit);
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
        section = Section::LIBRARY;
        selectedIndex = 0;
        ensureSectionLoaded();
        requestUpdate();
      } else if (selectedIndex == 1 && queuePreviewCount > 0) {
        activityManager.goToReader(queueBooks[0].path, true);
      } else if (selectedIndex == 1) {
        section = Section::QUEUE;
        selectedIndex = 0;
        ensureSectionLoaded();
        requestUpdate();
      } else {
        openCompletedCollection();
      }
      return;
    case Section::LIBRARY:
      if (shelfPreviewCount == 0) return;
      if (selectedIndex >= shelfPreviewCount)
        openAllShelvesCollection();
      else
        openShelfCollection(shelves[selectedIndex]);
      return;
    case Section::QUEUE:
      if (selectedIndex < queuePreviewCount) {
        activityManager.goToReader(queueBooks[selectedIndex].path, true);
      }
      return;
    case Section::READ:
      if (selectedIndex == reading_hub::readShowAllIndex(completedPreviewCount)) {
        openCompletedCollection();
      } else if (const int bookIndex = reading_hub::readBookIndex(selectedIndex, completedPreviewCount);
                 bookIndex >= 0) {
        activityManager.goToReader(completedBooks[bookIndex].path, true);
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
      if (const int bookIndex = reading_hub::readBookIndex(selectedIndex, completedPreviewCount); bookIndex >= 0) {
        return &completedBooks[bookIndex];
      }
      return nullptr;
  }
  return nullptr;
}

void ReadingHubActivity::showBookActions(const RecentBook& book) {
  const bool isFinished =
      completedHashCount > 0 && std::binary_search(completedHashes.get(), completedHashes.get() + completedHashCount,
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
  completedBooksLoadLimit = 0;
  collectionTotalKnown = false;
  loadCompletionIndex();
  loadQueuePreview(section == Section::QUEUE ? MAX_QUEUE_PREVIEW : 1);
  ensureSectionLoaded();
  if (page == Page::ROOT) {
    selectedIndex = std::clamp(selectedIndex, 0, std::max(0, selectionCount() - 1));
  } else {
    loadCollectionPage();
  }
  requestUpdate();
}

bool ReadingHubActivity::nextVisibleCoverTarget(RecentBook& target, int& height) const {
  const auto attempted = [this](const std::string& path, const int height) {
    const uint64_t hash = coverAttemptKey(path, height);
    const int retained = std::min<int>(coverAttemptCount, coverAttempts.size());
    return std::find(coverAttempts.begin(), coverAttempts.begin() + retained, hash) != coverAttempts.begin() + retained;
  };
  const auto needsWork = [&](const RecentBook& book, const int wantedHeight) {
    if (book.path.empty() || wantedHeight <= 0 || FsHelpers::hasTxtExtension(book.path) ||
        FsHelpers::hasMarkdownExtension(book.path) || attempted(book.path, wantedHeight)) {
      return false;
    }
    const std::string cover = coverTemplateForBook(book.path);
    // Page hydration resolves templates to a concrete existing thumbnail once. Reopening every
    // visible BMP header on every idle loop would turn the background worker itself into SD churn.
    const bool missingCover = book.coverBmpPath != UITheme::getCoverThumbPath(cover, wantedHeight);
    const bool missingSeries = FsHelpers::hasEpubExtension(book.path) && !book.seriesMetadataScanned;
    if (!missingCover && !missingSeries) return false;
    target = book;
    if (target.coverBmpPath.empty()) target.coverBmpPath = cover;
    height = wantedHeight;
    return true;
  };

  if (page == Page::ALL_SHELVES) {
    for (int index = 0; index < collectionShelfCount; index++) {
      RecentBook representative{collectionShelves[index].coverBookPath, "", "", collectionShelves[index].coverBmpPath};
      representative.seriesMetadataScanned = true;
      if (needsWork(representative, 72)) return true;
    }
    return false;
  }
  if (page == Page::SHELF_BOOKS || page == Page::COMPLETED_BOOKS) {
    for (int index = 0; index < collectionBookCount; index++) {
      if (needsWork(collectionBooks[index], 200)) return true;
    }
    return false;
  }
  if (menuOpen) return false;
  if (section == Section::NOW) {
    if (hasCurrentBook && needsWork(currentBook, 264)) return true;
    if (queuePreviewCount > 0 && needsWork(queueBooks[0], 96)) return true;
    for (int index = 0; index < std::min(completedPreviewCount, MAX_NOW_COMPLETED_PREVIEW); index++) {
      if (needsWork(completedBooks[index], 200)) return true;
    }
  } else if (section == Section::LIBRARY) {
    for (int index = 0; index < shelfPreviewCount; index++) {
      RecentBook representative{shelves[index].coverBookPath, "", "", shelves[index].coverBmpPath};
      representative.seriesMetadataScanned = true;
      if (needsWork(representative, 72)) return true;
    }
  } else if (section == Section::QUEUE) {
    for (int index = 0; index < queuePreviewCount; index++) {
      if (needsWork(queueBooks[index], 96)) return true;
    }
  } else if (section == Section::READ) {
    for (int index = 0; index < completedPreviewCount; index++) {
      if (needsWork(completedBooks[index], 200)) return true;
    }
  }
  return false;
}

void ReadingHubActivity::publishCoverResult(const RecentBook& result) {
  bool changed = false;
  const auto updateBook = [&](RecentBook& book, const int height) {
    if (book.path != result.path) return;
    if (!result.title.empty()) book.title = result.title;
    if (!result.author.empty()) book.author = result.author;
    if (result.seriesMetadataScanned) {
      book.series = result.series;
      book.seriesPosition = result.seriesPosition;
      book.seriesMetadataScanned = true;
    }
    book.coverBmpPath = coverTemplateForBook(book.path);
    cacheCoverPath(book, height);
    changed = true;
  };
  updateBook(currentBook, 264);
  for (auto& book : queueBooks) updateBook(book, 96);
  for (auto& book : completedBooks) updateBook(book, 200);
  for (auto& book : collectionBooks) updateBook(book, 200);
  const auto updateShelf = [&](ReadingHubShelf& shelf) {
    if (shelf.coverBookPath != result.path) return;
    shelf.coverBmpPath = cachedCoverPath(coverTemplateForBook(result.path), 72);
    changed = true;
  };
  for (auto& shelf : shelves) updateShelf(shelf);
  for (auto& shelf : collectionShelves) updateShelf(shelf);
  RECENT_BOOKS.updateBookData(result);
  if (changed) requestUpdate();
}

void ReadingHubActivity::stepVisibleCoverWorker() {
  if (coverWorker.busy()) return;
  if (const auto* result = coverWorker.pendingResult()) {
    if (result->completed) {
      const uint64_t hash = coverAttemptKey(result->book.path, result->primaryHeight);
      coverAttempts[coverAttemptCount % coverAttempts.size()] = hash;
      coverAttemptCount++;
      if (result->hasPrimaryThumb || result->metadataLoaded) publishCoverResult(result->book);
    }
    coverWorker.discardResult();
    return;
  }

  RecentBook book;
  int height = 0;
  if (!nextVisibleCoverTarget(book, height)) return;
  RenderLock lock{RenderLock::Try{}};
  if (!lock.held()) return;
  if (auto* fonts = renderer.getFontCacheManager()) fonts->releaseAllFontMemory();
  lock.unlock();

  CoverThumbnailWorker::Job job;
  job.book = std::move(book);
  job.primaryHeight = height;
  job.fetchSeriesMetadata = FsHelpers::hasEpubExtension(job.book.path) && !job.book.seriesMetadataScanned;
  const std::string wanted = UITheme::getCoverThumbPath(coverTemplateForBook(job.book.path), height);
  if (!cover_thumbnail::heightValid(wanted, height)) job.addTargetHeight(height);
  coverWorker.post(std::move(job));
}

void ReadingHubActivity::loop() {
  if (mappedInput.anyButtonDownRaw()) {
    lastInputMs = millis();
    coverWorker.requestCancel();
  }
  if (millis() - lastInputMs > 1000 && !mappedInput.anyButtonDownRaw() && !RenderLock::peek()) {
    stepVisibleCoverWorker();
  }
  if (page != Page::ROOT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCollection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      stepCollectionPage(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      stepCollectionPage(1);
      return;
    }
    const int visibleCount = page == Page::ALL_SHELVES ? collectionShelfCount : collectionBookCount;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      collectionSelectedIndex = reading_hub::stepRow(collectionSelectedIndex, -1, visibleCount);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      collectionSelectedIndex = reading_hub::stepRow(collectionSelectedIndex, 1, visibleCount);
      requestUpdate();
      return;
    }
    if (page == Page::ALL_SHELVES) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && collectionShelfCount > 0) {
        openShelfCollection(collectionShelves[collectionSelectedIndex]);
      }
      return;
    }
    if (const RecentBook* book = selectedCollectionBook()) {
      const auto action = pollLibraryBookInput(mappedInput, LONG_PRESS_MS, false);
      if (action == LibraryBookInputAction::ShowActions) {
        showBookActions(*book);
        return;
      }
      if (action == LibraryBookInputAction::OpenBook) {
        activityManager.goToReader(book->path, true);
        return;
      }
    }
    return;
  }

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
  if (page != Page::ROOT) {
    snprintf(title, sizeof(title), "%s", collectionTitle.c_str());
  } else if (menuOpen) {
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
        .page = page,
        .section = static_cast<uint8_t>(section),
        .selectedIndex = selectedIndex,
        .currentBook = hasCurrentBook ? &currentBook : nullptr,
        .currentProgress = currentProgress,
        .nextBook = queuePreviewCount > 0 ? &queueBooks[0] : nullptr,
        .shelves = shelves.data(),
        .shelfPreviewCount = shelfPreviewCount,
        .shelfTotalCount = shelfTotalCount,
        .shelfSummaryAvailable = shelfSummaryAvailable,
        .libraryCountKnown = libraryCountKnown,
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
            ratedBookCount > 0 ? static_cast<int>((ratingSum * 10u + ratedBookCount / 2u) / ratedBookCount) : 0,
        .collectionBooks = collectionBooks.data(),
        .collectionRatings = collectionRatings.data(),
        .collectionBookCount = collectionBookCount,
        .collectionShelves = page == Page::ALL_SHELVES ? collectionShelves.data() : nullptr,
        .collectionShelfCount = collectionShelfCount,
        .collectionSelectedIndex = collectionSelectedIndex,
        .collectionPageNumber = collectionPageNumber,
        .collectionPageCount = collectionPageCount()};
    GUI.drawReadingHubScreen(renderer, content, screen);
  }

  const bool emptyRootCollection = page == Page::ROOT && !menuOpen &&
                                   ((section == Section::LIBRARY && shelfPreviewCount == 0) ||
                                    (section == Section::QUEUE && queuePreviewCount == 0));
  const bool emptyDetailCollection =
      page != Page::ROOT && (page == Page::ALL_SHELVES ? collectionShelfCount : collectionBookCount) == 0;
  const char* confirmLabel = emptyRootCollection || emptyDetailCollection ? ""
                             : page == Page::ROOT && !menuOpen && section == Section::READ &&
                                     selectedIndex == reading_hub::readShowAllIndex(completedPreviewCount)
                                 ? tr(STR_HUB_SHOW_ALL)
                                 : tr(STR_OPEN);
  const char* backLabel = page != Page::ROOT || menuOpen ? tr(STR_BACK) : tr(STR_HUB_MENU);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(cleanInitialRefresh && firstPaint ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstPaint = false;
}
