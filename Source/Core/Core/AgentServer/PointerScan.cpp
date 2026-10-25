// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/PointerScan.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

namespace AgentServer::PointerScan
{
namespace
{
u32 ReadBE32(std::span<const u8> bytes, size_t offset)
{
  return (u32{bytes[offset]} << 24) | (u32{bytes[offset + 1]} << 16) |
         (u32{bytes[offset + 2]} << 8) | u32{bytes[offset + 3]};
}

bool InRanges(std::span<const Range> ranges, u32 address)
{
  return std::ranges::any_of(ranges,
                             [address](const Range& range) { return range.Contains(address); });
}

// The bracket notation as a list of operations: a value, then reads and additions.
class Parser final
{
public:
  explicit Parser(std::string_view text) : m_text(text) {}

  std::optional<Path> Parse()
  {
    if (!Expression() || (SkipSpaces(), m_pos != m_text.size()) || m_values != 1)
      return std::nullopt;
    return m_path;
  }

private:
  void SkipSpaces()
  {
    while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
      ++m_pos;
  }

  std::optional<u32> Hex()
  {
    SkipSpaces();
    if (m_text.substr(m_pos, 2) == "0x" || m_text.substr(m_pos, 2) == "0X")
      m_pos += 2;
    u64 value = 0;
    size_t digits = 0;
    while (m_pos < m_text.size() && std::isxdigit(static_cast<unsigned char>(m_text[m_pos])))
    {
      const int c = std::tolower(static_cast<unsigned char>(m_text[m_pos++]));
      value = value * 16 + static_cast<u64>(c <= '9' ? c - '0' : c - 'a' + 10);
      if (++digits > 8)
        return std::nullopt;
    }
    if (digits == 0)
      return std::nullopt;
    return static_cast<u32>(value);
  }

  void Add(u32 value)
  {
    if (m_path.offsets.empty())
      m_path.base += value;
    else
      m_path.offsets.back() += value;
  }

  // expression := term (('+' | '-') hex)*
  // term := '[' expression ']' | hex
  bool Expression()
  {
    SkipSpaces();
    if (m_pos < m_text.size() && m_text[m_pos] == '[')
    {
      ++m_pos;
      if (!Expression())
        return false;
      SkipSpaces();
      if (m_pos >= m_text.size() || m_text[m_pos] != ']')
        return false;
      ++m_pos;
      m_path.offsets.push_back(0);
    }
    else
    {
      const std::optional<u32> value = Hex();
      if (!value)
        return false;
      m_path.base = *value;
      ++m_values;
    }
    while (true)
    {
      SkipSpaces();
      if (m_pos >= m_text.size() || (m_text[m_pos] != '+' && m_text[m_pos] != '-'))
        return true;
      const bool minus = m_text[m_pos++] == '-';
      const std::optional<u32> value = Hex();
      if (!value)
        return false;
      Add(minus ? 0u - *value : *value);
    }
  }

