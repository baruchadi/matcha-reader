#pragma once

#include <array>
#include <cstdint>

#include "ReadingHubNavigation.h"
#include "ReadingQueueStore.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

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
  static constexpr int MAX_ROWS = 4;

  Section section = Section::NOW;
  int selectedRow = 0;
  bool menuOpen = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;
  bool firstPaint = true;
  const RecentBook* currentBook = nullptr;
  ReadingQueueStore queueStore;
  uint16_t libraryBookCount = 0;
  uint16_t completedBookCount = 0;
  bool libraryCountKnown = false;

  struct RowText {
    char currentStatus[96]{};
    char libraryStatus[40]{};
    char queueStatus[40]{};
    char completedStatus[40]{};
  };

  int buildRows(std::array<ReadingHubRow, MAX_ROWS>& rows, RowText& text) const;
  const char* sectionLabel() const;
  void stepSection(int delta);
  void stepRow(int delta);
  void activateSelection();
};
