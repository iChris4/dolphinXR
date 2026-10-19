// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Core/AgentServer/CodeAnalysis.h"

using namespace AgentServer;

namespace
{
class FakeMemory
{
public:
  void Put(u32 address, u32 word) { m_words[address] = word; }
  void PutString(u32 address, const std::string& text)
  {
    for (size_t i = 0; i < text.size() + 1; i += 4)
    {
      u32 word = 0;
      for (size_t j = 0; j < 4; ++j)
      {
        const u32 c = i + j < text.size() ? static_cast<u8>(text[i + j]) : 0;
        word |= c << (24 - 8 * j);
      }
      m_words[static_cast<u32>(address + i)] = word;
    }
  }
  ReadWordFn Reader() const
  {
    return [this](u32 address) -> std::optional<u32> {
      const auto it = m_words.find(address);
      if (it == m_words.end())
        return std::nullopt;
      return it->second;
    };
  }

private:
  std::map<u32, u32> m_words;
};
}  // namespace

TEST(CodeAnalysis, CountsTestInstructions)
{
  const u32 words[] = {
      0xFC010040,  // fcmpo cr0, f1, f0
      0xFC0D5000,  // fcmpu cr0, f13, f10
      0x38600000,  // li r3, 0
      0x38600001,  // li r3, 1
      0x38600002,  // li r3, 2: not a bool
      0x38000000,  // li r0, 0: not the result register
      0x4E800020,  // blr
      0x4D820020,  // beqlr
      0x4E800021,  // blrl: a call, not a return
      0x60000000,  // nop
  };
  const InstructionCounts counts = CountInstructions(words);
  EXPECT_EQ(counts.fcmp, 2u);
  EXPECT_EQ(counts.bool_results, 2u);
  EXPECT_EQ(counts.returns, 2u);

  const InstructionCounts none = CountInstructions({});
  EXPECT_EQ(none.fcmp, 0u);
  EXPECT_EQ(none.bool_results, 0u);
  EXPECT_EQ(none.returns, 0u);
}

TEST(CodeAnalysis, ReadsTheModuleList)
{
  // Two modules as Sonic Adventure DX loads them: the OS list head points at the first header,
  // and each header links to the next.
  FakeMemory memory;
  memory.Put(OS_MODULE_LIST_HEAD, 0x802CC040);

  memory.Put(0x802CC040, 1);           // id
  memory.Put(0x802CC044, 0x8123E820);  // next
  memory.Put(0x802CC048, 0);           // prev
  memory.Put(0x802CC04C, 3);           // numSections
  memory.Put(0x802CC050, 0x802CC08C);  // sectionInfoOffset
  memory.Put(0x802CC054, 0x802C9160);  // nameOffset
  memory.Put(0x802CC058, 0x2B);        // nameSize, with the NUL
  memory.Put(0x802CC05C, 3);           // version
  memory.Put(0x802CC08C, 0);           // section 0: empty
  memory.Put(0x802CC090, 0);
  memory.Put(0x802CC094, 0x802CC12D);  // section 1: code (bit 0)
  memory.Put(0x802CC098, 0x100);
  memory.Put(0x802CC09C, 0x804E07C0);  // section 2: data
  memory.Put(0x802CC0A0, 0x40);
  memory.PutString(0x802C9160, "D:\\gc\\projects\\sonic\\elf\\Release\\_Main.plf");

  memory.Put(0x8123E820, 0x20);
  memory.Put(0x8123E824, 0);  // the last module
  memory.Put(0x8123E828, 0x802CC040);
  memory.Put(0x8123E82C, 1);
  memory.Put(0x8123E830, 0x8123E86C);
  memory.Put(0x8123E834, 0x802C96C8);
  memory.Put(0x8123E838, 0x2B);
  memory.Put(0x8123E86C, 0x8123E8FD);
  memory.Put(0x8123E870, 0x200);
  memory.PutString(0x802C96C8, "D:\\gc\\projects\\sonic\\elf\\Release\\s_mot.plf");

  const std::vector<LoadedModule> modules = ReadLoadedModules(memory.Reader());
  ASSERT_EQ(modules.size(), 2u);
  EXPECT_EQ(modules[0].header, 0x802CC040u);
  EXPECT_EQ(modules[0].id, 1u);
  EXPECT_EQ(modules[0].name, "_Main.plf");
  ASSERT_EQ(modules[0].sections.size(), 3u);
  EXPECT_EQ(modules[0].sections[0].size, 0u);
  EXPECT_EQ(modules[0].sections[1].address, 0x802CC12Cu);
  EXPECT_EQ(modules[0].sections[1].size, 0x100u);
  EXPECT_TRUE(modules[0].sections[1].executable);
  EXPECT_EQ(modules[0].sections[2].address, 0x804E07C0u);
  EXPECT_FALSE(modules[0].sections[2].executable);
  EXPECT_EQ(modules[1].id, 0x20u);
  EXPECT_EQ(modules[1].name, "s_mot.plf");
  ASSERT_EQ(modules[1].sections.size(), 1u);

  const std::optional<ModuleLocation> in_code = FindModuleLocation(modules, 0x802CC12C + 0x44);
  ASSERT_TRUE(in_code);
  EXPECT_EQ(in_code->module, 0u);
  EXPECT_EQ(in_code->section, 1u);
  EXPECT_EQ(in_code->offset, 0x44u);
  const std::optional<ModuleLocation> in_second = FindModuleLocation(modules, 0x8123E8FC + 0x10);
  ASSERT_TRUE(in_second);
  EXPECT_EQ(in_second->module, 1u);
  EXPECT_EQ(in_second->section, 0u);
  // One past the end of a section, and the main executable.
  EXPECT_FALSE(FindModuleLocation(modules, 0x802CC12C + 0x100));
  EXPECT_FALSE(FindModuleLocation(modules, 0x80003100));
}

TEST(CodeAnalysis, StopsAtABrokenList)
{
  FakeMemory memory;
  EXPECT_TRUE(ReadLoadedModules(memory.Reader()).empty());

  memory.Put(OS_MODULE_LIST_HEAD, 0);
  EXPECT_TRUE(ReadLoadedModules(memory.Reader()).empty());

  // A module that links to itself, with no name and no sections.
  memory.Put(OS_MODULE_LIST_HEAD, 0x80400000);
  memory.Put(0x80400000, 7);
  memory.Put(0x80400004, 0x80400000);
  memory.Put(0x80400008, 0);
  memory.Put(0x8040000C, 0);
  memory.Put(0x80400010, 0x80400040);
  memory.Put(0x80400014, 0);
  memory.Put(0x80400018, 0);
  const std::vector<LoadedModule> modules = ReadLoadedModules(memory.Reader());
  ASSERT_EQ(modules.size(), 1u);
  EXPECT_EQ(modules[0].id, 7u);
  EXPECT_TRUE(modules[0].name.empty());
  EXPECT_TRUE(modules[0].sections.empty());
}
