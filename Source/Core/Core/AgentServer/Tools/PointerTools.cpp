// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Memory helpers: pointer paths from fixed addresses to an object (find_pointer_paths), structure
// dumps that tell pointers, floats and integers apart (dump_struct), and the "path" argument that
// reads through such a path.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/CodeAnalysis.h"
#include "Core/AgentServer/PointerScan.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace AgentServer
{
namespace
{
using PointerScan::Path;
using PointerScan::Range;

// The OS globals end where a DOL may start.
constexpr u32 OS_GLOBALS_END = 0x80003100;
constexpr size_t MAX_SAVED_PATHS = 5000;

struct PathSet
{
  u32 target = 0;
  std::vector<Path> paths;
};
struct Dump
{
  u32 address = 0;
  std::vector<std::optional<u32>> words;
};
struct DolCache
{
  std::string path;
  std::string game_id;
  std::optional<PointerScan::DolLayout> layout;
};

std::mutex s_mutex;
std::map<std::string, PathSet> s_path_sets;
std::map<std::string, Dump> s_dumps;
DolCache s_dol_cache;

// What stays where it is: the OS globals, the executable's data and bss, the data of the loaded
// REL modules. And the code, whose lwz instructions look like pointers.
struct FixedLayout
{
  std::vector<Range> fixed;
  std::vector<Range> code;
  std::string source;
};

PointerScan::ReadWordFn Reader(const Core::CPUThreadGuard& guard)
{
  return [&guard](u32 address) -> std::optional<u32> {
    const auto word = PowerPC::MMU::HostTryRead<u32>(guard, address);
    if (!word)
      return std::nullopt;
    return word->value;
  };
}

// An address inside the emulated RAM.
bool IsRamAddress(u32 value)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  if (value >= Memory::MEM1_BASE_ADDR && value - Memory::MEM1_BASE_ADDR < memory.GetRamSizeReal())
    return true;
  return GetSystem().IsWii() && value >= Memory::MEM2_BASE_ADDR &&
         value - Memory::MEM2_BASE_ADDR < memory.GetExRamSizeReal();
}

bool IsFunctionStart(u32 address)
{
  const Common::Symbol* symbol = GetSystem().GetPPCSymbolDB().GetSymbolFromAddr(address);
  return symbol && symbol->address == address;
}

// The layout of the executable the game was booted with, from its DOL header (on the disc, or the
// .dol file). Cached per file and game.
std::optional<PointerScan::DolLayout> ReadBootDol()
{
  const std::string path = Server::GetInstance().GetLastBootPath();
  const std::string game_id = SConfig::GetInstance().GetGameID();
  if (path.empty())
    return std::nullopt;
  {
    std::lock_guard lock(s_mutex);
    if (s_dol_cache.path == path && s_dol_cache.game_id == game_id)
      return s_dol_cache.layout;
  }

  std::optional<PointerScan::DolLayout> layout;
  std::array<u8, 0x100> header{};
  std::string extension;
  SplitPath(path, nullptr, nullptr, &extension);
  Common::ToLower(&extension);
  if (extension == ".dol")
  {
    File::IOFile file(path, "rb");
    if (file.ReadBytes(header.data(), header.size()))
      layout = PointerScan::ParseDolHeader(header, PathToFileName(path));
  }
  else if (const std::unique_ptr<DiscIO::VolumeDisc> volume = DiscIO::CreateDisc(path))
  {
    const DiscIO::Partition partition = volume->GetGamePartition();
    const std::optional<u64> offset = DiscIO::GetBootDOLOffset(*volume, partition);
    if ((volume->GetGameID() == game_id || volume->GetGameID(partition) == game_id) && offset &&
        volume->Read(*offset, header.size(), header.data(), partition))
    {
      layout = PointerScan::ParseDolHeader(header, "main.dol");
    }
  }

  std::lock_guard lock(s_mutex);
  s_dol_cache = {path, game_id, layout};
  return layout;
}

FixedLayout ReadFixedLayout(const Core::CPUThreadGuard& guard,
                            const std::optional<PointerScan::DolLayout>& dol,
                            const std::vector<std::string>& static_ranges)
{
  FixedLayout layout;
  layout.fixed.push_back({Memory::MEM1_BASE_ADDR, OS_GLOBALS_END, "OS globals"});
  const auto& ppc = GetSystem().GetPPCState();
  const u32 r13 = ppc.gpr[13];
  const u32 r2 = ppc.gpr[2];

  const bool dol_runs =
      dol && std::ranges::any_of(dol->data, [r13](const Range& r) { return r.Contains(r13); });
  if (!static_ranges.empty())
  {
    for (const std::string& text : static_ranges)
    {
      const size_t dash = text.find('-');
      const auto start = PointerScan::ParsePath(text.substr(0, dash));
      const auto end =
          dash == std::string::npos ? std::nullopt : PointerScan::ParsePath(text.substr(dash + 1));
      if (!start || !end || !start->offsets.empty() || !end->offsets.empty() ||
          end->base <= start->base)
      {
        throw ToolError(fmt::format("static_ranges: \"{}\" is not a range like "
                                    "\"80004000-8067E000\".",
                                    text));
      }
      layout.fixed.push_back({start->base, end->base, text});
    }
    layout.source = "the static_ranges given";
  }
  else if (dol_runs)
  {
    layout.fixed.insert(layout.fixed.end(), dol->data.begin(), dol->data.end());
    layout.source = "the boot executable's data and bss (DOL header)";
  }
  else
  {
    // Another executable runs (a launcher loaded the game's own, as in Metroid Prime Trilogy), or
    // the game was not booted from a file. CodeWarrior links .data, .bss, .sdata and .sbss in that
    // order below .sdata2, which starts at r2 - 0x8000 (r13 points into .sdata), and the code
    // below them.
    if (!IsRamAddress(r13) || !IsRamAddress(r2) || r2 <= r13)
    {
      throw ToolError("The fixed data of this game is unknown (the executable is not the boot "
                      "file, and r13/r2 do not point to small data): give static_ranges.");
    }
    u32 code_end = OS_GLOBALS_END;
    GetSystem().GetPPCSymbolDB().ForEachSymbol([&](const Common::Symbol& symbol) {
      if (symbol.address >= OS_GLOBALS_END && symbol.address + symbol.size <= r13 - 0x8000)
        code_end = std::max(code_end, symbol.address + symbol.size);
    });
    layout.fixed.push_back({code_end, r2 - 0x8000, "data and bss"});
    layout.source = fmt::format("estimated: {:08X}-{:08X}, between the code and r2 - 0x8000 (the "
                                "boot file is not the executable that runs)",
                                code_end, r2 - 0x8000);
  }
  if (dol_runs)
  {
    layout.code.insert(layout.code.end(), dol->text.begin(), dol->text.end());
  }
  else
  {
    // The boot DOL is not what runs: the code from the symbol map.
    GetSystem().GetPPCSymbolDB().ForEachSymbol([&](const Common::Symbol& symbol) {
      if (symbol.size != 0)
        layout.code.push_back({symbol.address, symbol.address + symbol.size, "code"});
    });
  }

  for (const LoadedModule& module : ReadLoadedModules(Reader(guard)))
  {
    const std::string name =
        module.name.empty() ? fmt::format("REL {}", module.id) : fmt::format("{}", module.name);
    for (const ModuleSection& section : module.sections)
    {
      if (section.address == 0 || section.size == 0)
        continue;
      (section.executable ? layout.code : layout.fixed)
          .push_back({section.address, section.address + section.size, name});
    }
  }
  return layout;
}

const Range* FindRange(const std::vector<Range>& ranges, u32 address)
{
  const auto it =
      std::ranges::find_if(ranges, [address](const Range& r) { return r.Contains(address); });
  return it == ranges.end() ? nullptr : &*it;
}

// What a pointer in a structure points to: a function, a vtable, an object with a vtable, fixed
// data, or the first word of what it points to.
std::string DescribePointee(const Core::CPUThreadGuard& guard, u32 pointer,
                            const FixedLayout& layout)
{
  const auto read = Reader(guard);
  if (FindRange(layout.code, pointer))
  {
    const std::string symbol = SymbolAt(pointer);
    return symbol.empty() ? "code" : "code " + symbol;
  }
  const std::optional<u32> first = read(pointer);
  // CodeWarrior vtables: the type info, an offset, then the functions.
  const auto is_vtable = [&](u32 address) {
    const std::optional<u32> function = read(address + 8);
    return function && IsFunctionStart(*function);
  };
  if (is_vtable(pointer))
    return "vtable";
  if (first && IsRamAddress(*first) && is_vtable(*first))
    return fmt::format("object, vtable {:08X}", *first);
  std::string where;
  if (const Range* range = FindRange(layout.fixed, pointer))
    where = range->name + ", ";
  if (!first)
    return where + "unreadable";
  const PointerScan::WordKind kind = PointerScan::Classify(*first, IsRamAddress);
  return fmt::format("{}first word {} {}", where, PointerScan::WordKindName(kind),
                     PointerScan::FormatWord(*first, kind));
}

ToolResult FindPointerPaths(const ToolArgs& args)
{
  RequireGame();
  std::string text = EnsureSymbols();
  if (!text.empty())
    text += "\n";
  if (const std::string check = args.GetString("check"); !check.empty())
  {
    PathSet set;
    {
      std::lock_guard lock(s_mutex);
      const auto it = s_path_sets.find(check);
      if (it == s_path_sets.end())
        throw ToolError(fmt::format("There are no paths saved as \"{}\".", check));
      set = it->second;
    }
    const size_t show = static_cast<size_t>(std::clamp<s64>(args.GetInt("show", 25), 1, 200));
    std::optional<u32> target;
    std::vector<std::optional<u32>> destinations;
    {
      Core::CPUThreadGuard guard(GetSystem());
      if (args.Has("address") || args.Has("path"))
        target = ResolveAddressOrPath(guard, args, nullptr);
      const auto read = Reader(guard);
      for (const Path& path : set.paths)
        destinations.push_back(PointerScan::Resolve(path, read, IsRamAddress).address);
    }

    picojson::array kept_json;
    if (target)
    {
      PathSet kept{*target, {}};
      for (size_t i = 0; i < set.paths.size(); ++i)
      {
        if (destinations[i] == target)
          kept.paths.push_back(set.paths[i]);
      }
      const std::string label = args.GetString("label", check);
      text += fmt::format("{} of the {} paths saved as \"{}\" (to {:08X}) lead to {:08X} now. "
                          "Saved as \"{}\".\n",
                          kept.paths.size(), set.paths.size(), check, set.target, *target, label);
      for (size_t i = 0; i < std::min(show, kept.paths.size()); ++i)
      {
        const Path& path = kept.paths[i];
        text += fmt::format("  {:<36} {}\n", PointerScan::FormatPath(path),
                            PointerScan::FormatLua(path));
        picojson::object entry;
        entry["text"] = JsonString(PointerScan::FormatPath(path));
        entry["lua"] = JsonString(PointerScan::FormatLua(path));
        kept_json.push_back(picojson::value(std::move(entry)));
      }
      if (kept.paths.empty())
      {
        text += "None: the object is reached another way in this state, or the address is not "
                "the same object. Search again here with find_pointer_paths address=<it>.";
      }
      else if (kept.paths.size() > 1)
      {
        text += "Check them in one more state (another room, after a reload) to keep the ones "
                "that always work.";
      }
      std::lock_guard lock(s_mutex);
      s_path_sets[label] = std::move(kept);
    }
    else
    {
      // Where the paths lead now, most common first.
      std::map<u32, size_t> counts;
      size_t broken = 0;
      for (const std::optional<u32>& destination : destinations)
      {
        if (destination)
          ++counts[*destination];
        else
          ++broken;
      }
      std::vector<std::pair<u32, size_t>> sorted(counts.begin(), counts.end());
      std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
      text += fmt::format("Where the {} paths saved as \"{}\" (to {:08X}) lead now ({} broken):\n",
                          set.paths.size(), check, set.target, broken);
      for (size_t i = 0; i < std::min(show, sorted.size()); ++i)
      {
        text += fmt::format("  {:08X} x{}\n", sorted[i].first, sorted[i].second);
        picojson::object entry = AddressJson(sorted[i].first);
        entry["paths"] = JsonNumber(static_cast<double>(sorted[i].second));
        kept_json.push_back(picojson::value(std::move(entry)));
      }
      text += "Pass address=<where the object is now> to keep the paths that reach it.";
    }
    picojson::object structured;
    structured["checked"] = JsonNumber(static_cast<double>(set.paths.size()));
    structured[target ? "paths" : "destinations"] = picojson::value(std::move(kept_json));
    return ToolResult::Text(std::move(text)).SetStructured(std::move(structured));
  }

  PointerScan::Options options;
  options.max_depth = static_cast<u32>(std::clamp<s64>(args.GetInt("max_depth", 3), 1, 5));
  options.max_offset =
      static_cast<u32>(std::clamp<s64>(args.GetInt("max_offset", 0x1000), 4, 0x10000));
  options.max_results = MAX_SAVED_PATHS;
  const size_t show = static_cast<size_t>(std::clamp<s64>(args.GetInt("show", 25), 1, 200));
  const std::string label = args.GetString("label", "paths");
  const std::optional<PointerScan::DolLayout> dol = ReadBootDol();

  // A copy of the RAM, so the game only pauses for the copy.
  std::vector<u8> mem1;
  std::vector<u8> mem2;
  FixedLayout layout;
  u32 target = 0;
  std::string how;
  {
    Core::CPUThreadGuard guard(GetSystem());
    target = ResolveAddressOrPath(guard, args, &how);
    layout = ReadFixedLayout(guard, dol, args.GetStringArray("static_ranges"));
    Memory::MemoryManager& memory = GetSystem().GetMemory();
    mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSizeReal());
    if (GetSystem().IsWii() && memory.GetEXRAM())
      mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSizeReal());
  }
  if (const Range* range = FindRange(layout.fixed, target))
  {
    return ToolResult::Text(
        text + fmt::format("{:08X} is itself at a fixed address ({}).", target, range->name));
  }

  std::vector<PointerScan::Region> regions = {{Memory::MEM1_BASE_ADDR, mem1}};
  if (!mem2.empty())
    regions.push_back({Memory::MEM2_BASE_ADDR, mem2});
  const auto start = std::chrono::steady_clock::now();
  PointerScan::Result result =
      PointerScan::Find(regions, target, layout.fixed, layout.code, options);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  text += fmt::format("Pointer paths to {:08X}{} (at most {} pointers, offsets up to 0x{:X}): {}{} "
                      "found in {:.1f} s among {} pointers in RAM.\n",
                      target, how.empty() ? "" : " (" + how + ")", options.max_depth,
                      options.max_offset, result.paths.size(), result.truncated ? "+" : "", seconds,
                      result.pointers);
  text += fmt::format("Fixed addresses: {}, the OS globals and the loaded modules' data.\n",
                      layout.source);
  picojson::array paths_json;
  for (size_t i = 0; i < std::min(show, result.paths.size()); ++i)
  {
    const Path& path = result.paths[i];
    const Range* base_range = FindRange(layout.fixed, path.base);
    const std::string base_symbol = SymbolAt(path.base);
    text += fmt::format("  {:<36} {:<44} {}{}\n", PointerScan::FormatPath(path),
                        PointerScan::FormatLua(path), base_range ? base_range->name : "",
                        base_symbol.empty() ? "" : " " + base_symbol);
    picojson::object entry = AddressJson(path.base);
    picojson::array offsets;
    for (const u32 offset : path.offsets)
      offsets.push_back(JsonNumber(offset));
    entry["offsets"] = picojson::value(std::move(offsets));
    entry["text"] = JsonString(PointerScan::FormatPath(path));
    entry["lua"] = JsonString(PointerScan::FormatLua(path));
    if (base_range)
      entry["base_region"] = JsonString(base_range->name);
    paths_json.push_back(picojson::value(std::move(entry)));
  }
  if (result.paths.empty())
  {
    text += "No path: raise max_offset (the address may be far into a big object) or max_depth, "
            "or start from the object's start rather than a field.";
  }
  else
  {
    text += fmt::format(
        "Saved as \"{}\". A path that works now can be chance: load another savestate where the "
        "object is elsewhere (another room, a reload), find it again (cheat_search on its vtable "
        "word or a known field), then find_pointer_paths check={} address=<its new address> "
        "keeps the paths that reach it there too.",
        label, label);
  }
  const size_t found = result.paths.size();
  {
    std::lock_guard lock(s_mutex);
    s_path_sets[label] = {target, std::move(result.paths)};
  }

  picojson::object structured;
  structured["target"] = JsonNumber(target);
  structured["found"] = JsonNumber(static_cast<double>(found));
  structured["truncated"] = JsonBool(result.truncated);
  structured["pointers"] = JsonNumber(static_cast<double>(result.pointers));
  structured["fixed"] = JsonString(layout.source);
  structured["paths"] = picojson::value(std::move(paths_json));
  return ToolResult::Text(std::move(text)).SetStructured(std::move(structured));
}

