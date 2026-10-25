// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/CameraPitch.h"

#include <atomic>

namespace VR::CameraPitch
{
namespace
{
// 0: no source (no valid address is 0).
std::atomic<u32> s_source{0};
}  // namespace

void SetSource(std::optional<u32> address)
{
  s_source.store(address.value_or(0), std::memory_order_relaxed);
}

std::optional<u32> GetSource()
{
  const u32 address = s_source.load(std::memory_order_relaxed);
  if (address == 0)
    return std::nullopt;
  return address;
}
}  // namespace VR::CameraPitch
