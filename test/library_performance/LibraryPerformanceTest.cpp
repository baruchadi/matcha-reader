#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "src/LibraryPerformance.h"

TEST(LibraryWindow, BoundsWorkToTheVisibleRows) {
  const LibraryBookWindow window = makeLibraryBookWindow(2048, 0, 2, 3);

  EXPECT_EQ(window.first, 0u);
  EXPECT_EQ(window.renderedEnd, 9u);
  EXPECT_EQ(window.titledEnd, 6u);
}

TEST(LibraryScanning, IgnoresLegacyDeviceCacheDirectories) {
  EXPECT_TRUE(libraryScanIgnoresEntry("XTCache"));
  EXPECT_FALSE(libraryScanIgnoresEntry("Fiction"));
}

namespace {
struct CatalogEntry {
  std::string path;
};

std::string_view catalogPath(const CatalogEntry& entry) { return entry.path; }
}  // namespace

TEST(LibraryCatalogLookup, FindsEveryBookWithLogarithmicProbeGrowth) {
  constexpr size_t bookCount = 2048;
  std::vector<CatalogEntry> books;
  books.reserve(bookCount);
  for (size_t i = 0; i < bookCount; ++i) {
    books.push_back({"/Books/Shared Prefix/Book " + std::to_string(100000 + i) + ".epub"});
  }

  SortedLibraryLookup<CatalogEntry, std::string_view> lookup;
  lookup.reset(books, &catalogPath);
  LibraryLookupStats stats;
  for (const auto& book : books) {
    const auto* found = lookup.find(book.path, &stats);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->path, book.path);
  }

  EXPECT_LE(stats.probes, bookCount * 12);
  const size_t oldLinearProbes = bookCount * (bookCount + 1) / 2;
  EXPECT_LT(stats.probes * 80, oldLinearProbes);
}

TEST(LibraryCatalogLookup, ReleasesItsTransientIndexBeforeCoverDecoding) {
  std::vector<CatalogEntry> books{{"/Books/A.epub"}, {"/Books/B.epub"}};
  SortedLibraryLookup<CatalogEntry, std::string_view> lookup;
  lookup.reset(books, &catalogPath);

  ASSERT_NE(lookup.find(books.front().path), nullptr);
  lookup.release();

  EXPECT_EQ(lookup.find(books.front().path), nullptr);
  EXPECT_EQ(lookup.indexEntryCount(), 0u);
}

TEST(LibraryShelfGrouping, BuildsOneStableFolderOrderWithoutQuadraticSearches) {
  std::vector<CatalogEntry> books{{"/Books/Z/Second.epub"},
                                  {"/Root.epub"},
                                  {"/Books/A/First.epub"},
                                  {"/Books/Z/First.epub"},
                                  {"relative.epub"}};
  LibraryFolderOrderStats stats;

  const auto order = makeLibraryFolderOrder(books, &catalogPath, &stats);

  ASSERT_EQ(order.size(), books.size());
  EXPECT_EQ(order, (std::vector<uint16_t>{1, 4, 2, 0, 3}));
  EXPECT_LE(stats.pathReads, stats.comparisons * 2);
}

TEST(LibraryProgressWarmup, AMonotonicCursorExaminesEachBookAtMostOnce) {
  std::vector<int> progress(2048, -1);
  progress[4] = -2;
  progress[2000] = -2;
  size_t cursor = 0;
  size_t inspected = 0;

  const auto isPending = [](const int percent) { return percent == -2; };
  EXPECT_EQ(findNextPendingLibraryProgress(progress, cursor, isPending, &inspected), 4u);
  progress[4] = 25;
  EXPECT_EQ(findNextPendingLibraryProgress(progress, cursor, isPending, &inspected), 2000u);
  progress[2000] = 80;
  EXPECT_EQ(findNextPendingLibraryProgress(progress, cursor, isPending, &inspected), progress.size());
  EXPECT_LE(inspected, progress.size());
}
