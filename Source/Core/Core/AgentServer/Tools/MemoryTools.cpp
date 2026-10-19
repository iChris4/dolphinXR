// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The PowerPC debugger for the agent: symbols, disassembly, memory, breakpoints, watchpoints,
// registers and the call stack.

#include <algorithm>
#include <cmath>
#include <future>
#include <mutex>
#include <regex>
#include <set>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/AchievementManager.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/System.h"

namespace AgentServer
{
namespace
{
std::mutex s_debug_mutex;
bool s_agent_enabled_debugging = false;
// Breakpoints and watchpoints the agent set (removed when the server or emulation stops).
std::set<u32> s_agent_breakpoints;
std::set<u32> s_agent_watchpoints;

std::optional<Expression> ParseCondition(const ToolArgs& args)
{
  const std::string text = args.GetString("condition");
  if (text.empty())
    return std::nullopt;
  std::optional<Expression> condition = Expression::TryParse(text);
  if (!condition)
  {
    throw ToolError(fmt::format("Cannot parse the condition \"{}\" (variables r0-r31, f0-f31, "
                                "pc, lr, ctr; functions read_u32(addr)...).",
                                text));
  }
  return condition;
}

std::string FormatInstructionLine(const Core::CPUThreadGuard& guard, u32 address)
{
  auto& power_pc = GetSystem().GetPowerPC();
  const auto word = PowerPC::MMU::HostTryReadInstruction(guard, address);
  const std::string disassembly = power_pc.GetDebugInterface().Disassemble(&guard, address);
  std::string line =
      fmt::format("{:08X}  {}  {}", address, word ? fmt::format("{:08X}", word->value) : "????????",
                  disassembly);
  if (word && (word->value & 0xFC0007FEu) == 0x4C000020u)
    line += "    <- return";
  // Name direct branch targets that start a function.
  if (word && (word->value >> 26) == 18)
  {
    const s32 offset = static_cast<s32>((word->value & 0x03FFFFFCu) << 6) >> 6;
    const u32 target = (word->value & 2) ? static_cast<u32>(offset) : address + offset;
    if (const Common::Symbol* symbol = GetSystem().GetPPCSymbolDB().GetSymbolFromAddr(target);
        symbol && symbol->address == target)
    {
      line += fmt::format("    -> {}", SymbolName(*symbol));
    }
  }
  return line;
}

ToolResult GenerateSymbols(const ToolArgs& args)
{
  RequireGame();
  auto& symbol_db = GetSystem().GetPPCSymbolDB();
  if (!symbol_db.IsEmpty() && !args.GetBool("force", false))
  {
    size_t count = 0;
    symbol_db.ForEachSymbol([&](const Common::Symbol&) { ++count; });
    return ToolResult::Text(fmt::format(
        "The symbol map already has {} symbols (pass force=true to rebuild it from the signature "
        "database).",
        count));
  }
  if (args.GetBool("force", false))
    symbol_db.Clear();
  return ToolResult::Text(EnsureSymbols());
}

ToolResult ListSymbols(const ToolArgs& args)
{
  RequireGame();
  const std::string note = EnsureSymbols();
  const std::string pattern = args.GetString("name_regex");
  std::optional<std::regex> regex;
  if (!pattern.empty())
  {
    try
    {
      regex.emplace(pattern, std::regex::ECMAScript | std::regex::icase);
    }
    catch (const std::regex_error& e)
    {
      throw ToolError(fmt::format("Invalid regex: {}", e.what()));
    }
  }
  const u32 address_min = args.GetAddress("address_min").value_or(0);
  const u32 address_max = args.GetAddress("address_max").value_or(0xFFFFFFFFu);
  const u32 size_min = static_cast<u32>(std::max<s64>(0, args.GetInt("size_min", 0)));
  const u32 size_max = static_cast<u32>(std::max<s64>(0, args.GetInt("size_max", 0x7FFFFFFF)));

  // Filtered outside ForEachSymbol: std::regex_search can throw, and the symbol database is not
  // built with exceptions off MSVC.
  std::vector<const Common::Symbol*> matches;
  GetSystem().GetPPCSymbolDB().ForEachSymbol([&](const Common::Symbol& symbol) {
    if (symbol.address >= address_min && symbol.address <= address_max && symbol.size >= size_min &&
        symbol.size <= size_max)
    {
      matches.push_back(&symbol);
    }
  });
  if (regex)
  {
    std::erase_if(matches, [&](const Common::Symbol* symbol) {
      return !std::regex_search(symbol->name, *regex);
    });
  }

  const size_t page_size =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("page_size", 100), 1, 500));
  const size_t pages = std::max<size_t>(1, (matches.size() + page_size - 1) / page_size);
  const size_t page = static_cast<size_t>(std::clamp<s64>(args.GetInt("page", 1), 1, pages));
  std::string text = note.empty() ? "" : note + "\n";
  text += fmt::format("{} symbol(s) match; page {}/{}. address size name (calls in the map)\n",
                      matches.size(), page, pages);
  for (size_t i = (page - 1) * page_size; i < std::min(matches.size(), page * page_size); ++i)
  {
    const Common::Symbol& symbol = *matches[i];
    text += fmt::format("{:08X} {:5} {}{}\n", symbol.address, symbol.size, SymbolName(symbol),
                        symbol.type == Common::Symbol::Type::Data ? " (data)" : "");
  }
  return ToolResult::Text(std::move(text));
}

