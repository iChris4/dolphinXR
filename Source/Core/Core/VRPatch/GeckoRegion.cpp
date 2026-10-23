// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/GeckoRegion.h"

#include <algorithm>

namespace VRPatch
{
std::optional<u32> AllocateScratch(std::span<const AddressRange> used, u32 size)
{
  if (size == 0 || size > MAX_SCRATCH_BYTES)
    return std::nullopt;
  u32 top = SCRATCH_TOP;
  while (top >= SCRATCH_FLOOR + size)
  {
    const u32 base = (top - size) & ~(SCRATCH_ALIGNMENT - 1);
    if (base < SCRATCH_FLOOR)
      return std::nullopt;
    const AddressRange candidate{base, base + size};
    u32 lowest_overlap = top;
    bool overlaps = false;
    for (const AddressRange& range : used)
    {
      if (candidate.Overlaps(range))
      {
        overlaps = true;
        lowest_overlap = std::min(lowest_overlap, range.start);
      }
    }
    if (!overlaps)
      return base;
    if (lowest_overlap >= top)
      return std::nullopt;
    top = lowest_overlap;
  }
  return std::nullopt;
}

u32 GeckoCodeListEnd(std::span<const AddressRange> scratch_blocks)
{
  u32 end = GECKO_REGION_END;
  for (const AddressRange& block : scratch_blocks)
  {
    if (block.end > block.start && block.start < end)
      end = block.start;
  }
  return end;
}

bool InGeckoRegion(const AddressRange& range)
{
  return range.Overlaps({GECKO_REGION_START, GECKO_REGION_END});
}

std::optional<AddressRange> FindOverlap(std::span<const AddressRange> a,
                                        std::span<const AddressRange> b)
{
  for (const AddressRange& x : a)
  {
    for (const AddressRange& y : b)
    {
      if (x.Overlaps(y))
        return x;
    }
  }
  return std::nullopt;
}
}  // namespace VRPatch
