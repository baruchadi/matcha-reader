#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "CoverThumbnailWorker.h"
#include "ReadingHubNavigation.h"
#include "ReadingQueueStore.h"
#include "RecentBook.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

enum class BookAction : uint32_t;
namespace library {
class LibraryIndexFile;
}

class ReadingHubActivity final : public Activity {
 public:
  using Section = reading_hub::Section;
  using Page = reading_hub::Page;

  explicit ReadingHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  static constexpr int SECTION_COUNT = reading_hub::SECTION_COUNT;
  static constexpr int MENU_ITEM_COUNT = 4;
  static constexpr int MAX_SHELF_PREVIEW = 7;
  static constexpr int MAX_QUEUE_PREVIEW = 5;
  static constexpr int MAX_COMPLETED_PREVIEW = 6;
  static constexpr int MAX_NOW_COMPLETED_PREVIEW = 3;
  static constexpr int MAX_COMPLETED_CANDIDATES = MAX_COMPLETED_PREVIEW * 2;
  static constexpr int COLLECTION_BOOKS_PER_PAGE = 6;
  static constexpr int COLLECTION_SHELVES_PER_PAGE = 7;

  Section section = Section::NOW;
  Page page = Page::ROOT;
  int selectedIndex = 0;
  bool menuOpen = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  bool firstPaint = true;
  RecentBook currentBook;
  bool hasCurrentBook = false;
  int currentProgress = -1;
  ReadingQueueStore queueStore;
  // Only the visible shelf page lives in RAM. The complete set stays in the
  // compact on-card index, so "Show all" never becomes a silent fixed cap.
  std::array<ReadingHubShelf, MAX_SHELF_PREVIEW> shelves;
  std::array<ReadingHubShelf, COLLECTION_SHELVES_PER_PAGE> collectionShelves;
  std::array<RecentBook, MAX_QUEUE_PREVIEW> queueBooks;
  std::array<RecentBook, MAX_COMPLETED_PREVIEW> completedBooks;
  std::array<std::string, MAX_COMPLETED_CANDIDATES> completedPaths;
  std::array<uint8_t, MAX_COMPLETED_CANDIDATES> completedPathRatings{};
  std::array<uint8_t, MAX_COMPLETED_PREVIEW> completedRatings{};
  std::unique_ptr<uint64_t[]> completedHashes;
  uint16_t completedHashCount = 0;
  bool completedHashesValidated = false;
  // Collection screens retain only the visible page; opening a 500-book completion history does
  // not retain 500 paths/covers in DRAM.
  std::array<RecentBook, COLLECTION_BOOKS_PER_PAGE> collectionBooks;
  std::array<uint8_t, COLLECTION_BOOKS_PER_PAGE> collectionRatings{};
  std::string collectionTitle;
  std::string collectionShelfPath;
  uint16_t collectionShelfId = UINT16_MAX;
  int collectionBookCount = 0;
  int collectionShelfCount = 0;
  int collectionTotalCount = 0;
  int collectionSelectedIndex = 0;
  int collectionPageNumber = 0;
  Page collectionParentPage = Page::ROOT;
  int collectionParentPageNumber = 0;
  int collectionParentSelectedIndex = 0;
  int shelfPreviewCount = 0;
  int shelfTotalCount = 0;
  int queuePreviewCount = 0;
  int completedPathCount = 0;
  int completedPreviewCount = 0;
  uint16_t libraryBookCount = 0;
  uint16_t completedBookCount = 0;
  uint16_t ratedBookCount = 0;
  uint32_t ratingSum = 0;
  bool libraryCountKnown = false;
  bool indexBuildAttempted = false;
  bool collectionTotalKnown = false;
  bool shelvesLoaded = false;
  bool shelfSummaryAvailable = false;
  bool queueFullyLoaded = false;
  int completedBooksLoadLimit = 0;
  CoverThumbnailWorker coverWorker;
  uint32_t lastInputMs = 0;
  std::array<uint64_t, 32> coverAttempts{};
  int coverAttemptCount = 0;

  [[nodiscard]] const char* sectionLabel() const;
  [[nodiscard]] const char* menuItemLabel(int index) const;
  [[nodiscard]] int selectionCount() const;
  RecentBook resolveBook(const std::string& path) const;
  static void cacheCoverPath(RecentBook& book, int preferredHeight);
  static std::string cachedCoverPath(const std::string& templatePath, int preferredHeight);
  [[nodiscard]] bool openCatalogIndex(library::LibraryIndexFile& index);
  [[nodiscard]] RecentBook readCatalogBook(library::LibraryIndexFile& index, uint16_t ordinal,
                                           const std::string* knownFolderPath = nullptr) const;
  void enrichBooksFromIndex(RecentBook* books, int count) const;
  void enrichBooksFromCache(RecentBook* books, int count) const;
  void hydrateBookPage(RecentBook* books, int count) const;
  [[nodiscard]] bool loadShelfPage(int pageNumber, std::array<ReadingHubShelf, MAX_SHELF_PREVIEW>& output,
                                   int& visibleCount, int& totalCount);
  void loadShelves();
  void loadQueuePreview(int limit);
  void loadCompletionIndex();
  void loadCompletedBooks(int limit);
  void selectCurrentBook(const std::vector<uint8_t>& recentCompleted);
  void ensureSectionLoaded();
  void stepSection(int delta);
  void stepSelection(int delta);
  void activateSelection();
  void openShelfCollection(const ReadingHubShelf& shelf);
  void openAllShelvesCollection();
  void openCompletedCollection();
  void closeCollection();
  void stepCollectionPage(int delta);
  void loadCollectionPage();
  void loadShelfCollectionPage();
  void loadCompletedCollectionPage();
  [[nodiscard]] int collectionPageCount() const;
  [[nodiscard]] const RecentBook* selectedCollectionBook() const;
  [[nodiscard]] const RecentBook* selectedBook() const;
  void showBookActions(const RecentBook& book);
  void applyBookAction(BookAction action, const std::string& path, const std::string& title);
  void showBookStats(const std::string& path, const std::string& title);
  void showBookRating(const std::string& path, const std::string& title);
  void refreshAfterBookAction();
  void stepVisibleCoverWorker();
  void publishCoverResult(const RecentBook& book);
  [[nodiscard]] bool nextVisibleCoverTarget(RecentBook& book, int& height) const;
  bool skipLoopDelay() override { return coverWorker.busy(); }
};