ToolResult GetSymbol(const ToolArgs& args)
{
  RequireGame();
  EnsureSymbols();
  const u32 address = ResolveCodeAddress(args);
  const auto& symbol_db = GetSystem().GetPPCSymbolDB();
  const Common::Symbol* symbol = symbol_db.GetSymbolFromAddr(address);
  if (!symbol)
    return ToolResult::Text(fmt::format("{:08X} is not inside a known function.", address));

  std::string text =
      fmt::format("{} at {:08X}, {} bytes ({} instructions){}\n", SymbolName(*symbol),
                  symbol->address, symbol->size, symbol->size / 4,
                  symbol->object_name.empty() ? "" : ", object " + symbol->object_name);
  const auto list = [&](const std::vector<Common::SCall>& calls, const char* label) {
    text += fmt::format("{} ({}):", label, calls.size());
    for (size_t i = 0; i < std::min<size_t>(calls.size(), 30); ++i)
    {
      const Common::Symbol* other = symbol_db.GetSymbolFromAddr(calls[i].function);
      text += fmt::format(" {:08X}{}", calls[i].call_address,
                          other ? fmt::format(" ({})", other->name) : "");
    }
    text += calls.size() > 30 ? " ...\n" : "\n";
  };
  list(symbol->callers, "Called from");
  list(symbol->calls, "Calls");
  return ToolResult::Text(std::move(text));
}

ToolResult Disassemble(const ToolArgs& args)
{
  RequireGame();
  EnsureSymbols();
  u32 address = ResolveCodeAddress(args) & ~3u;
  u32 count = static_cast<u32>(std::clamp<s64>(args.GetInt("count", 32), 1, 512));
  const Common::Symbol* symbol = GetSystem().GetPPCSymbolDB().GetSymbolFromAddr(address);
  if (args.GetBool("whole_function", false) && symbol)
  {
    address = symbol->address;
    count = std::clamp<u32>(symbol->size / 4, 1, 512);
  }

  std::string text = symbol ? fmt::format("{} ({:08X}, {} bytes)\n", SymbolName(*symbol),
                                          symbol->address, symbol->size) :
                              std::string();
  Core::CPUThreadGuard guard(GetSystem());
  for (u32 i = 0; i < count; ++i)
    text += FormatInstructionLine(guard, address + i * 4) + "\n";
  return ToolResult::Text(std::move(text));
}