ToolResult DumpStruct(const ToolArgs& args)
{
  RequireGame();
  std::string text = EnsureSymbols();
  if (!text.empty())
    text += "\n";
  const u32 size = static_cast<u32>(std::clamp<s64>(args.GetInt("size", 0x80), 4, 0x800)) & ~3u;
  const std::string label = args.GetString("label");
  const std::string compare_to = args.GetString("compare_to");
  const bool changed_only = args.GetBool("changed_only", false);
  std::optional<Dump> before;
  if (!compare_to.empty())
  {
    std::lock_guard lock(s_mutex);
    const auto it = s_dumps.find(compare_to);
    if (it == s_dumps.end())
      throw ToolError(fmt::format("There is no dump saved as \"{}\".", compare_to));
    before = it->second;
  }
  const std::optional<PointerScan::DolLayout> dol = ReadBootDol();

  Core::CPUThreadGuard guard(GetSystem());
  std::string how;
  const u32 address = ResolveAddressOrPath(guard, args, &how);
  FixedLayout layout;
  try
  {
    layout = ReadFixedLayout(guard, dol, {});
  }
  catch (const ToolError&)
  {
    // Only the pointer descriptions use it.
  }
  const auto read = Reader(guard);

  Dump dump{address, {}};
  size_t changes = 0;
  std::string lines;
  picojson::array words_json;
  for (u32 offset = 0; offset < size; offset += 4)
  {
    const std::optional<u32> word = read(address + offset);
    dump.words.push_back(word);
    std::optional<u32> previous;
    bool changed = false;
    if (before && offset / 4 < before->words.size())
    {
      previous = before->words[offset / 4];
      changed = previous != word;
      changes += changed;
    }
    picojson::object entry;
    entry["offset"] = JsonNumber(offset);
    if (!word)
    {
      entry["kind"] = JsonString("unreadable");
      if (!changed_only || changed)
        lines += fmt::format("{}+{:03X}  ????????\n", changed ? "*" : " ", offset);
      words_json.push_back(picojson::value(std::move(entry)));
      continue;
    }
    const PointerScan::WordKind kind = PointerScan::Classify(*word, IsRamAddress);
    std::string meaning = PointerScan::FormatWord(*word, kind);
    entry["word"] = JsonNumber(*word);
    entry["kind"] = JsonString(PointerScan::WordKindName(kind));
    if (kind == PointerScan::WordKind::Pointer)
    {
      meaning = "-> " + DescribePointee(guard, *word, layout);
      entry["points_to"] = JsonString(meaning.substr(3));
    }
    else if (kind == PointerScan::WordKind::Float)
    {
      entry["value"] = JsonFloat(std::bit_cast<float>(*word));
    }
    else if (kind == PointerScan::WordKind::Int)
    {
      entry["value"] = JsonNumber(static_cast<s32>(*word));
    }
    else if (kind == PointerScan::WordKind::Text)
    {
      entry["value"] = JsonString(meaning.substr(1, 4));
    }
    std::string was;
    if (changed)
    {
      entry["changed"] = JsonBool(true);
      if (previous)
      {
        const PointerScan::WordKind old_kind = PointerScan::Classify(*previous, IsRamAddress);
        was = fmt::format("  (was {:08X} {})", *previous,
                          PointerScan::FormatWord(*previous, old_kind));
        entry["previous"] = JsonNumber(*previous);
      }
    }
    if (!changed_only || changed)
    {
      lines += fmt::format("{}+{:03X}  {:08X}  {:<7} {}{}\n", changed ? "*" : " ", offset, *word,
                           PointerScan::WordKindName(kind), meaning, was);
    }
    words_json.push_back(picojson::value(std::move(entry)));
  }

  text += fmt::format("{} bytes at {}{}", size, DescribeAddress(address),
                      how.empty() ? "" : " (" + how + ")");
  if (before)
  {
    text += fmt::format(", compared with \"{}\"{}: {} word(s) changed (*)", compare_to,
                        before->address == address ?
                            "" :
                            fmt::format(" (taken at {:08X}, compared by offset)", before->address),
                        changes);
  }
  text += ":\n" + lines;
  text += "Kinds are guesses: a float can be an integer that looks like one, a pointer an ID. ";
  if (!label.empty())
  {
    text += fmt::format("Saved as \"{}\": change the game, then dump again with compare_to={} to "
                        "see which fields moved.",
                        label, label);
    std::lock_guard lock(s_mutex);
    s_dumps[label] = std::move(dump);
  }

  picojson::object structured = AddressJson(address);
  structured["size"] = JsonNumber(size);
  if (before)
    structured["changed"] = JsonNumber(static_cast<double>(changes));
  structured["words"] = picojson::value(std::move(words_json));
  return ToolResult::Text(std::move(text)).SetStructured(std::move(structured));
}
}  // namespace

