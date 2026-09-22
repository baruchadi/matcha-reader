#pragma once

#include <cstddef>
#include <string_view>

// One admission policy for the persisted Matcha Covers catalog. Reading Hub streams this file
// while CoverLibraryActivity loads it, so accepting even one different record would let a Hub
// shelf count a book that its destination cannot open until a background rescan completes.
namespace library_cache {
inline constexpr size_t MAX_BOOKS = 2048;
inline constexpr size_t MAX_PATH_LENGTH = 500;

inline bool admitsPath(const std::string_view path) {
  return !path.empty() && path.size() <= MAX_PATH_LENGTH;
}
}  // namespace library_cache