ToolResult ReadMemory(const ToolArgs& args)
{
  RequireGame();
  const u32 address = args.RequireAddress("address");
  const std::string format = args.GetString("format", "u32");
  const u32 count = static_cast<u32>(std::clamp<s64>(args.GetInt("count", 16), 1, 1024));

  Core::CPUThreadGuard guard(GetSystem());
  std::string text = fmt::format("{} at {}:\n", format, DescribeAddress(address));
  if (format == "string")
  {
    const auto value = PowerPC::MMU::HostTryReadString(guard, address, count);
    if (!value)
      throw ToolError("The address is not readable.");
    return ToolResult::Text(text + "\"" + value->value + "\"");
  }

  const u32 size = format == "u8"                       ? 1 :
                   format == "u16"                      ? 2 :
                   (format == "u64" || format == "f64") ? 8 :
                                                          4;
  if (format != "u8" && format != "u16" && format != "u32" && format != "u64" && format != "f32" &&
      format != "f64" && format != "s32")
  {
    throw ToolError("format is u8, u16, u32, s32, u64, f32, f64 or string.");
  }
  const u32 per_line = size == 1 ? 16 : size == 2 ? 8 : 4;
  for (u32 i = 0; i < count; ++i)
  {
    const u32 at = address + i * size;
    if (i % per_line == 0)
      text += fmt::format("{}{:08X}:", i == 0 ? "" : "\n", at);
    std::string value = "??";
    if (size == 1)
    {
      if (const auto v = PowerPC::MMU::HostTryRead<u8>(guard, at))
        value = fmt::format("{:02X}", v->value);
    }
    else if (size == 2)
    {
      if (const auto v = PowerPC::MMU::HostTryRead<u16>(guard, at))
        value = fmt::format("{:04X}", v->value);
    }
    else if (size == 8)
    {
      if (const auto v = PowerPC::MMU::HostTryRead<u64>(guard, at))
        value = format == "f64" ? fmt::format("{}", std::bit_cast<double>(v->value)) :
                                  fmt::format("{:016X}", v->value);
    }
    else if (const auto v = PowerPC::MMU::HostTryRead<u32>(guard, at))
    {
      value = format == "f32" ? fmt::format("{}", std::bit_cast<float>(v->value)) :
              format == "s32" ? fmt::format("{}", static_cast<s32>(v->value)) :
                                fmt::format("{:08X}", v->value);
    }
    text += " " + value;
  }
  return ToolResult::Text(std::move(text));
}

ToolResult WriteMemory(const ToolArgs& args)
{
  RequireGame();
  const u32 address = args.RequireAddress("address");
  const std::string type = args.GetString("type", "u32");
  const std::vector<u64> values = args.GetHexArray("values");
  if (values.empty())
    throw ToolError("values is a list of hex values, e.g. [\"0x38600001\"].");
  const bool code = args.GetBool("code", false);
  const u32 size = type == "u8" ? 1 : type == "u16" ? 2 : type == "u32" ? 4 : 0;
  if (size == 0)
    throw ToolError("type is u8, u16 or u32.");

  Core::CPUThreadGuard guard(GetSystem());
  auto& power_pc = GetSystem().GetPowerPC();
  for (size_t i = 0; i < values.size(); ++i)
  {
    const u32 at = address + static_cast<u32>(i) * size;
    bool ok = false;
    if (size == 1)
      ok = PowerPC::MMU::HostTryWrite<u8>(guard, static_cast<u8>(values[i]), at).has_value();
    else if (size == 2)
      ok = PowerPC::MMU::HostTryWrite<u16>(guard, static_cast<u16>(values[i]), at).has_value();
    else
      ok = PowerPC::MMU::HostTryWrite<u32>(guard, static_cast<u32>(values[i]), at).has_value();
    if (!ok)
      throw ToolError(fmt::format("{:08X} is not writable.", at));
    if (code)
      power_pc.ScheduleInvalidateCacheThreadSafe(at & ~3u);
  }
  return ToolResult::Text(fmt::format(
      "Wrote {} {} value(s) at {}. This is not saved and a savestate load undoes it; for code, "
      "prefer test_patch, which restores the original instructions.",
      values.size(), type, DescribeAddress(address)));
}

ToolResult SetBreakpoint(const ToolArgs& args)
{
  RequireGame();
  EnsureSymbols();
  const u32 address = ResolveCodeAddress(args) & ~3u;
  std::optional<Expression> condition = ParseCondition(args);
  EnsureDebugging();
  {
    Core::CPUThreadGuard guard(GetSystem());
    GetSystem().GetPowerPC().GetBreakPoints().Add(address, !args.GetBool("log_only", false),
                                                  args.GetBool("log_only", false),
                                                  std::move(condition));
  }
  {
    std::lock_guard lock(s_debug_mutex);
    s_agent_breakpoints.insert(address);
  }
  Host_PPCBreakpointsChanged();
  return ToolResult::Text(
      fmt::format("Breakpoint at {}. Resume, then wait_for_break.", DescribeAddress(address)));
}

