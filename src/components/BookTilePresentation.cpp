#include "BookTilePresentation.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "SeriesMetadata.h"

namespace {
void drawStar(const GfxRenderer& renderer, const int centerX, const int centerY, const bool filled,
              const bool black) {
  static constexpr int REL_X[10] = {0, 2, 6, 3, 4, 0, -4, -3, -6, -2};
  static constexpr int REL_Y[10] = {-6, -2, -2, 1, 5, 3, 5, 1, -2, -2};
  int xs[10];
  int ys[10];
  for (int index = 0; index < 10; index++) {
    xs[index] = centerX + REL_X[index];
    ys[index] = centerY + REL_Y[index];
  }
  if (filled) {
    renderer.fillPolygon(xs, ys, 10, black);
    return;
  }
  for (int index = 0; index < 10; index++) {
    renderer.drawLine(xs[index], ys[index], xs[(index + 1) % 10], ys[(index + 1) % 10], black);
  }
}
}  // namespace

namespace book_tile {
void formatSeriesLabel(const std::string& series, const uint16_t position, char* output, const size_t outputSize) {
  if (!output || outputSize == 0) return;
  output[0] = '\0';
  if (series.empty()) return;
  if (position == 0) {
    snprintf(output, outputSize, "%s", series.c_str());
    return;
  }
  char positionText[12];
  series_metadata::formatPosition(position, positionText, sizeof(positionText));
  snprintf(output, outputSize, tr(STR_SERIES_BOOK_FORMAT), series.c_str(), positionText);
}

void drawRatingStars(const GfxRenderer& renderer, const int x, const int y, const uint8_t rating, const bool black) {
  for (int star = 0; star < 5; star++) drawStar(renderer, x + star * 16 + 6, y + 6, star < rating, black);
}
}  // namespace book_tile
