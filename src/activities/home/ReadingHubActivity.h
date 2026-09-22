#pragma once

#include <array>
#include <cstdint>

#include "ReadingHubNavigation.h"
#include "ReadingQueueStore.h"
#include "RecentBook.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

enum class BookAction : uint32_t;

class ReadingHubActivity final : public Activity {
 public:
  using Section = reading_hub::Section;

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

  Section section = Section::NOW;
  int selectedIndex = 0;
  bool menuOpen = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  bool firstPaint = true;
  RecentBook currentBook;
  bool hasCurrentBook = false;
  int currentProgress = -1;
  ReadingQueueStore queueStore;
  std::array<ReadingHubShelf, MAX_SHELF_PREVIEW> shelves;
  std::array<RecentBook, MAX_QUEUE_PREVIEW> queueBooks;
  std::array<RecentBook, MAX_COMPLETED_PREVIEW> completedBooks;
  std::array<std::string, MAX_COMPLETED_PREVIEW> completedPaths;
  std::array<uint8_t, MAX_COMPLETED_PREVIEW> completedPathRatings{};
  std::array<uint8_t, MAX_COMPLETED_PREVIEW> completedRatings{};
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
  bool shelvesLoaded = false;
  bool shelfSummaryAvailable = false;
  bool queueFullyLoaded = false;
  bool completedBooksLoaded = false;

  [[nodiscard]] const char* sectionLabel() const;
  [[nodiscard]] const char* menuItemLabel(int index) const;
  [[nodiscard]] int selectionCount() const;
  RecentBook resolveBook(const std::string& path) const;
  static void cacheCoverPath(RecentBook& book, int preferredHeight);
  static std::string cachedCoverPath(const std::string& templatePath, int preferredHeight);
  void loadShelves();
  void loadQueuePreview(int limit);
  void loadCompletionIndex();
  void loadCompletedBooks();
  void selectCurrentBook(const std::vector<uint8_t>& recentCompleted);
  void ensureSectionLoaded();
  void stepSection(int delta);
  void stepSelection(int delta);
  void activateSelection();
  [[nodiscard]] const RecentBook* selectedBook() const;
  void showBookActions(const RecentBook& book);
  void applyBookAction(BookAction action, const std::string& path, const std::string& title);
  void showBookStats(const std::string& path, const std::string& title);
  void showBookRating(const std::string& path, const std::string& title);
  void refreshAfterBookAction();
};
