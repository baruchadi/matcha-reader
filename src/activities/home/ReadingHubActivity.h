#pragma once

#include <array>
#include <cstdint>

#include "ReadingHubNavigation.h"
#include "ReadingQueueStore.h"
#include "RecentBook.h"
#include "activities/Activity.h"

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
  static constexpr int MAX_LIBRARY_PREVIEW = 6;
  static constexpr int MAX_QUEUE_PREVIEW = 5;
  static constexpr int MAX_COMPLETED_PREVIEW = 6;

  Section section = Section::NOW;
  int selectedIndex = 0;
  bool menuOpen = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  bool firstPaint = true;
  const RecentBook* currentBook = nullptr;
  int currentProgress = -1;
  ReadingQueueStore queueStore;
  std::array<RecentBook, MAX_LIBRARY_PREVIEW> libraryBooks;
  std::array<RecentBook, MAX_QUEUE_PREVIEW> queueBooks;
  std::array<RecentBook, MAX_COMPLETED_PREVIEW> completedBooks;
  std::array<uint8_t, MAX_COMPLETED_PREVIEW> completedRatings{};
  int libraryPreviewCount = 0;
  int queuePreviewCount = 0;
  int completedPreviewCount = 0;
  uint16_t libraryBookCount = 0;
  uint16_t completedBookCount = 0;
  uint16_t ratedBookCount = 0;
  uint32_t ratingSum = 0;
  bool libraryCountKnown = false;

  [[nodiscard]] const char* sectionLabel() const;
  [[nodiscard]] const char* menuItemLabel(int index) const;
  [[nodiscard]] int selectionCount() const;
  [[nodiscard]] bool isInCompletedPreview(const std::string& path) const;
  RecentBook resolveBook(const std::string& path) const;
  void loadLibraryPreview();
  void loadQueuePreview();
  void loadCompletedPreview();
  void stepSection(int delta);
  void stepSelection(int delta);
  void activateSelection();
};
