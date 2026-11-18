// RecompCore: StaticRecomp CPU core - SMC and chunk validation.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/JitInterface.h"
#include "Common/Logging/Log.h"
#include <algorithm>
#include <cstdio>

namespace
{
u32 ReadGuestBE32(const u8* bytes)
{
  return (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) |
         (static_cast<u32>(bytes[2]) << 8) | bytes[3];
}
}

void StaticRecompCore::RefreshRelSections()
{
  if (!m_module || m_module->num_rel_modules == 0 || !m_guest.ram || m_guest.ram_size < 0x40)
    return;

  std::vector<ActiveRelSection> discovered;
  for (u32 module_index = 0; module_index < m_module->num_rel_modules; ++module_index)
  {
    const StaticRecompRelModule& module = m_module->rel_modules[module_index];
    const u64 table_end = static_cast<u64>(module.section_info_offset) +
                          static_cast<u64>(module.section_count) * 8;
    if (table_end > m_guest.ram_size)
      continue;
    for (u32 candidate = 0; static_cast<u64>(candidate) + table_end <= m_guest.ram_size;
         candidate += 4)
    {
      const u8* header = m_guest.ram + candidate;
      if (ReadGuestBE32(header) != module.module_id ||
          ReadGuestBE32(header + 0x0c) != module.section_count ||
          ReadGuestBE32(header + 0x10) != module.section_info_offset ||
          ReadGuestBE32(header + 0x1c) != module.version)
        continue;

      std::vector<ActiveRelSection> candidate_sections;
      bool valid = true;
      for (u32 section_index = 0; section_index < module.num_sections; ++section_index)
      {
        const StaticRecompRelSection& section = module.sections[section_index];
        const u8* entry = header + module.section_info_offset + section.section_index * 8;
        u32 runtime_start = ReadGuestBE32(entry) & ~1u;
        const u32 runtime_size = ReadGuestBE32(entry + 4);
        if (runtime_size != section.size || runtime_start == 0)
        {
          valid = false;
          break;
        }
        if (runtime_start < module.file_size)
          runtime_start += 0x80000000u + candidate;
        else if (runtime_start < m_guest.ram_size)
          runtime_start |= 0x80000000u;
        const u64 physical_start = static_cast<u64>(runtime_start - 0x80000000u);
        if (runtime_start < 0x80000000u || physical_start + section.size > m_guest.ram_size)
        {
          valid = false;
          break;
        }
        candidate_sections.push_back({module.module_id, section.section_index,
                                      section.linked_start, runtime_start, section.size});
      }
      if (valid)
      {
        discovered.insert(discovered.end(), candidate_sections.begin(), candidate_sections.end());
        break;
      }
    }
  }

  const bool changed = discovered.size() != m_active_rel_sections.size() ||
                       !std::equal(discovered.begin(), discovered.end(),
                                   m_active_rel_sections.begin(),
                                   [](const ActiveRelSection& left, const ActiveRelSection& right) {
                                     return left.module_id == right.module_id &&
                                            left.section_index == right.section_index &&
                                            left.linked_start == right.linked_start &&
                                            left.runtime_start == right.runtime_start &&
                                            left.size == right.size;
                                   });
  if (!changed)
    return;
  m_active_rel_sections = std::move(discovered);
  ++m_rel_mapping_generation;
  for (u32 i = 0; i < m_chunk_rel_sections.size(); ++i)
  {
    if (m_chunk_rel_sections[i] < 0)
      continue;
    if (m_chunk_state[i] == CHUNK_FAILED && m_failed_chunks != 0)
      --m_failed_chunks;
    m_chunk_state[i] = CHUNK_UNVERIFIED;
    m_effective_chunk_hashes[i] = m_module->chunk_hashes[i];
  }
}