u32 ResolveAddressOrPath(const Core::CPUThreadGuard& guard, const ToolArgs& args, std::string* how)
{
  const std::string text = args.GetString("path");
  if (text.empty())
  {
    if (!args.Has("address"))
      throw ToolError("Give an address (hex) or a path such as [[8067DC18]+C54]+9C.");
    return args.RequireAddress("address");
  }
  const std::optional<Path> path = PointerScan::ParsePath(text);
  if (!path)
  {
    throw ToolError(fmt::format("Cannot read the path \"{}\": write it as [[8067DC18]+C54]+9C "
                                "(hex, as find_pointer_paths prints it) or mem.chain(0x8067DC18, "
                                "0xC54, 0x9C).",
                                text));
  }
  const PointerScan::Resolution resolution =
      PointerScan::Resolve(*path, Reader(guard), IsRamAddress);
  if (!resolution.address)
  {
    const PointerScan::Step& step = resolution.steps.back();
    throw ToolError(
        fmt::format("The path {} breaks at its pointer {}: {:08X} holds {}.",
                    PointerScan::FormatPath(*path), resolution.steps.size(), step.address,
                    step.value ? fmt::format("{:08X}, not an address in RAM", *step.value) :
                                 "nothing (unmapped)"));
  }
  if (how)
    *how = fmt::format("{} = {:08X}", PointerScan::FormatPath(*path), *resolution.address);
  return *resolution.address;
}