ToolResult RemoveBreakpoint(const ToolArgs& args)
{
  RequireGame();
  const u32 address = ResolveCodeAddress(args) & ~3u;
  bool removed;
  {
    Core::CPUThreadGuard guard(GetSystem());
    removed = GetSystem().GetPowerPC().GetBreakPoints().Remove(address);
  }
  {
    std::lock_guard lock(s_debug_mutex);
    s_agent_breakpoints.erase(address);
  }
  Host_PPCBreakpointsChanged();
  ReleaseDebuggingIfUnused();
  return ToolResult::Text(removed ? "Removed." : "There was no breakpoint there.");
}

ToolResult ListBreakpoints(const ToolArgs&)
{
  RequireGame();
  std::string text;
  {
    Core::CPUThreadGuard guard(GetSystem());
    for (const TBreakPoint& bp : GetSystem().GetPowerPC().GetBreakPoints().GetBreakPoints())
    {
      text += fmt::format("{} {}{}{}\n", DescribeAddress(bp.address),
                          bp.is_enabled ? "" : "(disabled) ", bp.break_on_hit ? "break" : "",
                          bp.log_on_hit ? " log" : "");
    }
    for (const TMemCheck& mc : GetSystem().GetPowerPC().GetMemChecks().GetMemChecks())
    {
      text += fmt::format("watch {:08X}-{:08X} {}{} hits {}\n", mc.start_address, mc.end_address,
                          mc.is_break_on_read ? "read " : "", mc.is_break_on_write ? "write" : "",
                          mc.num_hits);
    }
  }
  return ToolResult::Text(text.empty() ? "No breakpoints or watchpoints." : text);
}

std::string DescribeRegisters()
{
  const auto& ppc = GetSystem().GetPPCState();
  std::string text =
      fmt::format("pc {} lr {} ctr {:08X} cr {:08X} msr {:08X}\n", DescribeAddress(ppc.pc),
                  DescribeAddress(LR(ppc)), CTR(ppc), ppc.cr.Get(), ppc.msr.Hex);
  for (int i = 0; i < 32; ++i)
    text += fmt::format("r{:<2} {:08X}{}", i, ppc.gpr[i], i % 8 == 7 ? "\n" : "  ");
  for (int i = 0; i < 32; ++i)
    text += fmt::format("f{:<2} {:<12.6g}{}", i, ppc.ps[i].PS0AsDouble(), i % 8 == 7 ? "\n" : " ");
  return text;
}

ToolResult WaitForBreak(const ToolArgs& args)
{
  RequireGame();
  const auto timeout =
      std::chrono::milliseconds(std::clamp<s64>(args.GetInt("timeout_ms", 10000), 100, 120000));
  if (!WaitUntil(IsPaused, timeout))
    return ToolResult::Text("No breakpoint was hit in time (emulation is still running).");
  Core::CPUThreadGuard guard(GetSystem());
  return ToolResult::Text("Paused at a breakpoint (or by the user).\n" + DescribeRegisters());
}

ToolResult GetRegisters(const ToolArgs&)
{
  RequireGame();
  if (!IsPaused())
    throw ToolError("Pause first (or wait_for_break): registers change constantly while running.");
  Core::CPUThreadGuard guard(GetSystem());
  return ToolResult::Text(DescribeRegisters());
}

ToolResult GetCallstack(const ToolArgs&)
{
  RequireGame();
  if (!IsPaused())
    throw ToolError("Pause first (or wait_for_break).");
  Core::CPUThreadGuard guard(GetSystem());
  std::vector<Dolphin_Debugger::CallstackEntry> stack;
  if (!Dolphin_Debugger::GetCallstack(guard, stack))
    throw ToolError("The call stack is not available here.");
  std::string text;
  for (const auto& entry : stack)
    text += fmt::format("{:08X} {}\n", entry.vAddress, StripWhitespace(entry.Name));
  return ToolResult::Text(std::move(text));
}

ToolResult StepInstruction(const ToolArgs& args)
{
  RequireGame();
  if (!IsPaused())
    throw ToolError("Pause first (or wait_for_break).");
  const u32 count = static_cast<u32>(std::clamp<s64>(args.GetInt("count", 1), 1, 1000));
  auto& cpu = GetSystem().GetCPU();
  for (u32 i = 0; i < count; ++i)
  {
    Common::Event event;
    cpu.StepOpcode(&event);
    if (!event.WaitFor(std::chrono::seconds(2)))
      throw ToolError("The step did not complete.");
  }
  Core::CPUThreadGuard guard(GetSystem());
  return ToolResult::Text(fmt::format("Stepped {} instruction(s).\n{}\n{}", count,
                                      FormatInstructionLine(guard, GetSystem().GetPPCState().pc),
                                      DescribeRegisters()));
}

