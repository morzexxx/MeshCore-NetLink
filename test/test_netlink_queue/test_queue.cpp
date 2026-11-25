#include "../../examples/simple_repeater/NetLinkQueue.h"
#include <gtest/gtest.h>
#include <set>

namespace {
using netlink::FrameQueue;

void put(FrameQueue& queue, uint32_t number) {
  auto& frame = queue.insert(number);
  frame.size = 1;
  frame.bytes[0] = static_cast<uint8_t>(number);
}

std::set<uint32_t> numbers(FrameQueue& queue) {
  std::set<uint32_t> result;
  for (auto& f : queue) {
    if (!f.size) continue;
    EXPECT_EQ(f.bytes[0], static_cast<uint8_t>(f.number));
    result.insert(f.number);
  }
  EXPECT_EQ(queue.count(), result.size());
  return result;
}

TEST(NetLinkQueue, DisabledQueueHasNoStorage) {
  FrameQueue queue;
  EXPECT_EQ(queue.begin(), nullptr);
  ASSERT_TRUE(queue.allocate(32));
  put(queue, 17);
  ASSERT_TRUE(queue.allocate(0));
  EXPECT_EQ(queue.begin(), nullptr);
  EXPECT_EQ(queue.count(), 0u);
}

TEST(NetLinkQueue, HolesAreReusedBeforeEviction) {
  FrameQueue queue;
  ASSERT_TRUE(queue.allocate(3));
  for (unsigned i = 1; i <= 3; ++i) put(queue, i);
  queue.remove(queue.begin()[1]);
  put(queue, 4);
  EXPECT_EQ(numbers(queue), (std::set<uint32_t>{1, 3, 4}));
  put(queue, 5);
  EXPECT_EQ(numbers(queue), (std::set<uint32_t>{3, 4, 5}));
}

TEST(NetLinkQueue, ShrinkAndGrowPreserveNewestAcrossWrap) {
  FrameQueue old, small, large;
  ASSERT_TRUE(old.allocate(4));
  for (uint32_t n : {UINT32_MAX - 2, UINT32_MAX - 1, UINT32_MAX, 1u, 2u}) put(old, n);
  ASSERT_TRUE(small.allocate(2));
  small.retainNewest(old);
  EXPECT_EQ(numbers(small), (std::set<uint32_t>{1, 2}));
  ASSERT_TRUE(large.allocate(32));
  large.retainNewest(small);
  EXPECT_EQ(numbers(large), numbers(small));
}
} // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
