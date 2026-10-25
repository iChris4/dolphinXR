// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Core/AgentServer/PointerScan.h"

using namespace AgentServer::PointerScan;

namespace
{
constexpr u32 BASE = 0x80000000;

// 64 KiB of fake MEM1.
class FakeMemory
{
public:
  FakeMemory() : m_bytes(0x10000) {}

  void Write(u32 address, u32 value)
  {
    const size_t at = address - BASE;
    m_bytes[at] = static_cast<u8>(value >> 24);
    m_bytes[at + 1] = static_cast<u8>(value >> 16);
    m_bytes[at + 2] = static_cast<u8>(value >> 8);
    m_bytes[at + 3] = static_cast<u8>(value);
  }
  std::optional<u32> Read(u32 address) const
  {
    if (address < BASE || address - BASE + 4 > m_bytes.size())
      return std::nullopt;
    const size_t at = address - BASE;
    return (u32{m_bytes[at]} << 24) | (u32{m_bytes[at + 1]} << 16) | (u32{m_bytes[at + 2]} << 8) |
           u32{m_bytes[at + 3]};
  }
  bool IsPointer(u32 value) const { return value >= BASE && value - BASE < m_bytes.size(); }
  std::vector<Region> Regions() const { return {{BASE, m_bytes}}; }

private:
  std::vector<u8> m_bytes;
};

// A field 0x40 into an object at 80008000, reached from two fixed words; a code word that looks
// like a pointer to the object must not count.
struct Scene
{
  static constexpr u32 OBJECT = 0x80008000;
  static constexpr u32 FIELD = OBJECT + 0x40;
  static constexpr u32 HOLDER = 0x80006000;
  static constexpr u32 DIRECT = 0x80001500;
  static constexpr u32 INDIRECT = 0x80001234 & ~3u;
  static constexpr u32 CODE_WORD = 0x80000200;
  static constexpr u32 TO_CODE = 0x80001600;

  Scene()
  {
    memory.Write(DIRECT, OBJECT);
    memory.Write(INDIRECT, HOLDER);
    memory.Write(HOLDER + 0x20, OBJECT);
    // lwz r0, -0x8000(0): the word 80008000.
    memory.Write(CODE_WORD, OBJECT);
    memory.Write(TO_CODE, CODE_WORD);
  }

  Result Find(const Options& options) const
  {
    const std::vector<Region> regions = memory.Regions();
    return AgentServer::PointerScan::Find(regions, FIELD, fixed, code, options);
  }

  FakeMemory memory;
  std::vector<Range> fixed = {{0x80001000, 0x80002000, "bss"}};
  std::vector<Range> code = {{0x80000100, 0x80000800, "text"}};
};
}  // namespace

TEST(AgentPointerScan, FormatsAndParsesPaths)
{
  const Path path{0x8067DC18, {0xC54, 0x9C}};
  EXPECT_EQ(FormatPath(path), "[[8067DC18]+C54]+9C");
  EXPECT_EQ(FormatLua(path), "mem.chain(0x8067DC18, 0xC54, 0x9C)");
  EXPECT_EQ(FormatPath({0x805C4F98, {0x2184, 0}}), "[[805C4F98]+2184]");
  EXPECT_EQ(FormatLua({0x805C4F98, {}}), "0x805C4F98");

  EXPECT_EQ(ParsePath("[[8067DC18]+C54]+9C"), path);
  EXPECT_EQ(ParsePath(" [ [0x8067DC18] + 0xC54 ] + 0x9C "), path);
  EXPECT_EQ(ParsePath("mem.chain(0x8067DC18, 3156, 0x9C)"), path);
  EXPECT_EQ(ParsePath("[805C4F90+8]+2184"), (Path{0x805C4F98, {0x2184}}));
  EXPECT_EQ(ParsePath("[[805C4F98]+2190-C]"), (Path{0x805C4F98, {0x2184, 0}}));
  EXPECT_EQ(ParsePath("805C4F98"), (Path{0x805C4F98, {}}));
  EXPECT_EQ(ParsePath(FormatPath({0x805C4F98, {0x2184, 0}})), (Path{0x805C4F98, {0x2184, 0}}));
  EXPECT_FALSE(ParsePath(""));
  EXPECT_FALSE(ParsePath("[[8067DC18]+C54"));
  EXPECT_FALSE(ParsePath("[80]+[90]"));
  EXPECT_FALSE(ParsePath("123456789"));
  EXPECT_FALSE(ParsePath("mem.chain(0x80, zz)"));
}

