#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class GfxRenderer;

namespace book_tile {

// Shared by every compact book card so series numbering and ratings cannot drift between the
// Reading Hub and the full Library.
void formatSeriesLabel(const std::string& series, uint16_t position, char* output, size_t outputSize);
void drawRatingStars(const GfxRenderer& renderer, int x, int y, uint8_t rating, bool black = true);

}  // namespace book_tile
