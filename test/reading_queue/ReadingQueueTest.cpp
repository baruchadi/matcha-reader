#include <gtest/gtest.h>

#include <string>

#include "ReadingQueue.h"

TEST(ReadingQueueTest, PreservesExplicitOrderAndMovesOneSlot) {
  ReadingQueue queue;
  ASSERT_TRUE(queue.add("/a.epub"));
  ASSERT_TRUE(queue.add("/b.epub"));
  ASSERT_TRUE(queue.add("/c.epub"));

  EXPECT_TRUE(queue.moveEarlier("/c.epub"));
  EXPECT_EQ(queue.paths()[0], "/a.epub");
  EXPECT_EQ(queue.paths()[1], "/c.epub");
  EXPECT_EQ(queue.paths()[2], "/b.epub");
  EXPECT_TRUE(queue.moveLater("/a.epub"));
  EXPECT_EQ(queue.paths()[0], "/c.epub");
  EXPECT_EQ(queue.paths()[1], "/a.epub");
}

TEST(ReadingQueueTest, DuplicateAndBoundaryOperationsAreNoOps) {
  ReadingQueue queue;
  ASSERT_TRUE(queue.add("/a.epub"));
  EXPECT_FALSE(queue.add("/a.epub"));
  EXPECT_FALSE(queue.moveEarlier("/a.epub"));
  EXPECT_FALSE(queue.moveLater("/a.epub"));
  EXPECT_FALSE(queue.remove("/missing.epub"));
  EXPECT_EQ(queue.size(), 1u);
}

TEST(ReadingQueueTest, HardCapacityBoundsLongLivedMemory) {
  ReadingQueue queue;
  for (size_t i = 0; i < ReadingQueue::MAX_BOOKS; i++) {
    ASSERT_TRUE(queue.add("/book-" + std::to_string(i) + ".epub"));
  }
  EXPECT_FALSE(queue.add("/one-too-many.epub"));
  EXPECT_EQ(queue.size(), ReadingQueue::MAX_BOOKS);
}

TEST(ReadingQueueTest, RenamePreservesPositionAndAvoidsDuplicates) {
  ReadingQueue queue;
  ASSERT_TRUE(queue.add("/a.epub"));
  ASSERT_TRUE(queue.add("/old.epub"));
  ASSERT_TRUE(queue.add("/new.epub"));

  EXPECT_TRUE(queue.updatePath("/old.epub", "/renamed.epub"));
  EXPECT_EQ(queue.paths()[1], "/renamed.epub");
  EXPECT_TRUE(queue.updatePath("/renamed.epub", "/new.epub"));
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.paths()[1], "/new.epub");
}
