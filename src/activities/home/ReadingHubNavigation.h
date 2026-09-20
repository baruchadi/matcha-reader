#pragma once

#include <cstdint>

namespace reading_hub {

enum class Section : uint8_t { NOW = 0, LIBRARY = 1, QUEUE = 2, READ = 3 };

constexpr int SECTION_COUNT = 4;

constexpr Section stepSection(const Section current, const int delta) {
  const int value = static_cast<int>(current);
  return static_cast<Section>((value + delta % SECTION_COUNT + SECTION_COUNT) % SECTION_COUNT);
}

constexpr int stepRow(const int current, const int delta, const int rowCount) {
  if (rowCount <= 0) return 0;
  return (current + delta % rowCount + rowCount) % rowCount;
}

}  // namespace reading_hub
