#pragma once

#include "components/themes/lyra/LyraTheme.h"

namespace ReadingHubMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics value = LyraMetrics::values;
  value.headerHeight = 56;
  value.headerUnderlineSize = 2;
  value.headerTitleAlign = 1;
  value.headerBatteryDetached = false;
  value.batteryBarHeight = 56;
  value.contentSidePadding = 18;
  value.buttonHintsHeight = 40;
  return value;
}();
}  // namespace ReadingHubMetrics

class ReadingHubTheme final : public LyraTheme {
 public:
  RootExperience rootExperience() const override { return RootExperience::READING_HUB; }
  void drawReadingHubScreen(const GfxRenderer& renderer, Rect rect, const ReadingHubScreen& screen) const override;
};