  std::string_view m_text;
  size_t m_pos = 0;
  // Plain values seen: exactly one, the innermost.
  int m_values = 0;
  Path m_path;
};

std::optional<Path> ParseChain(std::string_view text)
{
  constexpr std::string_view prefix = "mem.chain(";
  if (!text.starts_with(prefix) || !text.ends_with(')'))
    return std::nullopt;
  text = text.substr(prefix.size(), text.size() - prefix.size() - 1);
  std::vector<u32> numbers;
  while (!text.empty())
  {
    const size_t comma = text.find(',');
    std::string item(text.substr(0, comma));
    std::erase_if(item, [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    // Lua numbers: hex with 0x, else decimal.
    const bool hex = item.starts_with("0x") || item.starts_with("0X");
    const std::string digits = hex ? item.substr(2) : item;
    if (digits.empty() || digits.size() > (hex ? 8u : 10u) ||
        !std::ranges::all_of(digits, [hex](char c) {
          return hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0 :
                       std::isdigit(static_cast<unsigned char>(c)) != 0;
        }))
    {
      return std::nullopt;
    }
    numbers.push_back(static_cast<u32>(std::stoull(digits, nullptr, hex ? 16 : 10)));
  }
  if (numbers.empty())
    return std::nullopt;
  return Path{numbers.front(), std::vector<u32>(numbers.begin() + 1, numbers.end())};
}
}  // namespace

std::string FormatPath(const Path& path)
{
  std::string text = fmt::format("{:08X}", path.base);
  for (const u32 offset : path.offsets)
  {
    text = "[" + text + "]";
    if (offset != 0)
      text += fmt::format("+{:X}", offset);
  }
  return text;
}

std::string FormatLua(const Path& path)
{
  if (path.offsets.empty())
    return fmt::format("0x{:08X}", path.base);
  std::string text = fmt::format("mem.chain(0x{:08X}", path.base);
  for (const u32 offset : path.offsets)
    text += offset ? fmt::format(", 0x{:X}", offset) : ", 0";
  return text + ")";
}

std::optional<Path> ParsePath(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  if (text.starts_with("mem.chain"))
    return ParseChain(text);
  return Parser(text).Parse();
}

Resolution Resolve(const Path& path, const ReadWordFn& read,
                   const std::function<bool(u32)>& is_pointer)
{
  Resolution resolution;
  u32 address = path.base;
  for (const u32 offset : path.offsets)
  {
    const std::optional<u32> value = read(address);
    resolution.steps.push_back({address, value});
    if (!value || !is_pointer(*value))
      return resolution;
    address = *value + offset;
  }
  resolution.address = address;
  return resolution;
}

Result Find(std::span<const Region> regions, u32 target, std::span<const Range> static_ranges,
            std::span<const Range> excluded, const Options& options)
{
  const auto is_target = [&](u32 value) {
    return std::ranges::any_of(regions, [value](const Region& region) {
      return value >= region.base && value - region.base < region.bytes.size();
    });
  };

  // Every pointer, by the address it points to.
  std::vector<Range> skipped(excluded.begin(), excluded.end());
  std::ranges::sort(skipped, {}, &Range::start);
  std::vector<std::pair<u32, u32>> index;  // (value, location)
  for (const Region& region : regions)
  {
    auto skip = skipped.begin();
    for (size_t offset = 0; offset + 4 <= region.bytes.size(); offset += 4)
    {
      const u32 location = region.base + static_cast<u32>(offset);
      while (skip != skipped.end() && skip->end <= location)
        ++skip;
      if (skip != skipped.end() && skip->Contains(location))
      {
        // Jump to the first word past the excluded range.
        offset = ((static_cast<size_t>(skip->end - region.base) + 3) & ~size_t{3}) - 4;
        continue;
      }
      const u32 value = ReadBE32(region.bytes, offset);
      if ((value & 3) == 0 && is_target(value))
        index.emplace_back(value, location);
    }
  }
  std::ranges::sort(index);

  Result result;
  result.pointers = index.size();

  // Breadth first from the target: a node is an address to reach, with the offset to add after
  // reading it to reach its parent.
  struct Node
  {
    u32 address;
    u32 offset;
    s64 parent;
  };
  std::vector<Node> nodes = {{target, 0, -1}};
  std::unordered_set<u32> visited = {target};
  std::vector<size_t> frontier = {0};
  const auto suffix = [&nodes](size_t node) {
    std::vector<u32> offsets;
    for (s64 at = static_cast<s64>(node); nodes[at].parent >= 0; at = nodes[at].parent)
      offsets.push_back(nodes[at].offset);
    return offsets;
  };

  for (u32 depth = 1; depth <= options.max_depth && !frontier.empty(); ++depth)
  {
    std::vector<size_t> next;
    for (const size_t node : frontier)
    {
      const u32 address = nodes[node].address;
      const u32 low = address >= options.max_offset ? address - options.max_offset : 0;
      for (auto it = std::ranges::lower_bound(index, std::pair<u32, u32>{low, 0});
           it != index.end() && it->first <= address; ++it)
      {
        const auto [value, location] = *it;
        const u32 offset = address - value;
        if (InRanges(static_ranges, location))
        {
          Path path{location, {offset}};
          const std::vector<u32> rest = suffix(node);
          path.offsets.insert(path.offsets.end(), rest.begin(), rest.end());
          result.paths.push_back(std::move(path));
          if (result.paths.size() >= options.max_results)
          {
            result.truncated = true;
            break;
          }
        }
        else if (depth < options.max_depth && visited.insert(location).second)
        {
          if (nodes.size() >= options.max_nodes)
          {
            result.truncated = true;
            continue;
          }
          nodes.push_back({location, offset, static_cast<s64>(node)});
          next.push_back(nodes.size() - 1);
        }
      }
      if (result.paths.size() >= options.max_results)
        break;
    }
    if (result.paths.size() >= options.max_results)
      break;
    frontier = std::move(next);
  }

  const auto total = [](const Path& path) {
    u64 sum = 0;
    for (const u32 offset : path.offsets)
      sum += offset;
    return sum;
  };
  std::ranges::stable_sort(result.paths, [&](const Path& a, const Path& b) {
    if (a.offsets.size() != b.offsets.size())
      return a.offsets.size() < b.offsets.size();
    return total(a) < total(b);
  });
  return result;
}

WordKind Classify(u32 word, const std::function<bool(u32)>& is_pointer)
{
  if (word == 0)
    return WordKind::Zero;
  if (is_pointer(word))
    return WordKind::Pointer;
  const float as_float = std::bit_cast<float>(word);
  if (std::isfinite(as_float) && std::abs(as_float) >= 1e-4f && std::abs(as_float) <= 1e7f)
    return WordKind::Float;
  const s32 as_int = static_cast<s32>(word);
  if (as_int > -0x10000 && as_int < 0x10000)
    return WordKind::Int;
  int letters = 0;
  bool printable = true;
  for (int shift = 24; shift >= 0; shift -= 8)
  {
    const char c = static_cast<char>((word >> shift) & 0xFF);
    printable &= c >= 0x20 && c <= 0x7E;
    letters += std::isalpha(static_cast<unsigned char>(c)) != 0;
  }
  if (printable && letters >= 2)
    return WordKind::Text;
  return WordKind::Hex;
}

const char* WordKindName(WordKind kind)
{
  switch (kind)
  {
  case WordKind::Zero:
    return "zero";
  case WordKind::Pointer:
    return "pointer";
  case WordKind::Float:
    return "float";
  case WordKind::Int:
    return "int";
  case WordKind::Text:
    return "text";
  case WordKind::Hex:
    return "hex";
  }
  return "hex";
}

std::string FormatWord(u32 word, WordKind kind)
{
  switch (kind)
  {
  case WordKind::Zero:
    return "0";
  case WordKind::Pointer:
    return fmt::format("{:08X}", word);
  case WordKind::Float:
    return fmt::format("{:.6g}", std::bit_cast<float>(word));
  case WordKind::Int:
    return fmt::format("{}", static_cast<s32>(word));
  case WordKind::Text:
  {
    std::string text = "'";
    for (int shift = 24; shift >= 0; shift -= 8)
      text += static_cast<char>((word >> shift) & 0xFF);
    return text + "'";
  }
  case WordKind::Hex:
    break;
  }
  return fmt::format("0x{:08X}", word);
}

std::optional<DolLayout> ParseDolHeader(std::span<const u8> header, std::string_view name)
{
  constexpr size_t TEXT_COUNT = 7;
  constexpr size_t DATA_COUNT = 11;
  if (header.size() < 0xE4)
    return std::nullopt;
  const auto word = [&](size_t offset) { return ReadBE32(header, offset); };
  const auto valid = [](u32 address, u32 size) {
    return address >= 0x80000000 && address < 0x81800000 && size < 0x01800000 &&
           address + size <= 0x81800000;
  };
  const u32 entry = word(0xE0);
  if (entry < 0x80000000 || entry >= 0x81800000)
    return std::nullopt;

  DolLayout layout;
  for (size_t i = 0; i < TEXT_COUNT + DATA_COUNT; ++i)
  {
    const u32 address = word(0x48 + i * 4);
    const u32 size = word(0x90 + i * 4);
    if (size == 0)
      continue;
    if (!valid(address, size))
      return std::nullopt;
    const bool text = i < TEXT_COUNT;
    (text ? layout.text : layout.data)
        .push_back(
            {address, address + size,
             fmt::format("{} {}{}", name, text ? "text" : "data", text ? i : i - TEXT_COUNT)});
  }
  const u32 bss = word(0xD8);
  const u32 bss_size = word(0xDC);
  if (bss_size != 0)
  {
    if (!valid(bss, bss_size))
      return std::nullopt;
    layout.data.push_back({bss, bss + bss_size, fmt::format("{} bss", name)});
  }
  if (layout.text.empty())
    return std::nullopt;
  return layout;
}
}  // namespace AgentServer::PointerScan
