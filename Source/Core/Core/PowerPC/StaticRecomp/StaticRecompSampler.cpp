// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompSampler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach/mach.h>
#endif

namespace StaticRecomp
{
std::unique_ptr<Sampler> Sampler::CreateFromEnvironment(const u32* guest_pc)
{
#ifdef __APPLE__
  const char* spec = std::getenv("STATICRECOMP_SAMPLER");
  if (!spec || !*spec)
    return nullptr;
  char* end = nullptr;
  const double delay_s = std::strtod(spec, &end);
  const double length_s = (end && *end == ',') ? std::strtod(end + 1, nullptr) : 10.0;
  if (delay_s < 0 || length_s <= 0)
    return nullptr;
  // mach_thread_self() returns a send right this sampler keeps for its lifetime.
  return std::unique_ptr<Sampler>(
      new Sampler(static_cast<u32>(mach_thread_self()), guest_pc, delay_s, length_s));
#else
  (void)guest_pc;
  return nullptr;
#endif
}

Sampler::Sampler(u32 target_thread, const u32* guest_pc, double delay_s, double length_s)
{
  m_thread = std::thread(&Sampler::Main, this, target_thread, guest_pc, delay_s, length_s);
}

Sampler::~Sampler()
{
  m_stop.store(true);
  if (m_thread.joinable())
    m_thread.join();
}

void Sampler::Main(u32 target_thread, const u32* guest_pc, double delay_s, double length_s)
{
#ifdef __APPLE__
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now() + std::chrono::duration<double>(delay_s);
  while (Clock::now() < start)
  {
    if (m_stop.load())
    {
      mach_port_deallocate(mach_task_self(), static_cast<thread_act_t>(target_thread));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  NOTICE_LOG_FMT(POWERPC, "StaticRecomp sampler: sampling the CPU thread for {} s", length_s);

  const auto target = static_cast<thread_act_t>(target_thread);
  std::unordered_map<u64, u32> host_pcs;
  std::unordered_map<u32, u32> guest_pcs;
  u32 samples = 0;
  const auto stop = Clock::now() + std::chrono::duration<double>(length_s);
  while (Clock::now() < stop && !m_stop.load())
  {
    if (thread_suspend(target) == KERN_SUCCESS)
    {
      arm_thread_state64_t state;
      mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
      if (thread_get_state(target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                           &count) == KERN_SUCCESS)
      {
        ++host_pcs[arm_thread_state64_get_pc(state)];
        const u32 guest = *reinterpret_cast<const volatile u32*>(guest_pc);
        ++guest_pcs[guest];
        ++samples;
      }
      thread_resume(target);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(900));
  }

  // Host program counters as image + offset, most frequent first.
  std::vector<std::pair<u64, u32>> hosts(host_pcs.begin(), host_pcs.end());
  std::sort(hosts.begin(), hosts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string out = fmt::format("# {} samples over {} s\n# host image offset count\n", samples,
                                length_s);
  for (const auto& [pc, n] : hosts)
  {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(pc), &info) && info.dli_fname && info.dli_fbase)
    {
      const char* slash = std::strrchr(info.dli_fname, '/');
      out += fmt::format("host {} 0x{:x} {}\n", slash ? slash + 1 : info.dli_fname,
                         pc - reinterpret_cast<u64>(info.dli_fbase), n);
    }
    else
    {
      out += fmt::format("host ? 0x{:x} {}\n", pc, n);
    }
  }
  std::vector<std::pair<u32, u32>> guests(guest_pcs.begin(), guest_pcs.end());
  std::sort(guests.begin(), guests.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  out += "# guest pc count\n";
  for (const auto& [pc, n] : guests)
    out += fmt::format("guest {:08x} {}\n", pc, n);

  const std::string path = File::GetUserPath(D_USER_IDX) + "StaticRecompSamples.txt";
  File::WriteStringToFile(path, out);
  NOTICE_LOG_FMT(POWERPC, "StaticRecomp sampler: {} samples written to {}", samples, path);
  mach_port_deallocate(mach_task_self(), target);
#else
  (void)target_thread;
  (void)guest_pc;
  (void)delay_s;
  (void)length_s;
#endif
}
}  // namespace StaticRecomp
