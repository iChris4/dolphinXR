// RecompCore: StaticRecomp lockstep differential hook.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Common/Logging/Log.h"

#include <cstdlib>
#include <cstdio>

namespace StaticRecompLockstep
{
HwWriteSink g_hw_write_sink = nullptr;
void* g_hw_write_sink_user = nullptr;
HwReadSink g_hw_read_sink = nullptr;
void* g_hw_read_sink_user = nullptr;
bool g_tb_override_active = false;
u64 g_tb_override_value = 0;
RamWriteJournal g_ram_write_journal = nullptr;
void* g_ram_write_journal_user = nullptr;
LcWriteJournal g_lc_write_journal = nullptr;
void* g_lc_write_journal_user = nullptr;
VmemWriteJournal g_vmem_write_journal = nullptr;
void* g_vmem_write_journal_user = nullptr;

StaticRecompLockstepVerifier::StaticRecompLockstepVerifier(StaticRecompCore& core)
    : m_core(core)
{
}

StaticRecompLockstepVerifier::~StaticRecompLockstepVerifier()
{
  if (m_lockstep)
  {
    std::fprintf(stderr,
                 "[lockstep] summary: checks=%llu reports=%llu skipped_fallback=%llu "
                 "skipped_zero=%llu undercharges=%llu max_deficit=%lld distinct_pcs=%zu\n",
                 (unsigned long long)m_ls_checks, (unsigned long long)m_ls_reports,
                 (unsigned long long)m_ls_skipped_fallback, (unsigned long long)m_ls_skipped_zero,
                 (unsigned long long)m_ls_undercharges, (long long)m_ls_max_undercharge,
                 m_ls_checked.size());
    if (m_set_mem_journal)
      m_set_mem_journal(nullptr, nullptr);
  }
}

void StaticRecompLockstepVerifier::Init()
{
  const char* enable = std::getenv("STATICRECOMP_LOCKSTEP");
  if (!enable || enable[0] == '0' || enable[0] == '\0')
    return;
  if (!m_core.m_module)
  {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: LOCKSTEP requested but no module loaded; ignored.");
    return;
  }
  m_set_mem_journal = reinterpret_cast<SetMemJournalFn>(m_core.m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
  if (!m_set_mem_journal)
  {
    std::fprintf(stderr, "[lockstep] module lacks ppc_set_mem_write_journal export; "
                         "lockstep DISABLED (rebuild the module).\n");
    return;
  }

  m_lockstep = true;
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_START"))
    m_ls_start = std::strtoull(s, nullptr, 0);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_LIMIT"))
    m_ls_limit = std::strtoull(s, nullptr, 0);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_MAXREPORT"))
    m_ls_max_report = std::strtoull(s, nullptr, 0);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_STEPCAP"))
    m_ls_step_cap = std::atoi(s);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_TRACE"))
    m_ls_trace_pc = static_cast<u32>(std::strtoull(s, nullptr, 0));
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_REPEAT"))
    m_ls_repeat_pc = static_cast<u32>(std::strtoull(s, nullptr, 0));
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_WHITELIST"))
  {
    const char* p = s;
    while (*p)
    {
      char* end = nullptr;
      const unsigned long long pc = std::strtoull(p, &end, 0);
      if (end == p)
        break;
      m_ls_whitelist.insert(static_cast<u32>(pc));
      p = end;
      while (*p == ',' || *p == ' ')
        ++p;
    }
  }

  std::fprintf(
      stderr,
      "[lockstep] ENABLED: start=%llu limit=%llu maxreport=%llu stepcap=%d whitelist=%zu\n",
      (unsigned long long)m_ls_start, (unsigned long long)m_ls_limit,
      (unsigned long long)m_ls_max_report, m_ls_step_cap, m_ls_whitelist.size());
}

bool StaticRecompLockstepVerifier::ShouldCheck(u32 address) const
{
  if (!m_lockstep)
    return false;
  if (!LockstepWindowOpen())
    return false;
  return address == m_ls_repeat_pc || m_ls_checked.find(address) == m_ls_checked.end();
}

bool StaticRecompLockstepVerifier::LockstepWindowOpen() const
{
  if (m_core.m_native_dispatches < m_ls_start)
    return false;
  if (m_ls_limit != 0 && m_core.m_native_dispatches >= m_ls_limit)
    return false;
  return true;
}

void StaticRecompLockstepVerifier::Prepare(const CPUState& guest)
{
  m_ls_entry = guest.pc;
  m_ls_snapshot = guest;
  m_journal.Clear();
  m_ls_fallback_seen = false;
  m_ls_journaling = true;
  m_set_mem_journal(&StaticRecompLockstepVerifier::LsJournalTrampoline, this);
  StaticRecompLockstep::g_ram_write_journal = &StaticRecompLockstepVerifier::LsJournalTrampoline;
  StaticRecompLockstep::g_ram_write_journal_user = this;
  StaticRecompLockstep::g_lc_write_journal = &StaticRecompLockstepVerifier::LsNativeLcJournalTrampoline;
  StaticRecompLockstep::g_lc_write_journal_user = this;
  StaticRecompLockstep::g_vmem_write_journal = &StaticRecompLockstepVerifier::LsNativeVmemJournalTrampoline;
  StaticRecompLockstep::g_vmem_write_journal_user = this;
}

void StaticRecompLockstepVerifier::Verify(const CPUState& guest)
{
  m_ls_journaling = false;
  m_set_mem_journal(nullptr, nullptr);
  StaticRecompLockstep::g_ram_write_journal = nullptr;
  StaticRecompLockstep::g_ram_write_journal_user = nullptr;
  StaticRecompLockstep::g_lc_write_journal = nullptr;
  StaticRecompLockstep::g_lc_write_journal_user = nullptr;
  StaticRecompLockstep::g_vmem_write_journal = nullptr;
  StaticRecompLockstep::g_vmem_write_journal_user = nullptr;
  m_ls_checked.insert(m_ls_entry);
  LockstepCheck(m_ls_entry, guest.pc, m_ls_snapshot);
}

}  // namespace StaticRecompLockstep
