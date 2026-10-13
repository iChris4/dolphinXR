// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "VideoBackends/Vulkan/VKRecordingQueue.h"

namespace
{
struct Item
{
  int value;
  int padding[3];
};

constexpr Item WAKE_ITEM{-1, {}};
}  // namespace

TEST(VulkanRecordingQueue, PreservesOrderAcrossWraparound)
{
  Vulkan::RecordingQueue<Item, 8> queue;
  std::vector<int> received;
  std::thread consumer([&] {
    while (queue.WaitForCommand())
    {
      while (const Item* item = queue.Front())
      {
        received.push_back(item->value);
        queue.Pop();
      }
    }
  });
  for (int i = 0; i < 1000; ++i)
    queue.Push(Item{i, {}});
  queue.WaitForEmpty();
  queue.Shutdown(WAKE_ITEM);
  consumer.join();

  // The consumer may observe the shutdown flag before the wake-up command lands, so the
  // trailing wake-up item is optional; everything before it must be complete and in order.
  ASSERT_TRUE(received.size() == 1000u || received.size() == 1001u);
  for (int i = 0; i < 1000; ++i)
    EXPECT_EQ(received[i], i);
  if (received.size() == 1001u)
    EXPECT_EQ(received.back(), WAKE_ITEM.value);
}

TEST(VulkanRecordingQueue, ProducerBlocksWhenFullAndWaitForEmptyCoversEverything)
{
  Vulkan::RecordingQueue<Item, 4> queue;
  std::atomic<int> consumed{0};
  std::atomic<bool> release{false};
  std::thread consumer([&] {
    while (!release.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (queue.WaitForCommand())
    {
      while (const Item* item = queue.Front())
      {
        // Slow consumer: the producer must block on the full ring rather than overwrite.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (item->value > 0)
          consumed.fetch_add(item->value);
        queue.Pop();
      }
    }
  });

  bool saw_full_wait = false;
  release.store(true);
  for (int i = 1; i <= 64; ++i)
    saw_full_wait |= !queue.Push(Item{i, {}});
  queue.WaitForEmpty();
  EXPECT_EQ(queue.GetDepth(), 0u);
  EXPECT_EQ(consumed.load(), 64 * 65 / 2);
  EXPECT_TRUE(saw_full_wait);

  queue.Shutdown(WAKE_ITEM);
  consumer.join();
}

TEST(VulkanRecordingQueue, ShutdownWakesAnIdleConsumer)
{
  Vulkan::RecordingQueue<Item, 8> queue;
  std::atomic<int> wake_ups{0};
  std::atomic<bool> returned{false};
  std::thread consumer([&] {
    while (queue.WaitForCommand())
    {
      while (const Item* item = queue.Front())
      {
        wake_ups.fetch_add(1);
        EXPECT_EQ(item->value, WAKE_ITEM.value);
        queue.Pop();
      }
    }
    returned.store(true);
  });
  // Long enough for the consumer to fall out of its spin loop and sleep on the index.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(returned.load());
  queue.Shutdown(WAKE_ITEM);
  consumer.join();
  EXPECT_TRUE(returned.load());
  EXPECT_LE(wake_ups.load(), 1);
}

TEST(VulkanRecordingQueue, WaitForEmptyReturnsImmediatelyWhenNothingIsQueued)
{
  Vulkan::RecordingQueue<Item, 8> queue;
  queue.WaitForEmpty();
  EXPECT_EQ(queue.Front(), nullptr);
  EXPECT_EQ(queue.GetDepth(), 0u);
}
