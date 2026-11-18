// RecompCore: StaticRecomp lockstep differential hook.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/System.h"
#include "Core/Core.h"
#include "Common/StringUtil.h"

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fmt/format.h>

namespace StaticRecompLockstep
{

namespace
{
constexpr s64 LS_UNDERCHARGE_GRACE = 1200;

bool LsIsLoopHeader(const u8* ram, u32 ram_size, u32 end_pc)
{
  constexpr u32 kBase = 0x80000000u;
  if (end_pc < kBase)
    return false;
  const u32 start_off = end_pc - kBase;
  constexpr u32 kWindowInsns = 2048;
  for (u32 i = 0; i < kWindowInsns; ++i)
  {
    const u32 off = start_off + i * 4u;
    if (off + 4u > ram_size)
      break;
    const u32 insn = Common::swap32(&ram[off]);
    const u32 opcd = insn >> 26;
    const u32 addr = end_pc + i * 4u;
    if (addr <= end_pc)
      continue;
    if (opcd == 16u && (insn & 0x2u) == 0u)  // bc: B-form, 14-bit signed BD, AA=0
    {
      const s32 bd = static_cast<s32>(static_cast<s16>(insn & 0xFFFCu));
      if (addr + static_cast<u32>(bd) == end_pc)
        return true;
    }
    else if (opcd == 18u && (insn & 0x2u) == 0u)  // b: I-form, 24-bit signed LI, AA=0
    {
      s32 li = static_cast<s32>(insn & 0x03FFFFFCu);
      if (li & 0x02000000)
        li |= static_cast<s32>(0xFC000000u);
      if (addr + static_cast<u32>(li) == end_pc)
        return true;
    }
  }
  return false;
}
}  // namespace

void StaticRecompLockstepVerifier::LoadEntryRegsToPPC(const CPUState& s)
{
  auto& power_pc = m_core.m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  StaticRecompCore::SetPPCStateFromGuestState(s, ppc);
  ppc.Exceptions = 0;
  ppc.msr.Hex = s.msr;
  power_pc.MSRUpdated();
  PowerPC::RoundingModeUpdated(ppc);
}

void StaticRecompLockstepVerifier::LockstepCheck(u32 entry_pc, u32 end_pc, const CPUState& entry_state)
{
  ++m_ls_checks;

  if (m_ls_fallback_seen)
  {
    ++m_ls_skipped_fallback;
    return;
  }

  auto& power_pc = m_core.m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interp = m_core.m_system.GetInterpreter();
  auto& memory = m_core.m_system.GetMemory();
  u8* ram = m_core.m_guest.ram;
  const u32 ram_size = m_core.m_guest.ram_size;
  u8* const l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  u8* const vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();

  const s64 native_charge = -m_core.m_guest.downcount;
  if (native_charge <= 0)
  {
    ++m_ls_skipped_zero;
    return;
  }

  m_journal.ram_post.clear();
  for (const auto& [off, pre] : m_journal.ram_pre)
    m_journal.ram_post.emplace(off, off < ram_size ? ram[off] : pre);
  for (const auto& [off, pre] : m_journal.ram_pre)
    if (off < ram_size)
      ram[off] = pre;

  m_journal.lc_post.clear();
  if (l1)
  {
    for (const auto& [off, pre] : m_journal.lc_pre)
      m_journal.lc_post.emplace(off, off < l1_size ? l1[off] : pre);
    for (const auto& [off, pre] : m_journal.lc_pre)
      if (off < l1_size)
        l1[off] = pre;
  }

  m_journal.vmem_post.clear();
  if (vmem)
  {
    for (const auto& [off, pre] : m_journal.vmem_pre)
      m_journal.vmem_post.emplace(off, off < vmem_size ? vmem[off] : pre);
    for (const auto& [off, pre] : m_journal.vmem_pre)
      if (off < vmem_size)
        vmem[off] = pre;
  }

  const u64 saved_msr = ppc.msr.Hex;
  const int saved_downcount = ppc.downcount;
  const u32 saved_exceptions = ppc.Exceptions;
  LoadEntryRegsToPPC(entry_state);

  m_ls_read_index = 0;
  m_ls_read_overflow = false;
  StaticRecompLockstep::g_hw_write_sink = &StaticRecompLockstepVerifier::LsHwWriteTrampoline;
  StaticRecompLockstep::g_hw_write_sink_user = this;
  StaticRecompLockstep::g_hw_read_sink = &StaticRecompLockstepVerifier::LsHwReadTrampoline;
  StaticRecompLockstep::g_hw_read_sink_user = this;
  StaticRecompLockstep::g_ram_write_journal = &StaticRecompLockstepVerifier::LsShadowJournalTrampoline;
  StaticRecompLockstep::g_ram_write_journal_user = this;
  StaticRecompLockstep::g_lc_write_journal = &StaticRecompLockstepVerifier::LsShadowLcJournalTrampoline;
  StaticRecompLockstep::g_lc_write_journal_user = this;
  StaticRecompLockstep::g_vmem_write_journal = &StaticRecompLockstepVerifier::LsShadowVmemJournalTrampoline;
  StaticRecompLockstep::g_vmem_write_journal_user = this;
  StaticRecompLockstep::g_tb_override_active = true;
  StaticRecompLockstep::g_tb_override_value = entry_state.timebase;

  if (m_ls_trace_pc != 0 && entry_pc == m_ls_trace_pc)
  {
    const auto dump = [&](const char* tag, u32 gaddr) {
      const u32 o = gaddr - 0x80000000u;
      std::fprintf(stderr, "[ls-trace] entry %s=0x%08X bytes:", tag, gaddr);
      for (u32 k = 0; k < 20 && o + k < ram_size; ++k)
        std::fprintf(stderr, " %02X", ram[o + k]);
      std::fprintf(stderr, "\n");
    };
    std::fprintf(stderr, "[ls-trace] ENTRY r3=0x%08X r4=0x%08X r5=0x%08X r13=0x%08X charge=%lld\n",
                 entry_state.gpr[3], entry_state.gpr[4], entry_state.gpr[5], entry_state.gpr[13],
                 (long long)native_charge);
    if ((entry_state.gpr[3] >> 28) == 8)
      dump("r3", entry_state.gpr[3]);
    if ((entry_state.gpr[4] >> 28) == 8)
      dump("r4", entry_state.gpr[4]);
  }
  int steps = 0;
  s64 interp_cycles = 0;
  const bool end_is_loop_header = LsIsLoopHeader(ram, ram_size, end_pc);
  while (steps < m_ls_step_cap)
  {
    const u32 before = ppc.pc;
    interp_cycles += interp.SingleStepInner();
    ++steps;

    if (m_ls_trace_pc != 0 && entry_pc == m_ls_trace_pc)
    {
      std::fprintf(stderr,
                   "[ls-trace] step %d: pc=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r13=0x%08X "
                   "msr=0x%08X xer=0x%08X cr=0x%08X lr=0x%08X ctr=0x%08X\n",
                   steps, before, ppc.gpr[3], ppc.gpr[4], ppc.gpr[5], ppc.gpr[13], ppc.msr.Hex,
                   ppc.GetXER().Hex, ppc.cr.Get(), ppc.spr[SPR_LR], ppc.spr[SPR_CTR]);
    }
    if (ppc.pc == end_pc)
    {
      // A native dispatch can fall through into a loop body and return at its
      // back-edge with PC set to the loop header. In that one case the shadow
      // must execute the inlined iteration before stopping. An arrival by any
      // branch (including a call to an address which also happens to be a loop
      // header) is already the native dispatch boundary.
      // ...and on a loop header, not until the shadow has done as much work as
      // the native block did. A generated chunk leaves an inlined loop at its
      // header once the block's charge crosses DOLRECOMP_C_LOOP_CYCLE_BUDGET
      // (emitter.c:324), so the iteration it stops on is timing-determined and
      // cannot be inferred from the code. Stopping at the first branch arrival
      // compares two different iterations of the same loop at the same PC,
      // which is the long-standing GPR divergence on both arm64 and x86-64.
      if (!end_is_loop_header)
        break;
      if (interp_cycles >= native_charge)
        break;
    }
    if (ppc.Exceptions != 0)
      break;
  }

  StaticRecompLockstep::g_hw_write_sink = nullptr;
  StaticRecompLockstep::g_hw_write_sink_user = nullptr;
  StaticRecompLockstep::g_hw_read_sink = nullptr;
  StaticRecompLockstep::g_hw_read_sink_user = nullptr;
  StaticRecompLockstep::g_ram_write_journal = nullptr;
  StaticRecompLockstep::g_ram_write_journal_user = nullptr;
  StaticRecompLockstep::g_lc_write_journal = nullptr;
  StaticRecompLockstep::g_lc_write_journal_user = nullptr;
  StaticRecompLockstep::g_vmem_write_journal = nullptr;
  StaticRecompLockstep::g_vmem_write_journal_user = nullptr;
  StaticRecompLockstep::g_tb_override_active = false;

  const bool reached = (ppc.pc == end_pc && ppc.Exceptions == 0);
  const bool undercharged = reached && (interp_cycles > native_charge + LS_UNDERCHARGE_GRACE) && !end_is_loop_header;

  std::string diff;
  const auto addu = [&](const std::string& name, u64 nval, u64 ival) {
    if (nval != ival)
      diff += fmt::format(" {}:N={:#x},I={:#x}", name, nval, ival);
  };

  if (!reached)
  {
    diff += fmt::format(" CTRLFLOW:N_end={:#010x},I_pc={:#010x},steps={},N_cyc={},I_cyc={}", end_pc,
                        ppc.pc, steps, native_charge, interp_cycles);
  }
  else
  {
    for (int r = 0; r < 32; ++r)
      addu(fmt::format("r{}", r), m_core.m_guest.gpr[r], ppc.gpr[r]);
    for (int r = 0; r < 32; ++r)
    {
      u64 n, i;
      std::memcpy(&n, &m_core.m_guest.fpr[r], sizeof(u64));
      std::memcpy(&i, &ppc.ps[r].ps0, sizeof(u64));
      addu(fmt::format("f{}", r), n, i);
      std::memcpy(&n, &m_core.m_guest.ps1[r], sizeof(u64));
      std::memcpy(&i, &ppc.ps[r].ps1, sizeof(u64));
      addu(fmt::format("ps1_{}", r), n, i);
    }
    addu("lr", m_core.m_guest.lr, ppc.spr[SPR_LR]);
    addu("ctr", m_core.m_guest.ctr, ppc.spr[SPR_CTR]);
    addu("cr", m_core.m_guest.cr, ppc.cr.Get());
    addu("xer", m_core.m_guest.xer, ppc.GetXER().Hex);
    addu("fpscr", m_core.m_guest.fpscr, ppc.fpscr.Hex);
    addu("msr", m_core.m_guest.msr, ppc.msr.Hex);
    addu("srr0", m_core.m_guest.srr0, ppc.spr[SPR_SRR0]);
    addu("srr1", m_core.m_guest.srr1, ppc.spr[SPR_SRR1]);
    addu("pc", m_core.m_guest.pc, ppc.pc);

    for (const auto& [off, post] : m_journal.ram_post)
    {
      const u8 iv = (off < ram_size) ? ram[off] : post;
      if (iv != post)
        diff += fmt::format(" mem[{:#010x}]:N={:#04x},I={:#04x}", 0x80000000u + off, post, iv);
    }

    if (l1)
    {
      for (const auto& [off, post] : m_journal.lc_post)
      {
        const u8 iv = (off < l1_size) ? l1[off] : post;
        if (iv != post)
          diff += fmt::format(" lc[{:#010x}]:N={:#04x},I={:#04x}", 0xE0000000u + off, post, iv);
      }
    }

    if (vmem)
    {
      for (const auto& [off, post] : m_journal.vmem_post)
      {
        const u8 iv = (off < vmem_size) ? vmem[off] : post;
        if (iv != post)
          diff += fmt::format(" vmem[{:#08x}]:N={:#04x},I={:#04x}", off, post, iv);
      }
    }

    const auto low = [](u32 v, u32 sz) { return sz >= 4 ? v : (v & ((1u << (sz * 8)) - 1)); };
    if (m_journal.native_mmio.size() != m_journal.interp_mmio.size())
    {
      diff += fmt::format(" mmio#:N={},I={}", m_journal.native_mmio.size(), m_journal.interp_mmio.size());
      for (const LsWrite& w : m_journal.native_mmio)
        diff += fmt::format(" N@{:#010x}/{}", w.addr, w.size);
      for (const LsWrite& w : m_journal.interp_mmio)
        diff += fmt::format(" I@{:#010x}/{}", w.addr, w.size);
    }
    else
    {
      for (size_t k = 0; k < m_journal.native_mmio.size(); ++k)
      {
        const LsWrite& a = m_journal.native_mmio[k];
        const LsWrite& b = m_journal.interp_mmio[k];
        if ((a.addr & 0x0FFFFFFFu) != (b.addr & 0x0FFFFFFFu) || a.size != b.size ||
            low(a.data, a.size) != low(b.data, b.size))
        {
          diff += fmt::format(" mmio[{}]:N={:#x}={:#x}/{},I={:#x}={:#x}/{}", k, a.addr,
                              low(a.data, a.size), a.size, b.addr, low(b.data, b.size), b.size);
        }
      }
    }
    if (m_ls_read_overflow)
      diff += " mmio-read-seq-divergence";
  }

  if (!diff.empty() && m_ls_whitelist.find(entry_pc) == m_ls_whitelist.end())
  {
    ++m_ls_reports;
    if (m_ls_max_report == 0 || m_ls_reports <= m_ls_max_report)
    {
      std::fprintf(stderr, "[lockstep] DIVERGE #%llu entry=0x%08X end=0x%08X:%s\n",
                   (unsigned long long)m_ls_reports, entry_pc, end_pc, diff.c_str());
    }
  }
  else if (undercharged)
  {
    ++m_ls_undercharges;
    const s64 deficit = interp_cycles - native_charge;
    if (deficit > m_ls_max_undercharge)
      m_ls_max_undercharge = deficit;
    if (m_ls_undercharges <= 64)
    {
      std::fprintf(stderr,
                   "[lockstep] UNDERCHARGE #%llu entry=0x%08X end=0x%08X: "
                   "N_cyc=%lld I_cyc=%lld deficit=%lld (regs/mem exact)\n",
                   (unsigned long long)m_ls_undercharges, entry_pc, end_pc,
                   (long long)native_charge, (long long)interp_cycles, (long long)deficit);
    }
  }

  for (const auto& [off, pre] : m_journal.ram_shadow_pre)
    if (off < ram_size)
      ram[off] = pre;
  for (const auto& [off, post] : m_journal.ram_post)
    if (off < ram_size)
      ram[off] = post;

  if (l1)
  {
    for (const auto& [off, pre] : m_journal.lc_shadow_pre)
      if (off < l1_size)
        l1[off] = pre;
    for (const auto& [off, post] : m_journal.lc_post)
      if (off < l1_size)
        l1[off] = post;
  }

  if (vmem)
  {
    for (const auto& [off, pre] : m_journal.vmem_shadow_pre)
      if (off < vmem_size)
        vmem[off] = pre;
    for (const auto& [off, post] : m_journal.vmem_post)
      if (off < vmem_size)
        vmem[off] = post;
  }

  ppc.msr.Hex = saved_msr;
  power_pc.MSRUpdated();
  ppc.downcount = saved_downcount;
  ppc.Exceptions = saved_exceptions;
  if (m_core.m_module->on_state_loaded)
    m_core.m_module->on_state_loaded(&m_core.m_guest);
}

}  // namespace StaticRecompLockstep