ToolResult SetWatchpoint(const ToolArgs& args)
{
  RequireGame();
  const u32 address = args.RequireAddress("address");
  const u32 size = static_cast<u32>(std::clamp<s64>(args.GetInt("size", 4), 1, 0x10000));
  TMemCheck check;
  check.start_address = address;
  check.end_address = address + size - 1;
  check.is_ranged = size > 1;
  check.is_break_on_read = args.GetBool("read", false);
  check.is_break_on_write = args.GetBool("write", true);
  check.break_on_hit = !args.GetBool("log_only", false);
  check.log_on_hit = args.GetBool("log_only", false);
  check.condition = ParseCondition(args);
  if (!check.is_break_on_read && !check.is_break_on_write)
    throw ToolError("Watch reads, writes or both.");
  EnsureDebugging();
  GetSystem().GetPowerPC().GetMemChecks().Add(std::move(check));
  {
    std::lock_guard lock(s_debug_mutex);
    s_agent_watchpoints.insert(address);
  }
  Host_PPCBreakpointsChanged();
  return ToolResult::Text(fmt::format("Watching {:08X}-{:08X}. Resume, then wait_for_break.",
                                      address, address + size - 1));
}

ToolResult RemoveWatchpoint(const ToolArgs& args)
{
  RequireGame();
  const u32 address = args.RequireAddress("address");
  GetSystem().GetPowerPC().GetMemChecks().Remove(address);
  {
    std::lock_guard lock(s_debug_mutex);
    s_agent_watchpoints.erase(address);
  }
  Host_PPCBreakpointsChanged();
  ReleaseDebuggingIfUnused();
  return ToolResult::Text("Removed.");
}

// Removes what the agent set. Needs a running game (CPU thread guard).
void RemoveAgentDebugState()
{
  std::set<u32> breakpoints;
  std::set<u32> watchpoints;
  {
    std::lock_guard lock(s_debug_mutex);
    breakpoints.swap(s_agent_breakpoints);
    watchpoints.swap(s_agent_watchpoints);
  }
  if (!Core::IsRunning(GetSystem()))
    return;
  if (!breakpoints.empty() || !watchpoints.empty())
  {
    Core::CPUThreadGuard guard(GetSystem());
    for (const u32 address : breakpoints)
      GetSystem().GetPowerPC().GetBreakPoints().Remove(address);
  }
  for (const u32 address : watchpoints)
    GetSystem().GetPowerPC().GetMemChecks().Remove(address);
  Host_PPCBreakpointsChanged();
}
}  // namespace

void ApplyCPUThreadConfig()
{
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> future = done->get_future();
  Core::RunOnCPUThread(GetSystem(), [done] {
    CPUThreadConfigCallback::CheckForConfigChanges();
    done->set_value();
  });
  WaitUntil([&] { return future.wait_for(0s) == std::future_status::ready; }, 5s);
}

void EnsureDebugging()
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    throw ToolError("Debugging is disabled in RetroAchievements hardcore mode.");
  if (Config::IsDebuggingEnabled())
    return;
  {
    std::lock_guard lock(s_debug_mutex);
    s_agent_enabled_debugging = true;
  }
  Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, true);
  ApplyCPUThreadConfig();
}

void ReleaseDebuggingIfUnused()
{
  {
    std::lock_guard lock(s_debug_mutex);
    if (!s_agent_enabled_debugging || !s_agent_breakpoints.empty() || !s_agent_watchpoints.empty())
      return;
    s_agent_enabled_debugging = false;
  }
  Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_ENABLE_DEBUGGING);
  if (Core::IsRunning(GetSystem()))
    ApplyCPUThreadConfig();
}

