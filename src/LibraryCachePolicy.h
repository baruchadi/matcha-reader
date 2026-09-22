#pragma once

#include <cstddef>
#include <string_view>

// One admission policy for the persisted Matcha Covers catalog. Reading Hub streams this file
// while CoverLibraryActivity loads it, so accepting even one different record would let a Hub
// shelf count a book that its destination cannot open until a background rescan completes.
namespace library_cache {
inline constexpr size_t MAX_BOOKS = 2048;
inline constexpr size_t MAX_PATH_LENGTH = 500;
inline constexpr size_t MAX_TITLE_LENGTH = 255;
inline constexpr size_t MAX_AUTHOR_LENGTH = 128;
inline constexpr size_t MAX_COVER_PATH_LENGTH = 600;
inline constexpr size_t MAX_SERIES_LENGTH = 128;
inline constexpr size_t MAX_RECORD_BYTES = 4096;
// Optional enrichment must never dominate an e-ink page transition. Structural catalog data
// lives in CLX, so a bounded 512 KiB cache is preferable to multi-megabyte rescans.
inline constexpr size_t MAX_FILE_BYTES = 512 * 1024;

inline bool admitsPath(const std::string_view path) { return !path.empty() && path.size() <= MAX_PATH_LENGTH; }
}  // namespace library_cache
