#include "ReadingHubTheme.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "fontIds.h"

namespace {
constexpr int ROW_GAP = 12;
constexpr int ROW_RADIUS = 5;
constexpr int ROW_SIDE_PADDING = 16;
constexpr int TITLE_TOP = 15;
constexpr int SUBTITLE_TOP = 49;
}  // namespace

void ReadingHubTheme::drawReadingHubRows(const GfxRenderer& renderer, const Rect rect, const ReadingHubRow* rows,
                                         const int rowCount, const int selectedIndex) const {
  if (!rows || rowCount <= 0 || rect.width <= 0 || rect.height <= 0) return;

  const int availableHeight = std::max(0, rect.height - ROW_GAP * (rowCount - 1));
  const int rowHeight = std::min(116, availableHeight / rowCount);
  const int usedHeight = rowHeight * rowCount + ROW_GAP * (rowCount - 1);
  int y = rect.y + std::max(0, (rect.height - usedHeight) / 2);

  for (int index = 0; index < rowCount; ++index) {
    const bool selected = index == selectedIndex;
    if (selected) {
      renderer.fillRoundedRect(rect.x, y, rect.width, rowHeight, ROW_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(rect.x, y, rect.width, rowHeight, 2, ROW_RADIUS, true, true, true, true, true);
    }

    const int textWidth = rect.width - ROW_SIDE_PADDING * 2 - 20;
    const auto title = renderer.truncatedText(UI_12_FONT_ID, rows[index].title ? rows[index].title : "", textWidth,
                                              EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + ROW_SIDE_PADDING, y + TITLE_TOP, title.c_str(), !selected,
                      EpdFontFamily::BOLD);

    if (rows[index].subtitle && rows[index].subtitle[0] != '\0') {
      const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, rows[index].subtitle, textWidth);
      renderer.drawText(SMALL_FONT_ID, rect.x + ROW_SIDE_PADDING, y + SUBTITLE_TOP, subtitle.c_str(), !selected);
    }

    const int arrowX = rect.x + rect.width - ROW_SIDE_PADDING - 8;
    const int arrowY = y + rowHeight / 2;
    renderer.drawLine(arrowX - 5, arrowY - 6, arrowX + 1, arrowY, !selected);
    renderer.drawLine(arrowX + 1, arrowY, arrowX - 5, arrowY + 6, !selected);
    y += rowHeight + ROW_GAP;
  }
}