u32 ResolveCodeAddress(const ToolArgs& args)
{
  if (const std::optional<u32> address = args.GetAddress("address"))
    return *address;
  const std::string name = args.GetString("symbol");
  if (name.empty())
    throw ToolError("Give an \"address\" (hex) or a \"symbol\" (function name).");
  const Common::Symbol* symbol = GetSystem().GetPPCSymbolDB().GetSymbolFromName(name);
  if (!symbol)
  {
    throw ToolError(fmt::format("No function named \"{}\" in the symbol map (list_symbols, "
                                "generate_symbols).",
                                name));
  }
  return symbol->address;
}

std::string_view SymbolName(const Common::Symbol& symbol)
{
  std::string_view name = symbol.name;
  name = name.substr(0, name.find('	'));
  while (!name.empty() && name.back() == ' ')
    name.remove_suffix(1);
  return name;
}

std::string DescribeAddress(u32 address)
{
  const Common::Symbol* symbol = GetSystem().GetPPCSymbolDB().GetSymbolFromAddr(address);
  if (!symbol)
    return fmt::format("{:08X}", address);
  if (symbol->address == address)
    return fmt::format("{:08X} ({})", address, SymbolName(*symbol));
  return fmt::format("{:08X} ({}+0x{:X})", address, SymbolName(*symbol), address - symbol->address);
}

std::string EnsureSymbols()
{
  auto& system = GetSystem();
  auto& symbol_db = system.GetPPCSymbolDB();
  if (!symbol_db.IsEmpty())
    return "";

  size_t total = 0;
  size_t named = 0;
  {
    Core::CPUThreadGuard guard(system);
    const u32 ram_end = Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal();
    PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR, ram_end, &symbol_db);
    SignatureDB db(SignatureDB::HandlerType::DSY);
    if (db.Load(File::GetSysDirectory() + TOTALDB))
      db.Apply(guard, &symbol_db);
  }
  symbol_db.ForEachSymbol([&](const Common::Symbol& symbol) {
    ++total;
    named += !symbol.name.starts_with("zz_");
  });
  Host_PPCSymbolsChanged();
  return fmt::format("Built the symbol map from the code in RAM: {} functions, {} named by the "
                     "signature database (SDK and library functions it recognizes; a small "
                     "function can get the name of identical code from another game, and SDK "
                     "versions missing from the database leave VI/GX functions unnamed). The "
                     "others are called zz_<address>_.",
                     total, named);
}

