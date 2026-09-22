#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "RecentBook.h"

namespace series_metadata {

// Fixed-point hundredths keep sorting compact and deterministic without pulling
// floating-point parsing into the library scan. Zero means no usable position.
inline uint16_t parsePosition(const std::string_view text) {
  size_t at = 0;
  while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) at++;

  uint32_t whole = 0;
  bool haveDigit = false;
  while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
    haveDigit = true;
    whole = whole * 10u + static_cast<unsigned>(text[at] - '0');
    if (whole > 655u) return 0;
    at++;
  }

  uint32_t fraction = 0;
  if (at < text.size() && text[at] == '.') {
    at++;
    if (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
      fraction = static_cast<unsigned>(text[at++] - '0') * 10u;
      if (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
        fraction += static_cast<unsigned>(text[at++] - '0');
      }
      while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) at++;
    }
  }
  while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) at++;
  if (!haveDigit || at != text.size()) return 0;

  const uint32_t fixed = whole * 100u + fraction;
  return fixed > UINT16_MAX ? 0 : static_cast<uint16_t>(fixed);
}

inline void formatPosition(const uint16_t position, char* output, const size_t outputSize) {
  if (!output || outputSize == 0) return;
  if (position == 0) {
    output[0] = '\0';
  } else if (position % 100u == 0) {
    snprintf(output, outputSize, "%u", static_cast<unsigned>(position / 100u));
  } else if (position % 10u == 0) {
    snprintf(output, outputSize, "%u.%u", static_cast<unsigned>(position / 100u),
             static_cast<unsigned>((position % 100u) / 10u));
  } else {
    snprintf(output, outputSize, "%u.%02u", static_cast<unsigned>(position / 100u),
             static_cast<unsigned>(position % 100u));
  }
}

inline bool sameSeries(const RecentBook& left, const RecentBook& right) {
  return !left.series.empty() && left.series == right.series;
}

inline bool comesBeforeInSeries(const RecentBook& left, const RecentBook& right) {
  if (left.seriesPosition == 0) return false;
  if (right.seriesPosition == 0) return true;
  return left.seriesPosition < right.seriesPosition;
}

// Move every later member beside the first visible member, then order that run
// by volume. This preserves the surrounding recency order instead of turning the
// entire Active view into an alphabetical list.
inline void groupIndices(const std::vector<RecentBook>& books, std::vector<uint16_t>& indices) {
  for (size_t first = 0; first < indices.size(); first++) {
    if (indices[first] >= books.size() || books[indices[first]].series.empty()) continue;
    size_t groupedEnd = first + 1;
    for (size_t candidate = groupedEnd; candidate < indices.size(); candidate++) {
      if (indices[candidate] >= books.size() || !sameSeries(books[indices[first]], books[indices[candidate]])) continue;
      std::rotate(indices.begin() + groupedEnd, indices.begin() + candidate, indices.begin() + candidate + 1);
      groupedEnd++;
    }
    // Insertion sort keeps unknown positions stable and uses no temporary heap
    // buffer (std::stable_sort may allocate one).
    for (size_t current = first + 1; current < groupedEnd; current++) {
      const uint16_t value = indices[current];
      size_t insertAt = current;
      while (insertAt > first && comesBeforeInSeries(books[value], books[indices[insertAt - 1]])) {
        indices[insertAt] = indices[insertAt - 1];
        insertAt--;
      }
      indices[insertAt] = value;
    }
    first = groupedEnd - 1;
  }
}

}  // namespace series_metadata
