// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// find_pointer_paths and dump_struct: pointer paths from fixed addresses to an object (Cheat
// Engine's pointer scan), and the likely meaning of each word of a structure. Pure functions over
// copies of the emulated RAM, tested on their own.

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace AgentServer::PointerScan
{
// Big-endian bytes of emulated memory starting at `base`.
struct Region
{
  u32 base = 0;
  std::span<const u8> bytes;
};

// [start, end) with what it is ("main.dol .bss", "OS globals", a module...).
struct Range
{
  u32 start = 0;
  u32 end = 0;
  std::string name;

  bool Contains(u32 address) const { return address >= start && address < end; }
};

// A path in the notation of the VR patches' mem.chain(base, o1, ..., on): read the word at base,
// add o1, read the word there, add o2, ..., add on. No offsets: the base itself.
struct Path
{
  u32 base = 0;
  std::vector<u32> offsets;

  bool operator==(const Path&) const = default;
};

// "[[805C4F98]+2184]+3C".
std::string FormatPath(const Path& path);
// "mem.chain(0x805C4F98, 0x2184, 0x3C)".
std::string FormatLua(const Path& path);
// The bracket notation (hex, "0x" optional, any +/- offsets, e.g. "[[0x8067DC18]+0xC54]+0x9C" or
// "[805C4F98+10]") or "mem.chain(...)". Nothing when it does not parse.
std::optional<Path> ParsePath(std::string_view text);

// Reads a big-endian word; nothing when the address is not mapped.
using ReadWordFn = std::function<std::optional<u32>(u32 address)>;

struct Step
{
  // Where the step read, and what it read.
  u32 address = 0;
  std::optional<u32> value;
};
struct Resolution
{
  // The address the path leads to; nothing when a read on the way failed or did not give a
  // pointer (`is_pointer`).
  std::optional<u32> address;
  std::vector<Step> steps;
};
Resolution Resolve(const Path& path, const ReadWordFn& read,
                   const std::function<bool(u32)>& is_pointer);

struct Options
{
  // Pointers followed at most.
  u32 max_depth = 3;
  // Largest offset added after a pointer (the target is at most this far into the object).
  u32 max_offset = 0x1000;
  size_t max_results = 5000;
  // Addresses searched at most over all levels (the search stops there and reports truncated).
  size_t max_nodes = 400000;
};

struct Result
{
  // Fewest pointers first, then smallest offsets.
  std::vector<Path> paths;
  bool truncated = false;
  // The pointer words found in the regions.
  size_t pointers = 0;
};

// Searches `regions` for paths from a word in `static_ranges` to `target`. A pointer is an
// aligned word whose value is an aligned address inside one of `regions`; words inside
// `excluded` (code, whose lwz instructions look like pointers) are not pointers.
Result Find(std::span<const Region> regions, u32 target, std::span<const Range> static_ranges,
            std::span<const Range> excluded, const Options& options);

// The likely meaning of a word of a structure.
enum class WordKind : u8
{
  Zero,
  Pointer,
  Float,
  Int,
  Text,
  Hex,
};
WordKind Classify(u32 word, const std::function<bool(u32)>& is_pointer);
const char* WordKindName(WordKind kind);
// "1.5", "-3", "'Samu'", "0x00FF00FF"... (pointers are described by the caller).
std::string FormatWord(u32 word, WordKind kind);

// The data, bss and text ranges of a DOL from its 0x100-byte header (big-endian). Nothing when the
// header does not look like a DOL's.
struct DolLayout
{
  std::vector<Range> data;  // data sections and the bss
  std::vector<Range> text;
};
std::optional<DolLayout> ParseDolHeader(std::span<const u8> header, std::string_view name);
}  // namespace AgentServer::PointerScan
