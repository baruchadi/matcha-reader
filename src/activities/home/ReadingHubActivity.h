#pragma once

#include <array>
#include <cstdint>

#include "ReadingQueueStore.h"
#include "ReadingHubNavigation.h"
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

  int buildRows(std::array<ReadingHubRow, MAX_ROWS>& rows, char* queueStatus, size_t queueStatusSize) const;
  const char* sectionLabel() const;
  void stepSection(int delta);
  void stepRow(int delta);
  void activateSelection();
};
