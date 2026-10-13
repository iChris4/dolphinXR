// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace Vulkan
{
// A bounded, allocation-free lookup for immutable descriptor sets. The owner must clear it
// before the descriptor pool is reset or a referenced image view/sampler can be destroyed.
//
// The lookup hash is the XOR of one contribution per binding slot (HashSlot), so an owner that
// changes one slot at a time can maintain the hash incrementally instead of rehashing every
// binding on each draw. The overloads without a hash argument compute it from scratch.
template <size_t NumSamplers, size_t Capacity = 1024>
class SamplerDescriptorSetCache
{
public:
  using Bindings = std::array<VkDescriptorImageInfo, NumSamplers>;

  static size_t HashSlot(size_t index, const VkDescriptorImageInfo& binding)
  {
    uint64_t hash = Mix((static_cast<uint64_t>(index) + 1) * 0x9e3779b97f4a7c15ULL);
    hash = Mix(hash ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding.sampler)));
    hash = Mix(hash ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding.imageView)));
    hash = Mix(hash ^ static_cast<uint64_t>(binding.imageLayout));
    return static_cast<size_t>(hash);
  }

  static size_t HashBindings(const Bindings& bindings)
  {
    size_t hash = 0;
    for (size_t i = 0; i < NumSamplers; ++i)
      hash ^= HashSlot(i, bindings[i]);
    return hash;
  }

  VkDescriptorSet Find(const Bindings& bindings) const
  {
    return Find(bindings, HashBindings(bindings));
  }

  VkDescriptorSet Find(const Bindings& bindings, size_t hash) const
  {
    const auto& entry = m_entries[hash & (Capacity - 1)];
    if (entry.set == VK_NULL_HANDLE)
      return VK_NULL_HANDLE;
    for (size_t i = 0; i < NumSamplers; ++i)
    {
      // Compare fields, not struct padding. A hash collision is always a cache miss.
      if (entry.bindings[i].sampler != bindings[i].sampler ||
          entry.bindings[i].imageView != bindings[i].imageView ||
          entry.bindings[i].imageLayout != bindings[i].imageLayout)
      {
        return VK_NULL_HANDLE;
      }
    }
    return entry.set;
  }

  void Insert(const Bindings& bindings, VkDescriptorSet set)
  {
    Insert(bindings, HashBindings(bindings), set);
  }

  void Insert(const Bindings& bindings, size_t hash, VkDescriptorSet set)
  {
    m_entries[hash & (Capacity - 1)] = {bindings, set};
  }

  void Clear()
  {
    for (auto& entry : m_entries)
      entry.set = VK_NULL_HANDLE;
  }

private:
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);

  static uint64_t Mix(uint64_t value)
  {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
  }

  struct Entry
  {
    Bindings bindings{};
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  std::array<Entry, Capacity> m_entries{};
};
}  // namespace Vulkan
