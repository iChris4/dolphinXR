// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GPUFifoDiagnostics.h"

#include <fmt/format.h>

#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

namespace VideoCommon
{
GPUFifoState GetGPUFifoState(Core::System& system)
{
  auto& cp = system.GetCommandProcessor();
  const auto& fifo = cp.GetFifo();
  const auto debug = cp.GetDebugState();
  const auto& pi = system.GetProcessorInterface();

  GPUFifoState state;
  state.base = fifo.CPBase.load();
  state.end = fifo.CPEnd.load();
  state.read = fifo.CPReadPointer.load();
  state.write = fifo.CPWritePointer.load();
  state.safe_read = fifo.SafeCPReadPointer.load();
  state.distance = fifo.CPReadWriteDistance.load();
  // CPEnd is the last 32-byte block of the ring.
  const u32 ring_size = state.end + 32 - state.base;
  if (state.end >= state.base)
  {
    state.pointer_distance =
        state.write >= state.read ? state.write - state.read : state.write + ring_size - state.read;
  }
  state.breakpoint = fifo.CPBreakpoint.load();
  state.breakpoint_enabled = fifo.bFF_BPEnable.load() != 0;
  state.breakpoint_interrupt = fifo.bFF_BPInt.load() != 0;
  state.breakpoint_hit = fifo.bFF_Breakpoint.load() != 0;
  state.high_watermark = fifo.CPHiWatermark;
  state.low_watermark = fifo.CPLoWatermark;
  state.over_high = fifo.bFF_HiWatermark.load() != 0;
  state.under_low = fifo.bFF_LoWatermark.load() != 0;
  state.high_interrupt = fifo.bFF_HiWatermarkInt.load() != 0;
  state.low_interrupt = fifo.bFF_LoWatermarkInt.load() != 0;
  state.gp_read_enabled = fifo.bFF_GPReadEnable.load() != 0;
  state.linked = fifo.bFF_GPLinkEnable.load() != 0;
  state.cp_control = debug.control;
  state.cp_status = debug.status;
  state.cp_interrupt_set = debug.interrupt_set;
  state.cp_interrupt_waiting = debug.interrupt_waiting;
  state.pi_cause = pi.GetCause();
  state.pi_mask = pi.GetMask();
  state.cpu_external_interrupts = system.GetPPCState().msr.EE != 0;
  state.cpu_fifo_base = pi.m_fifo_cpu_base;
  state.cpu_fifo_end = pi.m_fifo_cpu_end;
  state.cpu_fifo_write = pi.m_fifo_cpu_write_pointer;
  state.undecoded_bytes = system.GetFifo().GetVideoBufferPendingBytes();
  return state;
}

std::string DescribeGPUFifoState(const GPUFifoState& s)
{
  const bool cp_pending = (s.pi_cause & ProcessorInterface::INT_CAUSE_CP) != 0;
  const bool cp_unmasked = (s.pi_mask & ProcessorInterface::INT_CAUSE_CP) != 0;
  std::string text = fmt::format(
      "CP FIFO {:08X}-{:08X}: read {:08X}, write {:08X}, distance {:#x} ({} bytes not read "
      "yet), safe read {:08X}.",
      s.base, s.end, s.read, s.write, s.distance, s.distance, s.safe_read);
  if (s.pointer_distance != s.distance)
  {
    text += fmt::format(" The pointers are {:#x} apart: the distance disagrees with them, so the "
                        "GPU stops early.",
                        s.pointer_distance);
  }
  text += fmt::format(
      "\nBreakpoint {:08X}: {}, interrupt {}, hit flag {}{}.\n", s.breakpoint,
      s.breakpoint_enabled ? "enabled" : "disabled", s.breakpoint_interrupt ? "on" : "off",
      s.breakpoint_hit ? "set" : "clear",
      s.breakpoint_enabled && s.read == s.breakpoint ? " (the GPU is stopped at it)" : "");
  text += fmt::format(
      "Watermarks: high {:#x} (over: {}, interrupt {}), low {:#x} (under: {}, interrupt {}).\n",
      s.high_watermark, s.over_high, s.high_interrupt, s.low_watermark, s.under_low,
      s.low_interrupt);
  text += fmt::format("CP control {:#06x} (GP read {}, link {}), status {:#06x}. CP interrupt "
                      "line {}, a raise or clear waiting for the CPU thread: {}.\n",
                      s.cp_control, s.gp_read_enabled ? "on" : "off", s.linked ? "on" : "off",
                      s.cp_status, s.cp_interrupt_set ? "set" : "clear",
                      s.cp_interrupt_waiting ? "yes" : "no");
  text += fmt::format("PI: cause {:#x}, mask {:#x} (CP interrupt pending {}, unmasked {}); CPU "
                      "external interrupts {}. CPU FIFO {:08X}-{:08X}, write {:08X}.\n",
                      s.pi_cause, s.pi_mask, cp_pending ? "yes" : "no", cp_unmasked ? "yes" : "no",
                      s.cpu_external_interrupts ? "enabled" : "disabled", s.cpu_fifo_base,
                      s.cpu_fifo_end, s.cpu_fifo_write);
  text += fmt::format("GPU thread: {} bytes copied from the FIFO but not decoded (an incomplete "
                      "command).",
                      s.undecoded_bytes);
  return text;
}
}  // namespace VideoCommon
