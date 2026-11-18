// RecompCore: StaticRecomp CPU core - State synchronization.
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"

void StaticRecompCore::SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc)
{
  std::memcpy(ppc.gpr, s.gpr, sizeof(ppc.gpr));
  for (int i = 0; i < 32; ++i)
  {
    std::memcpy(&ppc.ps[i].ps0, &s.fpr[i], sizeof(u64));
    std::memcpy(&ppc.ps[i].ps1, &s.ps1[i], sizeof(u64));
  }
  ppc.pc = s.pc;
  ppc.npc = s.pc;
  ppc.spr[SPR_LR] = s.lr;
  ppc.spr[SPR_CTR] = s.ctr;
  ppc.cr.Set(s.cr);
  ppc.SetXER(UReg_XER{s.xer});
  ppc.fpscr.Hex = s.fpscr;
  ppc.spr[SPR_SRR0] = s.srr0;
  ppc.spr[SPR_SRR1] = s.srr1;
  ppc.spr[SPR_DAR] = s.dar;
  ppc.spr[SPR_DSISR] = s.dsisr;
  ppc.spr[SPR_EAR] = s.ear;
  ppc.spr[SPR_HID2] = s.hid2;
  for (int i = 0; i < 16; ++i)
    ppc.sr[i] = s.sr[i];
  for (int i = 0; i < 8; ++i)
    ppc.spr[SPR_GQR0 + i] = s.gqr[i];
  ppc.reserve_address = s.reserve_addr;
  ppc.reserve = s.reserve_valid;
}

void StaticRecompCore::AdvanceGuestTimebase(u64 cpu_cycles)
{
  const u64 total_cycles = m_timebase_cycle_remainder + cpu_cycles;
  m_guest.timebase += total_cycles / SystemTimers::TIMER_RATIO;
  m_timebase_cycle_remainder = total_cycles % SystemTimers::TIMER_RATIO;
}

void StaticRecompCore::SyncIn()
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();

  std::memcpy(m_guest.gpr, ppc.gpr, sizeof(m_guest.gpr));
  for (int i = 0; i < 32; ++i)
  {
    std::memcpy(&m_guest.fpr[i], &ppc.ps[i].ps0, sizeof(u64));
    std::memcpy(&m_guest.ps1[i], &ppc.ps[i].ps1, sizeof(u64));
  }
  m_guest.pc = ppc.pc;
  m_guest.lr = ppc.spr[SPR_LR];
  m_guest.ctr = ppc.spr[SPR_CTR];
  m_guest.cr = ppc.cr.Get();
  m_guest.xer = ppc.GetXER().Hex;
  m_guest.fpscr = ppc.fpscr.Hex;
  m_guest.msr = ppc.msr.Hex;
  m_guest.srr0 = ppc.spr[SPR_SRR0];
  m_guest.srr1 = ppc.spr[SPR_SRR1];
  m_guest.dar = ppc.spr[SPR_DAR];
  m_guest.dsisr = ppc.spr[SPR_DSISR];
  m_guest.ear = ppc.spr[SPR_EAR];
  m_guest.hid2 = ppc.spr[SPR_HID2];
  for (int i = 0; i < 16; ++i)
    m_guest.sr[i] = ppc.sr[i];
  for (int i = 0; i < 8; ++i)
    m_guest.gqr[i] = ppc.spr[SPR_GQR0 + i];
  // Dolphin materializes TB lazily on read (spr[TL/TU] is a stale cache);
  // GetFakeTimeBase() is the live value, matching the interpreter's mftb.
  m_guest.timebase = m_system.GetSystemTimers().GetFakeTimeBase();
  const auto& core_timing = m_system.GetCoreTiming();
  m_timebase_cycle_remainder =
      (core_timing.GetTicks() - core_timing.GetFakeTBStartTicks()) % SystemTimers::TIMER_RATIO;
  m_guest.reserve_addr = ppc.reserve_address;
  m_guest.reserve_valid = ppc.reserve;
  m_guest.exception = 0;
  m_guest.program_exception = 0;
  m_guest.downcount = 0;  // charge accumulator, not a copy of ppc.downcount

  if (m_module && m_module->on_state_loaded)
    m_module->on_state_loaded(&m_guest);
}

void StaticRecompCore::SyncOut()
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();

  // Flush any cycle charge the module accumulated since the last flush
  // (matters on the mid-dispatch fallback path, where the interpreter step
  // that follows must see up-to-date time).
  ppc.downcount += static_cast<int>(m_guest.downcount);
  m_guest.downcount = 0;

  SetPPCStateFromGuestState(m_guest, ppc);
  // Timebase is owned by Dolphin's CoreTiming; native code cannot write it.

  if (ppc.msr.Hex != m_guest.msr)
  {
    ppc.msr.Hex = m_guest.msr;
    power_pc.MSRUpdated();
  }
  PowerPC::RoundingModeUpdated(ppc);
}

void StaticRecompCore::PropagateGuestMSR()
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  if (ppc.msr.Hex != m_guest.msr)
  {
    ppc.msr.Hex = m_guest.msr;
    power_pc.MSRUpdated();
  }
}
