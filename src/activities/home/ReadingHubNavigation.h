#pragma once

#include <cstdint>

#include "components/ReadingHubModel.h"

namespace reading_hub {

constexpr int SECTION_COUNT = 4;

constexpr Section stepSection(const Section current, const int delta) {
  const int value = static_cast<int>(current);
  return static_cast<Section>((value + delta % SECTION_COUNT + SECTION_COUNT) % SECTION_COUNT);
}

constexpr int stepRow(const int current, const int delta, const int rowCount) {
  if (rowCount <= 0) return 0;
  return (current + delta % rowCount + rowCount) % rowCount;
}

// The completed-book grid is the primary content. "Show all" follows the final
// preview rather than displacing the newest book at index zero.
constexpr int readSelectionCount(const int previewCount) { return (previewCount > 0 ? previewCount : 0) + 1; }

constexpr int readShowAllIndex(const int previewCount) { return previewCount > 0 ? previewCount : 0; }

constexpr int readBookIndex(const int selectedIndex, const int previewCount) {
  return selectedIndex >= 0 && selectedIndex < previewCount ? selectedIndex : -1;
}

constexpr Section parentSection(const Page page) {
  return page == Page::COMPLETED_BOOKS ? Section::READ : Section::LIBRARY;
}

constexpr int collectionPageCount(const int itemCount, const int pageSize) {
  return pageSize > 0 && itemCount > 0 ? (itemCount + pageSize - 1) / pageSize : 1;
}

constexpr int stepCollectionPage(const int page, const int delta, const int itemCount, const int pageSize) {
  const int pages = collectionPageCount(itemCount, pageSize);
  return (page + delta % pages + pages) % pages;
}

template <typename IsAvailable>
int collectAvailableIndices(const int candidateCount, const int limit, uint8_t* output, IsAvailable&& isAvailable) {
  if (!output || limit <= 0) return 0;
  int count = 0;
  for (int index = 0; index < candidateCount && count < limit; index++) {
    if (isAvailable(index)) output[count++] = static_cast<uint8_t>(index);
  }
  return count;
}

}  // namespace reading_hub
