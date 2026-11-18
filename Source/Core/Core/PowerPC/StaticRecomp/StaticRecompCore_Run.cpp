// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/SystemTimers.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
constexpr u32 ASYNC_EXCEPTION_MASK =
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR;
constexpr u32 MSR_EE = 0x00008000u;

struct FileCloser
{
  void operator()(std::FILE* file) const
  {
    if (file)
      std::fclose(file);
  }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr OpenDispatchTrace()
{
  const char* path = std::getenv("STATICRECOMP_TRACE_FILE");
  if (!path || !*path)
    return {};

  FilePtr file(std::fopen(path, "w"));
  if (file)
  {
    std::fprintf(file.get(), "dispatch,pc,lr,ctr,cr,timebase,ppc_downcount\n");
    std::fflush(file.get());
  }
  return file;
}
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();
  FilePtr dispatch_trace = OpenDispatchTrace();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);
  const bool lockstep_enabled = m_lockstep_verifier->IsEnabled();
  const auto fast_dispatchable_at = [this](u32 address) {
    // A REL running where it was recompiled needs no translation: its chunks are looked up as
    // main.dol's are (the chunk hashes still decide whether its code is there).
    if ((m_has_rel_modules && m_rel_translation) || !m_forced_fallback_ranges.empty())
      return FastDispatchableAt(address);
    if (!m_module_active || m_chunk_lookup_table.empty())
      return false;

    int lookup_index = -1;
    if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    {
      lookup_index = static_cast<int>((address - 0x80000000u) >> 2);
    }
    else if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    {
      lookup_index = static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
    }
    if (lookup_index < 0 || lookup_index >= static_cast<int>(m_chunk_lookup_table.size()))
      return false;
    const int chunk = m_chunk_lookup_table[lookup_index];
    return chunk >= 0 && m_chunk_state[chunk] == CHUNK_VERIFIED;
  };

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit && !m_guest.host_call)
  {
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    // Once per slice, the OS module list: two reads while nothing was linked or unlinked.
    if (m_has_rel_modules)
      RefreshRelSections();
    if (m_fallback_stats && !m_stats_ref_taken &&
        std::chrono::steady_clock::now() - m_stats_init_time > std::chrono::seconds(5))
    {
      m_stats_ref_taken = true;
      m_stats_ref_time = std::chrono::steady_clock::now();
      m_stats_ref_ticks = core_timing.GetTicks();
    }
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc) &&
          !(m_guest.host_call && IsHostCallAddress(ppc.pc)))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          if (dispatch_trace && (m_native_dispatches & 0xFFFFFu) == 0)
          {
            std::fprintf(dispatch_trace.get(), "%llu,%08x,%08x,%08x,%08x,%llu,%d\n",
                         static_cast<unsigned long long>(m_native_dispatches), m_guest.pc,
                         m_guest.lr, m_guest.ctr, m_guest.cr,
                         static_cast<unsigned long long>(m_guest.timebase), ppc.downcount);
            std::fflush(dispatch_trace.get());
          }
          const bool do_ls = lockstep_enabled && m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          if (m_collect_dispatch_samples && (m_native_dispatches & 4095u) == 0)
            ++m_dispatch_samples[m_guest.pc];
          const u32 runtime_dispatch_address = m_guest.pc;
          u32 linked_dispatch_address = runtime_dispatch_address;
          if (m_has_rel_modules && m_rel_translation)
            ResolveNativeAddress(runtime_dispatch_address, &linked_dispatch_address, nullptr);
          m_guest.pc = linked_dispatch_address;
          m_module->dispatch(&m_guest, linked_dispatch_address);
          if (m_has_rel_modules && m_rel_translation)
            m_guest.pc = TranslateRelAddress(m_guest.pc);
          ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
          ppc.downcount -= static_cast<int>(effective_charge);
          m_charged_cycles += effective_charge;
          const u64 total_cycles = m_timebase_cycle_remainder + effective_charge;
          m_guest.timebase += total_cycles / SystemTimers::TIMER_RATIO;
          m_timebase_cycle_remainder = total_cycles % SystemTimers::TIMER_RATIO;

          // Idle loop skipping: a configured target loop (e.g. Wii Menu OSIdleThread), or a
          // busy-wait loop the module came back to its head of (a loop runs a cycle budget per
          // dispatch, then returns at its head with the loop still waiting), found as the JITs
          // and the CachedInterpreter find them. Time skips to the next event, as there.
          if ((m_guest.pc == m_idle_pc && m_idle_pc != 0) ||
              (m_guest.pc == runtime_dispatch_address && IsBusyWaitLoop(m_guest.pc)))
          {
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
          if ((ppc.Exceptions & ASYNC_EXCEPTION_MASK) != 0 && (m_guest.msr & MSR_EE) != 0)
            break;  // rfi/mtmsr re-enabled interrupts while one was pending.
        } while (m_module_active && fast_dispatchable_at(m_guest.pc) &&
                 !(m_guest.host_call && IsHostCallAddress(m_guest.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
        else if ((ppc.Exceptions & ASYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExternalExceptions();
      }
      else
      {
        if (m_guest.host_call && IsHostCallAddress(ppc.pc))
        {
          SyncIn();
          bool handled = m_guest.host_call(&m_guest, m_guest.pc);
          if (!handled && m_guest.pc < m_guest.ram_size)
            handled = m_guest.host_call(&m_guest, m_guest.pc | 0x80000000u);
          if (m_fallback_jit && IsHostCallAddress(m_guest.lr))
            m_fallback_jit->GetBlockCache()->InvalidateICache(m_guest.lr, 4, true);
          if (handled)
          {
            const s64 charge = -m_guest.downcount;
            m_guest.downcount = 0;
            const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
            ppc.downcount -= static_cast<int>(effective_charge);
            AdvanceGuestTimebase(effective_charge);
            SyncOut();
            continue;
          }
          SyncOut();
          if (m_fallback_jit)
          {
            m_host_call_passthrough_pc = ppc.pc;
            m_host_call_passthrough = true;
          }
        }
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_module_active && IsForcedFallbackAddress(ppc.pc))
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        }
        else if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) &&
                   !IsHostCallAddress(ppc.pc) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
