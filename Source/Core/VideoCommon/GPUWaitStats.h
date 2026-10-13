// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "Common/CommonTypes.h"

// Accounting of the moments the video thread blocks on the GPU, and of CPU EFB reads.
//
// Backends call RecordWait() from the places where they actually waited for GPU work (fence
// waits, blocking maps, glFinish). VideoCommon tags what that wait was for with a ScopedReason
// around the operation that may block, so the once-per-5-seconds report can say whether a frame
// stalls on EFB reads, on EFB copies to RAM, on frame-resource recycling, etc. Everything is
// video-thread only except the atomics, which keep a backend worker thread's reports safe.
namespace VideoCommon::GPUWaitStats
{
enum class Reason : u8
{
  Other,         // No tag set: driver/backend internal, utility readbacks, shutdown...
  EFBPeek,       // CPU EFB color/depth read (FramebufferManager::PeekEFB*, RefreshPeekCache)
  EFBCopyToRAM,  // EFB copy written back to guest memory (TextureCacheBase)
  BoundingBox,
  PerfQuery,
  FrameRecycle,  // Waiting for an older frame's command buffer so its resources can be reused
  Count
};

// Sets the reason for waits recorded until the scope ends (nested scopes restore the outer one).
class ScopedReason
{
public:
  explicit ScopedReason(Reason reason);
  ~ScopedReason();
  ScopedReason(const ScopedReason&) = delete;
  ScopedReason& operator=(const ScopedReason&) = delete;

private:
  Reason m_previous;
};

// Called by the backend after it blocked for wait_us microseconds (0 is ignored).
void RecordWait(u64 wait_us);

// Outcome of one CPU EFB read.
enum class EFBRead : u8
{
  Fresh,    // The newest readback was complete: current data, no wait
  Stale,    // The newest readback was still in flight: data of the previous completed one
  Blocked,  // Had to wait for the GPU
  Count
};
void RecordEFBRead(EFBRead outcome);
// One EFB tile readback issued to the GPU.
void RecordEFBPopulate();

// Once per emulated frame. Logs the summary line every ~5 seconds when anything was recorded.
void OnFrameEnd();

// Drops the current window (backend init/shutdown).
void Reset();
}  // namespace VideoCommon::GPUWaitStats
