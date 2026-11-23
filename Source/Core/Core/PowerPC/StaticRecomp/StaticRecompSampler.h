// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "Common/CommonTypes.h"

namespace StaticRecomp
{
// A sampling profiler for the StaticRecomp CPU thread, for devices where Instruments cannot attach
// (Vision Pro drops off its device list for long stretches). STATICRECOMP_SAMPLER="<delay>,<length>"
// in the environment: starting <delay> seconds after the core first runs, the calling thread is
// sampled about a thousand times a second for <length> seconds, then
// <User dir>/StaticRecompSamples.txt lists the host program counters seen (image, offset in the
// image, count; symbolize with atos) and the guest program counters (the generated code keeps
// ctx->pc current). Apple platforms only.
class Sampler
{
public:
  // Null unless STATICRECOMP_SAMPLER is set (or off Apple platforms). Call on the thread to sample.
  static std::unique_ptr<Sampler> CreateFromEnvironment(const u32* guest_pc);
  ~Sampler();

  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

private:
  Sampler(u32 target_thread, const u32* guest_pc, double delay_s, double length_s);
  void Main(u32 target_thread, const u32* guest_pc, double delay_s, double length_s);

  std::atomic<bool> m_stop{false};
  std::thread m_thread;
};
}  // namespace StaticRecomp
