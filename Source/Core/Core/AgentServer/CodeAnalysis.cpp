// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/CodeAnalysis.h"

#include <algorithm>

namespace AgentServer
{
InstructionCounts CountInstructions(std::span<const u32> words)
{
  InstructionCounts counts;
  for (const u32 word : words)
  {
    const u32 opcode = word >> 26;
    const u32 xo = (word >> 1) & 0x3FF;
    if (opcode == 63 && (xo == 0 || xo == 32))
      ++counts.fcmp;
    // addi r3, 0, 0 and addi r3, 0, 1
    if (word == 0x38600000u || word == 0x38600001u)
      ++counts.bool_results;
    // bclr with LK clear
    if ((word & 0xFC0007FFu) == 0x4C000020u)
      ++counts.returns;
  }
  return counts;
}

namespace
{
constexpr size_t MAX_MODULES = 64;
constexpr u32 MAX_SECTIONS = 64;
constexpr u32 MAX_NAME_SIZE = 256;

bool IsRamAddress(u32 address)
{
  return (address >= 0x80000000u && address < 0x81800000u) ||
         (address >= 0x90000000u && address < 0x94000000u);
}

bool IsRamPointer(u32 address)
{
  return IsRamAddress(address) && (address & 3) == 0;
}

std::string ReadModuleName(const ReadWordFn& read, u32 address, u32 size)
{
  std::string name;
  bool ended = false;
  for (u32 i = 0; i < size && !ended; i += 4)
  {
    const std::optional<u32> word = read(address + i);
    if (!word)
      break;
    for (int shift = 24; shift >= 0 && !ended; shift -= 8)
    {
      const char c = static_cast<char>((*word >> shift) & 0xFF);
      ended = c == '\0' || name.size() >= size;
      if (!ended)
        name.push_back(c);
    }
  }
  // The header keeps the build path (D:\gc\projects\sonic\elf\Release\_Main.plf).
  const size_t slash = name.find_last_of("\\/");
  if (slash != std::string::npos)
    name.erase(0, slash + 1);
  return name;
}
}  // namespace

std::vector<LoadedModule> ReadLoadedModules(const ReadWordFn& read)
{
  std::vector<LoadedModule> modules;
  std::optional<u32> header = read(OS_MODULE_LIST_HEAD);
  while (header && IsRamPointer(*header) && modules.size() < MAX_MODULES)
  {
    // OSModuleInfo: id, link.next, link.prev, numSections, sectionInfoOffset, nameOffset,
    // nameSize, version... The offsets are absolute addresses once the module is loaded.
    const u32 base = *header;
    const std::optional<u32> id = read(base);
    const std::optional<u32> next = read(base + 4);
    const std::optional<u32> num_sections = read(base + 12);
    const std::optional<u32> section_info = read(base + 16);
    const std::optional<u32> name_offset = read(base + 20);
    const std::optional<u32> name_size = read(base + 24);
    if (!id || !next || !num_sections || !section_info || !name_offset || !name_size ||
        *num_sections > MAX_SECTIONS || !IsRamPointer(*section_info))
    {
      break;
    }
    if (std::ranges::any_of(modules, [base](const LoadedModule& m) { return m.header == base; }))
      break;

    LoadedModule module;
    module.header = base;
    module.id = *id;
    if (IsRamAddress(*name_offset))
      module.name = ReadModuleName(read, *name_offset, std::min(*name_size, MAX_NAME_SIZE));
    for (u32 i = 0; i < *num_sections; ++i)
    {
      const std::optional<u32> offset = read(*section_info + i * 8);
      const std::optional<u32> size = read(*section_info + i * 8 + 4);
      if (!offset || !size)
        break;
      // Bit 0 of the offset marks an executable section.
      module.sections.push_back({*offset & ~1u, *size, (*offset & 1) != 0});
    }
    modules.push_back(std::move(module));
    header = next;
  }
  return modules;
}

std::optional<ModuleLocation> FindModuleLocation(std::span<const LoadedModule> modules, u32 address)
{
  for (size_t m = 0; m < modules.size(); ++m)
  {
    const std::vector<ModuleSection>& sections = modules[m].sections;
    for (size_t s = 0; s < sections.size(); ++s)
    {
      if (sections[s].size != 0 && address >= sections[s].address &&
          address - sections[s].address < sections[s].size)
      {
        return ModuleLocation{m, s, address - sections[s].address};
      }
    }
  }
  return std::nullopt;
}
}  // namespace AgentServer
