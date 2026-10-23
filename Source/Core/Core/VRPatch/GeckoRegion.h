// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Where VR patches get scratch memory: the top of the region Dolphin's Gecko code handler uses
// (0x80001800-0x80003000, which games leave free), below the code handler's return trampoline.
// The Gecko code list then ends below the scratch blocks. Pure functions, tested on their own.

#include <optional>
#include <span>

#include "Common/CommonTypes.h"
#include "Core/VRPatch/PatchTypes.h"

namespace VRPatch
{
constexpr u32 GECKO_REGION_START = 0x80001800;
constexpr u32 GECKO_REGION_END = 0x80003000;
// Scratch blocks are allocated downward from here (the last 8 bytes hold the Gecko handler's end
// code and its HLE return trampoline).
constexpr u32 SCRATCH_TOP = 0x80002FF8;
// The Gecko handler (codehandler.bin, 0xB40 bytes) and a code list of at least 150 lines stay
// below this.
constexpr u32 SCRATCH_FLOOR = 0x80002800;
constexpr u32 MAX_SCRATCH_BYTES = SCRATCH_TOP - SCRATCH_FLOOR;
constexpr u32 SCRATCH_ALIGNMENT = 16;

// The base of the highest free block of `size` bytes (16-aligned) between SCRATCH_FLOOR and
// SCRATCH_TOP that overlaps none of `used`; nullopt when there is none. Running patches keep their
// blocks, so a new block never moves another.
std::optional<u32> AllocateScratch(std::span<const AddressRange> used, u32 size);

// Where the Gecko code list must end: below every scratch block (GECKO_REGION_END when none).
u32 GeckoCodeListEnd(std::span<const AddressRange> scratch_blocks);

// True when `range` lies (partly) inside the Gecko region.
bool InGeckoRegion(const AddressRange& range);

// The first range of `a` that overlaps a range of `b`, if any.
std::optional<AddressRange> FindOverlap(std::span<const AddressRange> a,
                                        std::span<const AddressRange> b);
}  // namespace VRPatch