TEST(AgentPointerScan, ResolvesPaths)
{
  const Scene scene;
  const auto read = [&](u32 a) { return scene.memory.Read(a); };
  const auto is_pointer = [&](u32 v) { return scene.memory.IsPointer(v); };
  EXPECT_EQ(Resolve({Scene::INDIRECT, {0x20, 0x40}}, read, is_pointer).address, Scene::FIELD);
  EXPECT_EQ(Resolve({Scene::DIRECT, {}}, read, is_pointer).address, Scene::DIRECT);

  // [80006000+0x24] is 0: not a pointer, the path breaks at its second pointer.
  const Resolution broken = Resolve({Scene::INDIRECT, {0x24, 0x40}}, read, is_pointer);
  EXPECT_FALSE(broken.address);
  ASSERT_EQ(broken.steps.size(), 2u);
  EXPECT_EQ(broken.steps[1].address, Scene::HOLDER + 0x24);
  EXPECT_EQ(broken.steps[1].value, 0u);
}

TEST(AgentPointerScan, FindsPathsFromFixedAddresses)
{
  const Scene scene;
  const Result result = scene.Find({});
  EXPECT_FALSE(result.truncated);
  ASSERT_EQ(result.paths.size(), 2u);
  // Fewest pointers first.
  EXPECT_EQ(result.paths[0], (Path{Scene::DIRECT, {0x40}}));
  EXPECT_EQ(result.paths[1], (Path{Scene::INDIRECT, {0x20, 0x40}}));
  // The code word was not taken for a pointer: no path through it.
  for (const Path& path : result.paths)
    EXPECT_NE(path.base, Scene::TO_CODE);

  EXPECT_EQ(scene.Find({.max_depth = 1}).paths.size(), 1u);
  EXPECT_TRUE(scene.Find({.max_offset = 0x3C}).paths.empty());
  const Result capped = scene.Find({.max_results = 1});
  EXPECT_TRUE(capped.truncated);
  EXPECT_EQ(capped.paths.size(), 1u);
}

TEST(AgentPointerScan, ClassifiesWords)
{
  const auto is_pointer = [](u32 v) { return v >= 0x80000000 && v < 0x81800000; };
  EXPECT_EQ(Classify(0, is_pointer), WordKind::Zero);
  EXPECT_EQ(Classify(0x80A14740, is_pointer), WordKind::Pointer);
  EXPECT_EQ(Classify(0x3F800000, is_pointer), WordKind::Float);
  EXPECT_EQ(FormatWord(0x3E6B851F, WordKind::Float), "0.23");
  EXPECT_EQ(Classify(5, is_pointer), WordKind::Int);
  EXPECT_EQ(Classify(0xFFFFFFFF, is_pointer), WordKind::Int);
  EXPECT_EQ(FormatWord(0xFFFFFFFF, WordKind::Int), "-1");
  EXPECT_EQ(Classify(0x53616D75, is_pointer), WordKind::Text);
  EXPECT_EQ(FormatWord(0x53616D75, WordKind::Text), "'Samu'");
  EXPECT_EQ(Classify(0x12345678, is_pointer), WordKind::Hex);
}

TEST(AgentPointerScan, ParsesDolHeaders)
{
  std::array<u8, 0x100> header{};
  const auto put = [&](size_t at, u32 value) {
    header[at] = static_cast<u8>(value >> 24);
    header[at + 1] = static_cast<u8>(value >> 16);
    header[at + 2] = static_cast<u8>(value >> 8);
    header[at + 3] = static_cast<u8>(value);
  };
  put(0x48, 0x80003100);  // text0 address
  put(0x90, 0x1000);      // text0 size
  put(0x64, 0x80005000);  // data0 address
  put(0xAC, 0x200);       // data0 size
  put(0xD8, 0x80006000);  // bss
  put(0xDC, 0x100);
  put(0xE0, 0x80003100);  // entry point
  const std::optional<DolLayout> layout = ParseDolHeader(header, "main.dol");
  ASSERT_TRUE(layout);
  ASSERT_EQ(layout->text.size(), 1u);
  EXPECT_EQ(layout->text[0].start, 0x80003100u);
  EXPECT_EQ(layout->text[0].end, 0x80004100u);
  ASSERT_EQ(layout->data.size(), 2u);
  EXPECT_EQ(layout->data[0].name, "main.dol data0");
  EXPECT_EQ(layout->data[1].name, "main.dol bss");
  EXPECT_TRUE(layout->data[1].Contains(0x800060FC));
  EXPECT_FALSE(layout->data[1].Contains(0x80006100));

  put(0xE0, 0x12345678);
  EXPECT_FALSE(ParseDolHeader(header, "main.dol"));
}
