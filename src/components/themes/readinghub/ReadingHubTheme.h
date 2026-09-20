#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Reading Hub keeps Lyra's compact component language outside Home while its
// dedicated root activity presents Now, Library, Queue, and Read as first-class
// sections.
namespace ReadingHubMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics value = LyraMetrics::values;
  value.headerHeight = 56;
  value.headerUnderlineSize = 2;
  value.headerTitleAlign = 1;
  value.headerBatteryDetached = false;
  value.batteryBarHeight = 56;
  value.buttonHintsHeight = 40;
  return value;
}();
}  // namespace ReadingHubMetrics

class ReadingHubTheme final : public LyraTheme {
 public:
  RootExperience rootExperience() const override { return RootExperience::READING_HUB; }
  void drawReadingHubRows(const GfxRenderer& renderer, Rect rect, const ReadingHubRow* rows, int rowCount,
                          int selectedIndex) const override;
};
