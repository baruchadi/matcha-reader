#include <gtest/gtest.h>

#include "src/LibraryCachePolicy.h"
#include "src/RecentBook.h"
#include "src/SeriesMetadata.h"

TEST(RecentBooksMerge, KeepsRecentsFirstWithoutDuplicatesOrLosingScannedCovers) {
  std::vector<RecentBook> catalog{{"/a.epub", "Scanned A", "", "cover-a"}, {"/b.epub", "Scanned B", "", "cover-b"}};
  std::vector<RecentBook> recents{{"/b.epub", "Recent B", "Author B", ""}, {"/new.epub", "New", "", "cover-new"}};

  mergeRecentBooks(catalog, std::move(recents));

  ASSERT_EQ(catalog.size(), 3u);
  EXPECT_EQ(catalog[0].path, "/b.epub");
  EXPECT_EQ(catalog[0].title, "Recent B");
  EXPECT_EQ(catalog[0].author, "Author B");
  EXPECT_EQ(catalog[0].coverBmpPath, "cover-b");
  EXPECT_EQ(catalog[1].path, "/new.epub");
  EXPECT_EQ(catalog[2].path, "/a.epub");
}

TEST(LibraryCachePolicy, SharesExactCatalogAndPathBoundaries) {
  EXPECT_EQ(library_cache::MAX_BOOKS, 2048u);
  EXPECT_FALSE(library_cache::admitsPath(""));
  EXPECT_TRUE(library_cache::admitsPath(std::string(library_cache::MAX_PATH_LENGTH, 'a')));
  EXPECT_FALSE(library_cache::admitsPath(std::string(library_cache::MAX_PATH_LENGTH + 1, 'a')));
}

TEST(RecentBooksMerge, KeepsScannedSeriesMetadataWhenRecentEntryIsOlder) {
  RecentBook scanned{"/a.epub", "A", "", "cover"};
  scanned.series = "Saga";
  scanned.seriesPosition = 200;
  scanned.seriesMetadataScanned = true;
  std::vector<RecentBook> catalog{scanned};
  std::vector<RecentBook> recents{{"/a.epub", "Recent A", "", ""}};

  mergeRecentBooks(catalog, std::move(recents));

  ASSERT_EQ(catalog.size(), 1u);
  EXPECT_EQ(catalog[0].series, "Saga");
  EXPECT_EQ(catalog[0].seriesPosition, 200);
  EXPECT_TRUE(catalog[0].seriesMetadataScanned);
}

TEST(SeriesMetadata, ParsesAndFormatsCompactDecimalPositions) {
  EXPECT_EQ(series_metadata::parsePosition("2"), 200);
  EXPECT_EQ(series_metadata::parsePosition(" 1.5 "), 150);
  EXPECT_EQ(series_metadata::parsePosition("0.25"), 25);
  EXPECT_EQ(series_metadata::parsePosition("volume two"), 0);

  char value[12];
  series_metadata::formatPosition(200, value, sizeof(value));
  EXPECT_STREQ(value, "2");
  series_metadata::formatPosition(150, value, sizeof(value));
  EXPECT_STREQ(value, "1.5");
  series_metadata::formatPosition(25, value, sizeof(value));
  EXPECT_STREQ(value, "0.25");
}

TEST(SeriesMetadata, GroupsSeriesAtFirstAppearanceAndOrdersVolumes) {
  std::vector<RecentBook> books{{"/single-a", "Single A", "", ""},
                                {"/saga-2", "Saga Two", "", ""},
                                {"/single-b", "Single B", "", ""},
                                {"/saga-1", "Saga One", "", ""},
                                {"/other", "Other", "", ""}};
  books[1].series = "Saga";
  books[1].seriesPosition = 200;
  books[3].series = "Saga";
  books[3].seriesPosition = 100;
  std::vector<uint16_t> indices{0, 1, 2, 3, 4};

  series_metadata::groupIndices(books, indices);

  EXPECT_EQ(indices, (std::vector<uint16_t>{0, 3, 1, 2, 4}));
}
