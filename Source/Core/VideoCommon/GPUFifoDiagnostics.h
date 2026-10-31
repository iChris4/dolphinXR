// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace VideoCommon
{
// A snapshot of the command processor's FIFO as the GPU thread and the CPU see it: pointers,
// read/write distance, breakpoint, watermarks, control and status registers, the CP interrupt line
// and the PI interrupt cause and mask. It explains why a game that waits for the GPU (a frame-end
// breakpoint, a draw-done) does not get it. Unlocked reads of state two threads change.
struct GPUFifoState
{
  u32 base = 0;
  u32 end = 0;
  u32 read = 0;
  u32 write = 0;
  u32 safe_read = 0;
  u32 distance = 0;
  // The distance the pointers imply (write - read around the ring). It differs from `distance`
  // when the FIFO state is corrupted: the GPU then stops that many bytes early.
  u32 pointer_distance = 0;
  u32 breakpoint = 0;
  bool breakpoint_enabled = false;
  bool breakpoint_interrupt = false;
  bool breakpoint_hit = false;
  u32 high_watermark = 0;
  u32 low_watermark = 0;
  bool over_high = false;
  bool under_low = false;
  bool high_interrupt = false;
  bool low_interrupt = false;
  bool gp_read_enabled = false;
  bool linked = false;
  u16 cp_control = 0;
  u16 cp_status = 0;
  bool cp_interrupt_set = false;
  bool cp_interrupt_waiting = false;
  u32 pi_cause = 0;
  u32 pi_mask = 0;
  bool cpu_external_interrupts = false;
  u32 cpu_fifo_base = 0;
  u32 cpu_fifo_end = 0;
  u32 cpu_fifo_write = 0;
  size_t undecoded_bytes = 0;
};

GPUFifoState GetGPUFifoState(Core::System& system);
std::string DescribeGPUFifoState(const GPUFifoState& state);
}  // namespace VideoCommon
