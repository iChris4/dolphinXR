// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoBackends/Vulkan/SamplerDescriptorSetCache.h"

namespace
{
using Cache = Vulkan::SamplerDescriptorSetCache<8>;

Cache::Bindings MakeBindings()
{
  Cache::Bindings bindings{};
  for (size_t i = 0; i < bindings.size(); ++i)
  {
    bindings[i] = {reinterpret_cast<VkSampler>(0x1000 + i),
                   reinterpret_cast<VkImageView>(0x2000 + i),
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  return bindings;
}
}  // namespace

TEST(VulkanSamplerDescriptorSetCache, RequiresEveryBindingFieldToMatch)
{
  Cache cache;
  const auto bindings = MakeBindings();
  const auto set = reinterpret_cast<VkDescriptorSet>(0x3000);
  EXPECT_EQ(cache.Find(bindings), VK_NULL_HANDLE);
  cache.Insert(bindings, set);
  EXPECT_EQ(cache.Find(bindings), set);

  for (size_t i = 0; i < bindings.size(); ++i)
  {
    auto changed = bindings;
    changed[i].sampler = reinterpret_cast<VkSampler>(0x4000);
    EXPECT_EQ(cache.Find(changed), VK_NULL_HANDLE);
    changed = bindings;
    changed[i].imageView = reinterpret_cast<VkImageView>(0x5000);
    EXPECT_EQ(cache.Find(changed), VK_NULL_HANDLE);
    changed = bindings;
    changed[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    EXPECT_EQ(cache.Find(changed), VK_NULL_HANDLE);
  }
  EXPECT_EQ(cache.Find(bindings), set);
}

TEST(VulkanSamplerDescriptorSetCache, CollisionsEvictWithoutReturningTheWrongSet)
{
  // Force every binding combination into the same slot.
  Vulkan::SamplerDescriptorSetCache<8, 1> cache;
  const auto first = MakeBindings();
  auto second = first;
  second[7].imageView = reinterpret_cast<VkImageView>(0x5000);
  const auto first_set = reinterpret_cast<VkDescriptorSet>(0x3000);
  const auto second_set = reinterpret_cast<VkDescriptorSet>(0x4000);
  cache.Insert(first, first_set);
  EXPECT_EQ(cache.Find(second), VK_NULL_HANDLE);
  cache.Insert(second, second_set);
  EXPECT_EQ(cache.Find(first), VK_NULL_HANDLE);
  EXPECT_EQ(cache.Find(second), second_set);
}

TEST(VulkanSamplerDescriptorSetCache, ClearPreventsReuseAcrossResourceLifetimes)
{
  Cache cache;
  const auto bindings = MakeBindings();
  cache.Insert(bindings, reinterpret_cast<VkDescriptorSet>(0x3000));
  cache.Clear();
  EXPECT_EQ(cache.Find(bindings), VK_NULL_HANDLE);
  const auto new_set = reinterpret_cast<VkDescriptorSet>(0x4000);
  cache.Insert(bindings, new_set);
  EXPECT_EQ(cache.Find(bindings), new_set);
  cache.Insert(bindings, VK_NULL_HANDLE);
  EXPECT_EQ(cache.Find(bindings), VK_NULL_HANDLE);
}

TEST(VulkanSamplerDescriptorSetCache, IncrementalHashMatchesFullHash)
{
  // Mirror StateTracker: keep one contribution per slot and XOR them into a running hash.
  auto bindings = MakeBindings();
  std::array<size_t, 8> slot_hashes{};
  size_t running = 0;
  for (size_t i = 0; i < bindings.size(); ++i)
  {
    slot_hashes[i] = Cache::HashSlot(i, bindings[i]);
    running ^= slot_hashes[i];
  }
  EXPECT_EQ(running, Cache::HashBindings(bindings));

  const auto update = [&](size_t index) {
    running ^= slot_hashes[index];
    slot_hashes[index] = Cache::HashSlot(index, bindings[index]);
    running ^= slot_hashes[index];
  };

  bindings[3].imageView = reinterpret_cast<VkImageView>(0x7000);
  update(3);
  EXPECT_EQ(running, Cache::HashBindings(bindings));
  bindings[3].sampler = reinterpret_cast<VkSampler>(0x7100);
  update(3);
  bindings[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  update(0);
  EXPECT_EQ(running, Cache::HashBindings(bindings));

  // Swapping two slots' contents must change the hash: the slot index is part of each term.
  auto swapped = bindings;
  std::swap(swapped[1], swapped[2]);
  EXPECT_NE(Cache::HashBindings(swapped), Cache::HashBindings(bindings));

  // The hash-taking overloads agree with the computing ones.
  Cache cache;
  const auto set = reinterpret_cast<VkDescriptorSet>(0x3000);
  cache.Insert(bindings, running, set);
  EXPECT_EQ(cache.Find(bindings), set);
  EXPECT_EQ(cache.Find(bindings, running), set);
}
