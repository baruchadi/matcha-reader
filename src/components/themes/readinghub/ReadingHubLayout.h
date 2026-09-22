#pragma once

#include <algorithm>

namespace reading_hub {

constexpr int selectionBorderWidth(const bool selected) { return selected ? 4 : 2; }

// Reading Hub never changes text/cover polarity to show focus. A heavier card border keeps
// selection consistent without turning detailed e-ink artwork into a black rectangle.
constexpr bool selectionInkIsBlack(const bool) { return true; }

struct NowLayout {
  int continueCardHeight;
  int queueCardHeight;
  int completedPreviewHeight;
};

constexpr NowLayout makeNowLayout(const int contentHeight, const int headingLineHeight) {
  constexpr int CONTINUE_TOP_GAP = 8;
  constexpr int QUEUE_GAP = 12;
  constexpr int COMPLETED_GAP = 14;
  constexpr int COMPLETED_HEADER_HEIGHT = 32;
  const int continueHeight = std::clamp(contentHeight * 36 / 100, 150, 244);
  const int queueHeight = std::clamp(contentHeight * 14 / 100, 72, 92);
  const int used = headingLineHeight + CONTINUE_TOP_GAP + continueHeight + QUEUE_GAP + queueHeight + COMPLETED_GAP +
                   COMPLETED_HEADER_HEIGHT;
  return {continueHeight, queueHeight, std::max(1, contentHeight - used)};
}

}  // namespace reading_hub