bool StaticRecompCore::ResolveNativeAddress(u32 runtime_address, u32* linked_address,
                                            u32* rel_section_index)
{
  const auto resolve_active = [&]() {
    for (u32 i = 0; i < m_active_rel_sections.size(); ++i)
    {
      const ActiveRelSection& section = m_active_rel_sections[i];
      if (runtime_address >= section.runtime_start &&
          static_cast<u64>(runtime_address) < static_cast<u64>(section.runtime_start) + section.size)
      {
        *linked_address = section.linked_start + (runtime_address - section.runtime_start);
        if (rel_section_index)
          *rel_section_index = i;
        return true;
      }
    }
    return false;
  };
  if (resolve_active())
    return true;
  const int direct_index = GetAddressLookupIndex(runtime_address);
  if (direct_index >= 0 && direct_index < static_cast<int>(m_chunk_lookup_table.size()))
  {
    const int chunk = m_chunk_lookup_table[direct_index];
    if (chunk >= 0 && m_chunk_rel_sections[chunk] < 0)
    {
      *linked_address = runtime_address;
      if (rel_section_index)
        *rel_section_index = 0xffffffffu;
      return true;
    }
  }
  RefreshRelSections();
  return resolve_active();
}

bool StaticRecompCore::ResolveRuntimeAddress(u32 linked_address, u32* runtime_address) const
{
  for (const ActiveRelSection& section : m_active_rel_sections)
  {
    if (linked_address >= section.linked_start &&
        static_cast<u64>(linked_address) < static_cast<u64>(section.linked_start) + section.size)
    {
      *runtime_address = section.runtime_start + (linked_address - section.linked_start);
      return true;
    }
  }
  *runtime_address = linked_address;
  return true;
}

u32 StaticRecompCore::TranslateRelAddress(u32 linked_address)
{
  u32 runtime_address = linked_address;
  ResolveRuntimeAddress(linked_address, &runtime_address);
  return runtime_address;
}

int StaticRecompCore::GetAddressLookupIndex(u32 address) const
{
  if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    return static_cast<int>((address - 0x80000000u) >> 2);
  if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    return static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
  return -1;
}

void StaticRecompCore::InitLookupTable(u32 ram_size, u32 exram_size)
{
  if (m_lookup_ram_size == ram_size && m_lookup_exram_size == exram_size)
    return;

  m_lookup_ram_size = ram_size;
  m_lookup_exram_size = exram_size;

  if (!m_module)
  {
    m_chunk_lookup_table.clear();
    return;
  }

  u32 total_instructions = (ram_size + exram_size) >> 2;
  m_chunk_lookup_table.assign(total_instructions, -1);

  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    int start_idx = GetAddressLookupIndex(chunk.start);
    int end_idx = GetAddressLookupIndex(chunk.end);

    if (start_idx >= 0 && end_idx >= start_idx)
    {
      for (int idx = start_idx; idx < end_idx; ++idx)
      {
        m_chunk_lookup_table[idx] = static_cast<int>(i);
      }
    }
  }
}

int StaticRecompCore::ChunkIndexOf(u32 address)
{
  if (!m_module_active || m_chunk_lookup_table.empty())
    return -1;

  u32 linked_address = address;
  // A DOL-only module has no runtime-to-linked REL translation to perform.
  // Avoid the resolver on this per-block hot path while retaining the exact
  // existing path for modules that declare relocatable sections.
  if (m_module->num_rel_modules != 0 &&
      !ResolveNativeAddress(address, &linked_address, nullptr))
    return -1;
  int idx = GetAddressLookupIndex(linked_address);
  if (idx < 0 || idx >= static_cast<int>(m_chunk_lookup_table.size()))
    return -1;

  return m_chunk_lookup_table[idx];
}

bool StaticRecompCore::FastDispatchableAt(u32 address)
{
  if (IsForcedFallbackAddress(address))
    return false;
  const int index = ChunkIndexOf(address);
  return index >= 0 && m_chunk_state[index] == CHUNK_VERIFIED;
}

bool StaticRecompCore::DispatchableAt(u32 address)
{
  if (IsForcedFallbackAddress(address))
    return false;
  const int index = ChunkIndexOf(address);
  if (index < 0)
    return false;
  if (m_chunk_state[index] == CHUNK_UNVERIFIED)
    VerifyChunk(static_cast<u32>(index));
  return m_chunk_state[index] == CHUNK_VERIFIED;
}