void RegisterMemoryTools(ToolRegistry& registry, Server& server)
{
  server.AddServerStoppedCallback([] {
    RemoveAgentDebugState();
    ReleaseDebuggingIfUnused();
  });
  server.AddEmulationStoppedCallback([] {
    std::lock_guard lock(s_debug_mutex);
    s_agent_breakpoints.clear();
    s_agent_watchpoints.clear();
    s_agent_enabled_debugging = false;
  });

  registry.Add({.name = "generate_symbols",
                .title = "Build the symbol map",
                .description =
                    "Finds the functions in RAM and names the SDK ones from Dolphin's signature "
                    "database, when the game has no symbol map yet (done automatically by the "
                    "tools that need symbols).",
                .input_schema = Schema().Boolean("force", "Rebuild even if a map exists.").Build(),
                .handler = GenerateSymbols});

  registry.Add({.name = "list_symbols",
                .title = "List functions",
                .description = "Functions of the symbol map, filtered and paged.",
                .input_schema = Schema()
                                    .String("name_regex", "Case-insensitive regex on the name.")
                                    .Hex("address_min", "Lowest address.")
                                    .Hex("address_max", "Highest address.")
                                    .Integer("size_min", "Smallest size in bytes.")
                                    .Integer("size_max", "Largest size in bytes.")
                                    .Integer("page", "1-based page.", 1)
                                    .Integer("page_size", "Default 100.", 1, 500)
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListSymbols});

  registry.Add(
      {.name = "get_symbol",
       .title = "One function",
       .description = "The function containing an address (or named), its size, "
                      "callers and callees.",
       .input_schema =
           Schema().Hex("address", "Code address.").String("symbol", "Function name.").Build(),
       .read_only = true,
       .idempotent = true,
       .handler = GetSymbol});

  registry.Add({.name = "disassemble",
                .title = "Disassemble",
                .description = "PowerPC disassembly from an address or of a whole function, with "
                               "return instructions and called functions marked.",
                .input_schema = Schema()
                                    .Hex("address", "Start address.")
                                    .String("symbol", "Function name.")
                                    .Integer("count", "Instructions (default 32).", 1, 512)
                                    .Boolean("whole_function", "The whole containing function.")
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = Disassemble});

  registry.Add(
      {.name = "read_memory",
       .title = "Read memory",
       .description = "Reads emulated memory (effective addresses, e.g. 0x80xxxxxx).",
       .input_schema = Schema()
                           .Hex("address", "Start address.")
                           .String("format", "Default u32.",
                                   {"u8", "u16", "u32", "s32", "u64", "f32", "f64", "string"})
                           .Integer("count", "Values (or string length), default 16.", 1, 1024)
                           .Required({"address"})
                           .Build(),
       .read_only = true,
       .idempotent = true,
       .handler = ReadMemory});

  registry.Add({.name = "write_memory",
                .title = "Write memory",
                .description = "Writes emulated memory now (not saved; a savestate load undoes "
                               "it). code=true invalidates the instruction caches.",
                .input_schema = Schema()
                                    .Hex("address", "Start address.")
                                    .String("type", "Default u32.", {"u8", "u16", "u32"})
                                    .HexArray("values", "Hex values written one after the other.")
                                    .Boolean("code", "The bytes are instructions.")
                                    .Required({"address", "values"})
                                    .Build(),
                .destructive = true,
                .handler = WriteMemory});

  registry.Add(
      {.name = "set_breakpoint",
       .title = "Set a breakpoint",
       .description =
           "Breaks (pauses) when the instruction runs, optionally only when a condition holds "
           "(e.g. \"r3 == 0\"). log_only=true logs instead (read_log). Turns on Dolphin's "
           "debugging mode for the session (slower emulation).",
       .input_schema = Schema()
                           .Hex("address", "Code address.")
                           .String("symbol", "Function name (its first instruction).")
                           .String("condition", "Dolphin debugger expression.")
                           .Boolean("log_only", "Log the hit instead of pausing.")
                           .Build(),
       .handler = SetBreakpoint});

  registry.Add(
      {.name = "remove_breakpoint",
       .title = "Remove a breakpoint",
       .description = "Removes a breakpoint.",
       .input_schema =
           Schema().Hex("address", "Code address.").String("symbol", "Function name.").Build(),
       .idempotent = true,
       .handler = RemoveBreakpoint});

  registry.Add({.name = "list_breakpoints",
                .title = "List breakpoints",
                .description = "Breakpoints and watchpoints.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListBreakpoints});

  registry.Add(
      {.name = "wait_for_break",
       .title = "Wait for a breakpoint",
       .description = "Waits until emulation pauses on a breakpoint or watchpoint and "
                      "returns the registers.",
       .input_schema = Schema().Integer("timeout_ms", "Default 10000.", 100, 120000).Build(),
       .read_only = true,
       .handler = WaitForBreak});

  registry.Add({.name = "get_registers",
                .title = "CPU registers",
                .description = "PowerPC registers (paused only).",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetRegisters});

  registry.Add({.name = "get_callstack",
                .title = "Call stack",
                .description = "The PowerPC call stack (paused only).",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetCallstack});

  registry.Add({.name = "step_instruction",
                .title = "Step instructions",
                .description = "Executes single instructions while paused.",
                .input_schema = Schema().Integer("count", "Default 1.", 1, 1000).Build(),
                .handler = StepInstruction});

  registry.Add(
      {.name = "set_watchpoint",
       .title = "Set a watchpoint",
       .description =
           "Breaks when memory is written (or read), optionally on a condition. log_only=true "
           "logs instead. For the code that writes a value, trace_memory is often simpler.",
       .input_schema = Schema()
                           .Hex("address", "Start address.")
                           .Integer("size", "Bytes (default 4).", 1, 0x10000)
                           .Boolean("write", "Default true.")
                           .Boolean("read", "Default false.")
                           .String("condition", "Dolphin debugger expression.")
                           .Boolean("log_only", "Log instead of pausing.")
                           .Required({"address"})
                           .Build(),
       .handler = SetWatchpoint});

  registry.Add(
      {.name = "remove_watchpoint",
       .title = "Remove a watchpoint",
       .description = "Removes a watchpoint by its start address.",
       .input_schema = Schema().Hex("address", "Start address.").Required({"address"}).Build(),
       .idempotent = true,
       .handler = RemoveWatchpoint});
}
}  // namespace AgentServer
