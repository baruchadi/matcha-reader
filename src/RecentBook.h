#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  std::string series;
  // Hundredths: 100 = book 1, 150 = book 1.5, zero = unspecified.
  uint16_t seriesPosition = 0;
  // Distinguishes an EPUB with no series from one whose metadata has not been
  // inspected yet, so non-series books are not reparsed on every Library visit.
  bool seriesMetadataScanned = false;

  RecentBook() = default;
  RecentBook(std::string pathValue, std::string titleValue, std::string authorValue, std::string coverBmpPathValue)
      : path(std::move(pathValue)),
        title(std::move(titleValue)),
        author(std::move(authorValue)),
        coverBmpPath(std::move(coverBmpPathValue)) {}

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

// Recents contains at most ten entries. Move them to the front of the scanned catalog in place
// instead of allocating another full catalog just to preserve recent-first ordering.
inline void mergeRecentBooks(std::vector<RecentBook>& catalog, std::vector<RecentBook> recents) {
  for (auto recent = recents.rbegin(); recent != recents.rend(); ++recent) {
    const auto scanned =
        std::find_if(catalog.begin(), catalog.end(), [&](const RecentBook& book) { return book.path == recent->path; });
    if (scanned == catalog.end()) {
      catalog.insert(catalog.begin(), std::move(*recent));
      continue;
    }
    if (!recent->title.empty()) scanned->title = std::move(recent->title);
    if (!recent->author.empty()) scanned->author = std::move(recent->author);
    if (!recent->coverBmpPath.empty()) scanned->coverBmpPath = std::move(recent->coverBmpPath);
    if (recent->seriesMetadataScanned) {
      scanned->series = std::move(recent->series);
      scanned->seriesPosition = recent->seriesPosition;
      scanned->seriesMetadataScanned = true;
    }
    std::rotate(catalog.begin(), scanned, scanned + 1);
  }
}

inline std::string bookTitleFromPath(const std::string_view path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string_view::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end = dot == std::string_view::npos || dot <= start ? path.size() : dot;
  return std::string(path.substr(start, end - start));
}

// Completion history is independent of the optional library scan cache. Add a lightweight
// fallback record for a finished path missing from that cache so "Show all completed" cannot
// shrink to whichever few completed books happened to be recent. A background library scan
// replaces the fallback title and cover with full metadata.
inline bool ensureBookPathInCatalog(std::vector<RecentBook>& catalog, const std::string& path) {
  if (path.empty() ||
      std::any_of(catalog.begin(), catalog.end(), [&](const RecentBook& book) { return book.path == path; })) {
    return false;
  }
  catalog.emplace_back(path, bookTitleFromPath(path), "", "");
  return true;
}
