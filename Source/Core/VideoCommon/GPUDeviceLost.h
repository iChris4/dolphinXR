// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace VideoCommon
{
// The video backend lost its GPU device: a driver reset after a GPU fault (Windows' TDR), or the
// GPU removed. Nothing the device held works any more and new resources cannot be created, so the
// paths that use them would crash (a failed texture allocation made an EFB copy write into a null
// cache entry). From the first report on, the video thread drops the FIFO's commands and the CPU
// thread's video requests answer without the GPU, emulation stops, and the user is told.

// Any thread. The first call logs `detail` and stops emulation with a message for the user.
void ReportGPUDeviceLost(std::string_view backend, std::string_view detail);

// Any thread.
bool IsGPUDeviceLost();

// A new video backend starts with a working device.
void ResetGPUDeviceLost();

// A device was lost since Dolphin started. Backends may not get a new one in this process: D3D12
// creates no device on a GPU while anything still holds the lost one (the OpenXR runtime can), and
// Vulkan keeps its context between VR games.
bool WasGPUDeviceLostInThisProcess();
}  // namespace VideoCommon
