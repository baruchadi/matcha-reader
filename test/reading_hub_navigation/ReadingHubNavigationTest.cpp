#include <gtest/gtest.h>

#include <cstdlib>

#include "ReadingHubNavigation.h"
#include "src/components/BookTilePresentation.h"

using reading_hub::Section;

TEST(ReadingHubNavigationTest, SideButtonsCycleRootSections) {
  EXPECT_EQ(reading_hub::stepSection(Section::NOW, 1), Section::LIBRARY);
  EXPECT_EQ(reading_hub::stepSection(Section::LIBRARY, 1), Section::QUEUE);
  EXPECT_EQ(reading_hub::stepSection(Section::QUEUE, 1), Section::READ);
  EXPECT_EQ(reading_hub::stepSection(Section::READ, 1), Section::NOW);
  EXPECT_EQ(reading_hub::stepSection(Section::NOW, -1), Section::READ);
}

TEST(ReadingHubNavigationTest, FrontButtonsWrapWithinSection) {
  EXPECT_EQ(reading_hub::stepRow(0, 1, 3), 1);
  EXPECT_EQ(reading_hub::stepRow(2, 1, 3), 0);
  EXPECT_EQ(reading_hub::stepRow(0, -1, 3), 2);
  EXPECT_EQ(reading_hub::stepRow(0, 1, 0), 0);
}

TEST(ReadingHubNavigationTest, CompletedBooksComeBeforeShowAll) {
  EXPECT_EQ(reading_hub::readSelectionCount(0), 1);
  EXPECT_EQ(reading_hub::readShowAllIndex(0), 0);
  EXPECT_EQ(reading_hub::readSelectionCount(6), 7);
  EXPECT_EQ(reading_hub::readBookIndex(0, 6), 0);
  EXPECT_EQ(reading_hub::readBookIndex(5, 6), 5);
  EXPECT_EQ(reading_hub::readBookIndex(6, 6), -1);
  EXPECT_EQ(reading_hub::readShowAllIndex(6), 6);
}

TEST(ReadingHubNavigationTest, StaleCompletionsDoNotConsumeVisiblePreviewSlots) {
  constexpr bool PRESENT[] = {false, true, false, true, true, true, true};
  uint8_t indices[3] = {};

  const int count = reading_hub::collectAvailableIndices(7, 3, indices, [](const int index) { return PRESENT[index]; });

  ASSERT_EQ(count, 3);
  EXPECT_EQ(indices[0], 1);
  EXPECT_EQ(indices[1], 3);
  EXPECT_EQ(indices[2], 4);
}

TEST(ReadingHubNavigationTest, ShortCompletedTilesShrinkCoverWithoutCroppingItsAspect) {
  const book_tile::CoverSize cover = book_tile::fitTwoByThreeCover(141, 174, 63, 5);

  EXPECT_LT(cover.width, 131);
  EXPECT_LE(cover.height, 174 - 63);
  EXPECT_LE(std::abs(cover.width * 3 - cover.height * 2), 2);
}
