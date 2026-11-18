// RecompCore: StaticRecomp lockstep differential hook.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace StaticRecompLockstep
{

bool LsHwAccessInScope(PowerPC::MMU& mmu, u32 ea)
{
  u32 phys = ea;
  if (const std::optional<u32> t = mmu.GetTranslatedAddress(ea))
    phys = *t;
  const bool is_gather = (phys & 0xFFFFF000u) == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS;
  const bool is_mmio = (phys & 0xF8000000u) == 0x08000000u;
  return is_gather || is_mmio;
}

void StaticRecompLockstepVerifier::LsJournalTrampoline(u32 offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  const u8* ram = verifier->m_core.m_guest.ram;
  const u32 ram_size = verifier->m_core.m_guest.ram_size;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = offset + i;
    if (off >= ram_size)
      break;
    verifier->m_journal.ram_pre.emplace(off, ram[off]);
  }
}

void StaticRecompLockstepVerifier::LsShadowJournalTrampoline(u32 offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  const u8* ram = verifier->m_core.m_guest.ram;
  const u32 ram_size = verifier->m_core.m_guest.ram_size;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = offset + i;
    if (off >= ram_size)
      break;
    verifier->m_journal.ram_shadow_pre.emplace(off, ram[off]);
  }
}

void StaticRecompLockstepVerifier::LsNativeLcJournalTrampoline(u32 lc_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  if (!l1)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = lc_offset + i;
    if (off >= l1_size)
      break;
    verifier->m_journal.lc_pre.emplace(off, l1[off]);
  }
}

void StaticRecompLockstepVerifier::LsShadowLcJournalTrampoline(u32 lc_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  if (!l1)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = lc_offset + i;
    if (off >= l1_size)
      break;
    verifier->m_journal.lc_shadow_pre.emplace(off, l1[off]);
  }
}

void StaticRecompLockstepVerifier::LsNativeVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();
  if (!vmem)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = vmem_offset + i;
    if (off >= vmem_size)
      break;
    verifier->m_journal.vmem_pre.emplace(off, vmem[off]);
  }
}

void StaticRecompLockstepVerifier::LsShadowVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();
  if (!vmem)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = vmem_offset + i;
    if (off >= vmem_size)
      break;
    verifier->m_journal.vmem_shadow_pre.emplace(off, vmem[off]);
  }
}

void StaticRecompLockstepVerifier::LsHwWriteTrampoline(u32 physical_address, u32 data, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  verifier->m_journal.interp_mmio.push_back({physical_address, data, size});
}

u32 StaticRecompLockstepVerifier::LsHwReadTrampoline(u32 physical_address, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  if (verifier->m_ls_read_index < verifier->m_journal.native_reads.size())
  {
    const LsWrite& rec = verifier->m_journal.native_reads[verifier->m_ls_read_index++];
    if ((rec.addr & 0x0FFFFFFFu) != (physical_address & 0x0FFFFFFFu))
      verifier->m_ls_read_overflow = true;
    return rec.data;
  }
  verifier->m_ls_read_overflow = true;
  return 0;
}

}  // namespace StaticRecompLockstep