void RegisterPointerTools(ToolRegistry& registry, Server& server)
{
  server.AddServerStoppedCallback([] {
    std::lock_guard lock(s_mutex);
    s_path_sets.clear();
    s_dumps.clear();
  });

  registry.Add(
      {.name = "find_pointer_paths",
       .title = "Find pointer paths",
       .description =
           "Finds how to reach an object or a field from a fixed address, as Cheat Engine's "
           "pointer scan does: paths [[base]+o1]+o2 (= mem.chain(base, o1, o2) in a VR patch) "
           "from the executable's data and bss, the OS globals or a loaded module's data, "
           "following up to max_depth pointers with offsets up to max_offset (a field far into a "
           "big object needs more: Metroid Prime 3's player is 0x2184 into its state manager). "
           "Code is not searched (its lwz instructions look like pointers). Fewest pointers "
           "first, then the smallest offsets. The paths "
           "are saved under label: since some work by chance, check them in another savestate "
           "where the object sits elsewhere with check=<label> address=<its new address> (keeps "
           "the paths that reach it, and saves them under label, by default the same), or "
           "check=<label> alone to see where they lead. The fixed data comes from the boot DOL's "
           "header; when another executable runs (a launcher), it is estimated from r13 and r2; "
           "static_ranges overrides it. structuredContent: {target, found, truncated, pointers, "
           "fixed, paths: [{address, symbol?, offsets, text, lua, base_region}]}; with check: "
           "{checked, paths: [{text, lua}]} or {checked, destinations: [{address, paths}]}.",
       .input_schema =
           Schema()
               .Hex("address", "The object or field to reach.")
               .String("path", "Or a path to it, e.g. [[8067DC18]+C54]+9C.")
               .Integer("max_depth", "Pointers to follow, default 3.", 1, 5)
               .Integer("max_offset", "Largest offset after a pointer, default 4096 (0x1000).", 4,
                        65536)
               .String("label", "Name to save the paths under (default \"paths\").")
               .String("check", "Check the paths saved under this name instead of searching.")
               .Integer("show", "Paths listed, default 25.", 1, 200)
               .StringArray("static_ranges",
                            "Fixed ranges to use instead of the executable's, e.g. "
                            "[\"80004000-8067E000\"].")
               .Build(),
       .read_only = true,
       .handler = FindPointerPaths});

  registry.Add(
      {.name = "dump_struct",
       .title = "Dump a structure",
       .description =
           "Reads a block of memory word by word, each with its likely meaning: a pointer (and "
           "what it points to: code, a vtable, an object with its vtable, fixed data, or the "
           "first word there), a float, an integer, text or hex. Save a dump with label, change "
           "the game (move, open a menu, turn), then dump again with compare_to=<label> to see "
           "which words changed (changed_only lists only those): the fastest way to find a "
           "field. address, or path through pointers ([[8067DC18]+C54], as find_pointer_paths "
           "prints them). structuredContent: {address, symbol?, size, changed?, words: [{offset, "
           "word, kind, value?, points_to?, changed?, previous?}]}.",
       .input_schema =
           Schema()
               .Hex("address", "Start address.")
               .String("path", "Or a path to the start, e.g. [[805C4F98]+2184].")
               .Integer("size", "Bytes, default 128 (0x80), at most 2048.", 4, 2048)
               .String("label", "Save the dump under this name.")
               .String("compare_to", "Mark the words that changed since this saved dump.")
               .Boolean("changed_only", "With compare_to: list only the changed words.")
               .Build(),
       .read_only = true,
       .handler = DumpStruct});
}
}  // namespace AgentServer
