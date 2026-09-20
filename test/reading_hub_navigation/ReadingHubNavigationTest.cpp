#include <gtest/gtest.h>

#include "ReadingHubNavigation.h"

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
