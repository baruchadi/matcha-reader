#include <gtest/gtest.h>

#include "src/activities/reader/DeferredReaderOpenBookkeeping.h"
#include "src/activities/reader/ReaderPerformance.h"

TEST(ReaderOpenBookkeeping, RunsOnlyAfterTheFirstPageRenderAndOnlyOnce) {
  DeferredReaderOpenBookkeeping bookkeeping;
  int writes = 0;
  bookkeeping.arm();

  EXPECT_FALSE(bookkeeping.runIfReadyWhenIdle(5000, false, [&] { ++writes; }));
  bookkeeping.noteRenderResult(true, 1000);
  EXPECT_FALSE(bookkeeping.runIfReadyWhenIdle(1399, false, [&] { ++writes; }));
  EXPECT_FALSE(bookkeeping.runIfReadyWhenIdle(1400, true, [&] { ++writes; }));
  EXPECT_TRUE(bookkeeping.runIfReadyWhenIdle(1400, false, [&] { ++writes; }));
  EXPECT_EQ(writes, 1);
  EXPECT_FALSE(bookkeeping.runIfReadyWhenIdle(1800, false, [&] { ++writes; }));
}

TEST(ReaderOpenBookkeeping, ExitFlushesPendingWork) {
  DeferredReaderOpenBookkeeping bookkeeping;
  int writes = 0;
  bookkeeping.arm();

  EXPECT_TRUE(bookkeeping.runNowIfPending([&] { ++writes; }));
  EXPECT_EQ(writes, 1);
  EXPECT_FALSE(bookkeeping.runNowIfPending([&] { ++writes; }));
}

TEST(ReaderForegroundPriority, InputAndRenderingSuppressBackgroundWork) {
  EXPECT_TRUE(readerBackgroundWorkAllowed(false, false, false, false));
  EXPECT_FALSE(readerBackgroundWorkAllowed(true, false, false, false));
  EXPECT_FALSE(readerBackgroundWorkAllowed(false, true, false, false));
  EXPECT_FALSE(readerBackgroundWorkAllowed(false, false, true, false));
  EXPECT_FALSE(readerBackgroundWorkAllowed(false, false, false, true));
}