bool StaticRecompCore::IsForcedFallbackAddress(u32 address) const
{
  for (const StaticRecompRange& range : m_forced_fallback_ranges)
  {
    if (address >= range.start && address < range.end)
      return true;
  }
  return false;
}

bool StaticRecompCore::ChunkContainsHostCall(u32 index) const
{
  if (!m_module_source.host_call_contains || index >= m_chunk_host_call_state.size())
    return false;

  u8& state = m_chunk_host_call_state[index];
  if (state != 0)
    return state == 2;

  const auto& chunk = m_module->chunk_ranges[index];
  bool found = false;
  if (m_module_source.host_call_range_contains)
  {
    found = m_module_source.host_call_range_contains(
        chunk.start, chunk.end, m_module_source.host_call_user);
  }
  else
  {
    for (u32 address = chunk.start; address < chunk.end; address += 4)
    {
      if (IsHostCallAddress(address))
      {
        found = true;
        break;
      }
    }
  }
  state = found ? 2 : 1;
  if (found)
    std::fprintf(stderr, "[staticrecomp] mod fallback: chunk [0x%08X,0x%08X)\n", chunk.start,
                 chunk.end);
  return found;
}

void StaticRecompCore::VerifyChunk(u32 index)
{
  const auto& chunk = m_module->chunk_ranges[index];
  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  u32 runtime_start = chunk.start;
  ResolveRuntimeAddress(chunk.start, &runtime_start);
  const u32 offset = runtime_start - 0x80000000u;
  const u32 length = chunk.end - chunk.start;
  ++m_verifications;

  if (runtime_start < 0x80000000u || offset >= ram_size || length > ram_size - offset)
  {
    m_chunk_state[index] = CHUNK_FAILED;
    ++m_failed_chunks;
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: chunk [0x{:08X},0x{:08X}) outside guest RAM",
                  chunk.start, chunk.end);
    return;
  }

  // FNV-1a 64, matching gen_module_tables.py.
  const u8* bytes = memory.GetRAM() + offset;
  u64 hash = 0xCBF29CE484222325ull;
  for (u32 i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }

  if (m_effective_chunk_hashes[index] == 0)
    m_effective_chunk_hashes[index] = hash;

  if (hash == m_effective_chunk_hashes[index])
  {
    m_chunk_state[index] = CHUNK_VERIFIED;
  }
  else
  {
    m_chunk_state[index] = CHUNK_FAILED;
    ++m_failed_chunks;
    std::fprintf(stderr,
                 "[staticrecomp] SMC: chunk [0x%08X,0x%08X) hash mismatch; interpreter until "
                 "next invalidation (%u failed)\n",
                 chunk.start, chunk.end, m_failed_chunks);
    WARN_LOG_FMT(POWERPC,
                 "StaticRecomp: chunk [0x{:08X},0x{:08X}) failed verification (guest code "
                 "differs from module); interpreter until next invalidation",
                 chunk.start, chunk.end);
  }
}

void StaticRecompCore::OnICacheInvalidate(u32 address, u32 length)
{
  if (m_fallback_jit)
  {
    m_fallback_jit->GetBlockCache()->InvalidateICache(address, length, false);
  }

  if (!m_module_active || length == 0)
    return;
  u32 linked_address = address;
  ResolveNativeAddress(address, &linked_address, nullptr);
  address = linked_address;
  const u32 last = address + (length - 1u);

  // Binary search to find the first chunk index that could possibly overlap (chunk.end > address)
  u32 lo = 0;
  u32 hi = m_module->num_chunk_ranges;
  while (lo < hi)
  {
    const u32 mid = lo + (hi - lo) / 2;
    if (m_module->chunk_ranges[mid].end <= address)
      lo = mid + 1;
    else
      hi = mid;
  }

  for (u32 i = lo; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    if (chunk.start > last)
      break;

    if (m_chunk_state[i] != CHUNK_UNVERIFIED)
    {
      if (m_chunk_state[i] == CHUNK_FAILED)
        --m_failed_chunks;
      m_chunk_state[i] = CHUNK_UNVERIFIED;
      ++m_reverify_events;
    }
  }
}
