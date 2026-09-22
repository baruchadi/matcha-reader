#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class GfxRenderer;

namespace book_tile {

constexpr int COMPLETED_METADATA_LINES = 3;

struct CoverSize {
  int width;
  int height;
};

constexpr CoverSize fitTwoByThreeCover(const int tileWidth, const int tileHeight, const int metadataHeight,
                                       const int padding) {
  const int availableWidth = tileWidth - padding * 2 > 1 ? tileWidth - padding * 2 : 1;
  const int availableHeight = tileHeight - metadataHeight > 1 ? tileHeight - metadataHeight : 1;
  const int naturalHeight = availableWidth * 3 / 2;
  const int height = availableHeight < naturalHeight ? availableHeight : naturalHeight;
  return {height * 2 / 3, height};
}

// Shared by every compact book card so series numbering and ratings cannot drift between the
// Reading Hub and the full Library.
void formatSeriesLabel(const std::string& series, uint16_t position, char* output, size_t outputSize);
void drawRatingStars(const GfxRenderer& renderer, int x, int y, uint8_t rating, bool black = true);

}  // namespace book_tile
