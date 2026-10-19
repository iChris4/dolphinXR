// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Code experiments for culling and 60fps codes: JIT profiling, call and memory traces, patch and
// memory-write experiments from a reference savestate, frame-rate measurement, memory watches
// and the cheat search.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/PatchTemplates.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/AgentServer/Tracepoints.h"
#include "Core/CheatSearch.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace AgentServer
{
namespace
{
FrameCounters& Counters()
{
  return Server::GetInstance().GetFrameCounters();
}

// Waits for `target` XFB copies. False when no copy came for `hang_timeout` (the game froze or
// stopped drawing) or after `total_timeout`.
bool WaitForXfbCopies(u64 target, std::chrono::milliseconds hang_timeout,
                      std::chrono::milliseconds total_timeout = 120s)
{
  const auto deadline = std::chrono::steady_clock::now() + total_timeout;
  u64 last = Counters().GetSnapshot().xfb_copies;
  auto last_progress = std::chrono::steady_clock::now();
  while (true)
  {
    const u64 copies = Counters().GetSnapshot().xfb_copies;
    if (copies >= target)
      return true;
    const auto now = std::chrono::steady_clock::now();
    if (copies != last)
    {
      last = copies;
      last_progress = now;
    }
    if (now - last_progress > hang_timeout || now > deadline || Server::GetInstance().IsStopping())
      return false;
    std::this_thread::sleep_for(2ms);
  }
}

// Runs emulation for `frames` XFB copies, then restores the pause state. Used by the profiling
// and trace tools.
void RunFrames(u32 frames, std::chrono::milliseconds hang_timeout,
               const std::function<bool()>& stop)
{
  const bool was_paused = IsPaused();
  const u64 start = Counters().GetSnapshot().xfb_copies;
  ResumeEmulation();
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  auto last_progress = std::chrono::steady_clock::now();
  u64 last = start;
  while (true)
  {
    const u64 copies = Counters().GetSnapshot().xfb_copies;
    if (copies >= start + frames || (stop && stop()))
      break;
    const auto now = std::chrono::steady_clock::now();
    if (copies != last)
    {
      last = copies;
      last_progress = now;
    }
    if (now - last_progress > hang_timeout || now > deadline || Server::GetInstance().IsStopping())
      break;
    std::this_thread::sleep_for(2ms);
  }
  if (was_paused)
    PauseEmulation();
}

// Answers every alert of an experiment: a bad patch can make the game read wild addresses or fail
// an assert, and the dialog would block emulation (on Quest, where nobody sees it in the
// headset). Disabling alerts is not enough: asserts ask "Ignore and continue?" regardless, and
// any config change re-applies the user's alert setting. Questions are answered yes (ignore and
// continue); the first line of each alert goes into the result.
class ScopedAlertCapture final
{
public:
  ScopedAlertCapture() : m_previous(Common::GetMsgAlertHandler())
  {
    {
      std::lock_guard lock(s_mutex);
      s_alerts.clear();
      s_count = 0;
    }
    Common::RegisterMsgAlertHandler(&Capture);
    Common::SetEnableAlert(true);
  }
  ~ScopedAlertCapture()
  {
    Common::RegisterMsgAlertHandler(m_previous);
    // UICommon sets the flag from this setting.
    Common::SetEnableAlert(Config::Get(Config::MAIN_USE_PANIC_HANDLERS));
  }
  ScopedAlertCapture(const ScopedAlertCapture&) = delete;
  ScopedAlertCapture& operator=(const ScopedAlertCapture&) = delete;

  static std::vector<std::string> Take()
  {
    std::lock_guard lock(s_mutex);
    std::vector<std::string> alerts = std::exchange(s_alerts, {});
    if (!alerts.empty() && s_count > alerts.size())
      alerts.back() += fmt::format(" (and {} more alert(s))", s_count - alerts.size());
    s_count = 0;
    return alerts;
  }

private:
  // Any thread (usually the CPU thread).
  static bool Capture(const char* caption, const char* text, bool, Common::MsgType)
  {
    std::lock_guard lock(s_mutex);
    ++s_count;
    if (s_alerts.size() < 3)
    {
      std::string_view first_line = text;
      first_line = first_line.substr(0, first_line.find('\n'));
      std::string alert = fmt::format("{}: {}", caption, first_line.substr(0, 300));
      s_alerts.push_back(std::move(alert));
    }
    return true;
  }

  static inline std::mutex s_mutex;
  static inline std::vector<std::string> s_alerts;
  static inline size_t s_count = 0;
  Common::MsgAlertHandler m_previous;
};

struct PatchSpec
{
  u32 address = 0;
  std::vector<u32> words;
  std::string label;
};

PatchSpec ParsePatch(const picojson::object& object)
{
  const ToolArgs args(object);
  PatchSpec spec;
  spec.address = args.RequireAddress("address") & ~3u;
  if (args.Has("template"))
  {
    const std::string name = args.RequireString("template");
    const std::optional<PatchTemplate> patch_template = ParsePatchTemplate(name);
    if (!patch_template)
      throw ToolError("template is li_r3_1, li_r3_0 or nop_blr.");
    spec.words = GetPatchTemplateWords(*patch_template);
    spec.label = fmt::format("{:08X} {}", spec.address, GetPatchTemplateName(*patch_template));
  }
  else
  {
    for (const u64 word : args.GetHexArray("words"))
      spec.words.push_back(static_cast<u32>(word));
    if (spec.words.empty())
      throw ToolError("Each patch needs a template or words.");
    spec.label = fmt::format("{:08X} {} word(s)", spec.address, spec.words.size());
  }
  return spec;
}

std::vector<PatchSpec> ParsePatches(const picojson::array* patches)
{
  std::vector<PatchSpec> specs;
  if (!patches)
    return specs;
  for (const picojson::value& value : *patches)
  {
    if (!value.is<picojson::object>())
      throw ToolError("Each patch is an object {address, template | words}.");
    specs.push_back(ParsePatch(value.get<picojson::object>()));
  }
  return specs;
}

// Writes the patches and returns the words they replaced.
std::vector<u32> ApplyPatches(const Core::CPUThreadGuard& guard,
                              const std::vector<PatchSpec>& patches)
{
  auto& power_pc = GetSystem().GetPowerPC();
  std::vector<u32> originals;
  for (const PatchSpec& patch : patches)
  {
    for (size_t i = 0; i < patch.words.size(); ++i)
    {
      const u32 address = patch.address + static_cast<u32>(i * 4);
      const auto original = PowerPC::MMU::HostTryRead<u32>(guard, address);
      if (!original || !PowerPC::MMU::HostTryWrite<u32>(guard, patch.words[i], address))
        throw ToolError(fmt::format("{:08X} is not writable code.", address));
      originals.push_back(original->value);
      power_pc.ScheduleInvalidateCacheThreadSafe(address);
    }
  }
  return originals;
}

void RevertPatches(const Core::CPUThreadGuard& guard, const std::vector<PatchSpec>& patches,
                   const std::vector<u32>& originals)
{
  auto& power_pc = GetSystem().GetPowerPC();
  size_t index = 0;
  for (const PatchSpec& patch : patches)
  {
    for (size_t i = 0; i < patch.words.size() && index < originals.size(); ++i, ++index)
    {
      const u32 address = patch.address + static_cast<u32>(i * 4);
      PowerPC::MMU::HostTryWrite<u32>(guard, originals[index], address);
      power_pc.ScheduleInvalidateCacheThreadSafe(address);
    }
  }
}

ToolResult TestPatch(const ToolArgs& args)
{
  const ExperimentOptions options = ParseExperimentOptions(args);
  const std::vector<PatchSpec> patches = ParsePatches(args.GetArray("patches"));
  ToolResult images;
  std::vector<u32> originals;
  const ExperimentResult result = RunExperiment(
      options, [&](const Core::CPUThreadGuard& guard) { originals = ApplyPatches(guard, patches); },
      [&](const Core::CPUThreadGuard& guard) { RevertPatches(guard, patches, originals); }, {},
      &images);

  std::string text;
  for (const PatchSpec& patch : patches)
    text += fmt::format("{}{}", text.empty() ? "Patches: " : ", ", patch.label);
  if (patches.empty())
    text = "Baseline (no patch)";
  text += ".\n" + FormatExperimentResult(result);
  ToolResult output = ToolResult::Text(std::move(text));
  for (picojson::value& block : images.content)
    output.content.push_back(std::move(block));
  picojson::object structured;
  structured["hung"] = JsonBool(result.hung);
  structured["draw_calls_avg"] = JsonNumber(result.draw_calls_avg);
  structured["prims_avg"] = JsonNumber(result.prims_avg);
  structured["frames_per_field"] = JsonNumber(result.frames_per_field);
  structured["alerts"] = JsonNumber(static_cast<double>(result.alerts.size()));
  return output.SetStructured(std::move(structured));
}

ToolResult TestPatches(const ToolArgs& args)
{
  ExperimentOptions options = ParseExperimentOptions(args);
  const picojson::array* candidates = args.GetArray("candidates");
  if (!candidates || candidates->empty() || candidates->size() > 12)
    throw ToolError("candidates is 1 to 12 patches {address, template | words}.");
  const std::string images_mode = args.GetString("images", "best");

  struct Row
  {
    std::string label;
    ExperimentResult result;
    ToolResult images;
  };
  std::vector<Row> rows;
  const auto run = [&](std::vector<PatchSpec> patches, std::string label) {
    Row row;
    row.label = std::move(label);
    std::vector<u32> originals;
    ExperimentOptions run_options = options;
    run_options.screenshot = options.screenshot && images_mode != "none";
    row.result = RunExperiment(
        run_options,
        [&](const Core::CPUThreadGuard& guard) { originals = ApplyPatches(guard, patches); },
        [&](const Core::CPUThreadGuard& guard) { RevertPatches(guard, patches, originals); }, {},
        &row.images);
    rows.push_back(std::move(row));
  };

  if (args.GetBool("include_baseline", true))
    run({}, "baseline");
  for (const picojson::value& value : *candidates)
  {
    if (!value.is<picojson::object>())
      throw ToolError("Each candidate is an object {address, template | words}.");
    PatchSpec patch = ParsePatch(value.get<picojson::object>());
    std::string label = patch.label;
    run({std::move(patch)}, std::move(label));
  }

  const double baseline_draws =
      rows.front().label == "baseline" ? rows.front().result.draw_calls_avg : 0.0;
  std::string text = "candidate | hung | draw calls avg (min-max) | vs baseline | primitives avg | "
                     "frames/field\n";
  for (const Row& row : rows)
  {
    const double delta =
        baseline_draws > 0 ? (row.result.draw_calls_avg / baseline_draws - 1) * 100 : 0;
    text += fmt::format("{} | {} | {:.1f} ({}-{}) | {:+.0f}% | {:.0f} | {:.2f}\n", row.label,
                        row.result.hung ? "HUNG" : "no", row.result.draw_calls_avg,
                        row.result.draw_calls_min, row.result.draw_calls_max, delta,
                        row.result.prims_avg, row.result.frames_per_field);
  }

  for (const Row& row : rows)
  {
    if (!row.result.alerts.empty())
      text += fmt::format("{} raised an alert: {}\n", row.label, row.result.alerts.front());
  }

  // Images: baseline plus the candidates that add the most draws ("best"), or all.
  std::vector<const Row*> shown;
  if (images_mode == "all")
  {
    for (const Row& row : rows)
      shown.push_back(&row);
  }
  else if (images_mode == "best")
  {
    std::vector<const Row*> sorted;
    for (const Row& row : rows)
    {
      if (row.label != "baseline" && !row.result.hung)
        sorted.push_back(&row);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Row* a, const Row* b) {
      return a->result.draw_calls_avg > b->result.draw_calls_avg;
    });
    if (rows.front().label == "baseline")
      shown.push_back(&rows.front());
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 2); ++i)
      shown.push_back(sorted[i]);
  }
  if (!shown.empty())
  {
    text += "Images (in order):";
    for (const Row* row : shown)
    {
      text += fmt::format(" [{}: {}]", row->label,
                          !row->result.screenshot_text.empty() ? row->result.screenshot_text :
                          row->result.hung                     ? "no screenshot (hung)" :
                                                                 "no screenshot");
    }
  }

  ToolResult output = ToolResult::Text(std::move(text));
  for (const Row* row : shown)
  {
    for (const picojson::value& block : row->images.content)
      output.content.push_back(block);
  }
  return output;
}

ToolResult SavePatchAsARCode(const ToolArgs& args)
{
  RequireGame();
  const PatchSpec patch = ParsePatch(args.Raw());
  const std::string name = args.GetString("name", fmt::format("VR: {} ({})", patch.label, "agent"));
  const std::vector<std::string> lines = BuildARCodeLines(patch.address, patch.words);
  std::string text = AddActionReplayCode(name, lines, args.GetBool("enabled", true),
                                         args.GetBool("replace", false));
  text += "\n";
  for (const std::string& line : lines)
    text += line + "\n";
  text += "Ask the user to check it in the headset (look around, play a bit); on Quest, measure "
          "the GPU cost: removing culling draws more.";
  return ToolResult::Text(std::move(text));
}

// Counts the instructions of a function: float compares (frustum tests) and returns.
void CountInstructions(const Core::CPUThreadGuard& guard, const Common::Symbol& symbol, u32* fcmp,
                       u32* returns)
{
  *fcmp = 0;
  *returns = 0;
  const u32 count = std::min<u32>(symbol.size / 4, 4096);
  for (u32 i = 0; i < count; ++i)
  {
    const auto word = PowerPC::MMU::HostTryRead<u32>(guard, symbol.address + i * 4);
    if (!word)
      break;
    const u32 opcode = word->value >> 26;
    const u32 xo = (word->value >> 1) & 0x3FF;
    if (opcode == 63 && (xo == 0 || xo == 32))
      ++*fcmp;
    if ((word->value & 0xFC0007FEu) == 0x4C000020u)
      ++*returns;
  }
}

// True for a branch that calls (bl, bla, bcl, bctrl, blrl): its destination is a function entry.
bool IsCallInstruction(UGeckoInstruction inst)
{
  if (!inst.LK)
    return false;
  return inst.OPCD == 18 || inst.OPCD == 16 ||
         (inst.OPCD == 19 && (inst.SUBOP10 == 16 || inst.SUBOP10 == 528));
}

ToolResult ProfileFunctions(const ToolArgs& args)
{
  RequireGame();
  const u32 frames = static_cast<u32>(std::clamp<s64>(args.GetInt("frames", 60), 5, 600));
  const size_t top = static_cast<size_t>(std::clamp<s64>(args.GetInt("top", 40), 1, 200));
  const u32 max_size = static_cast<u32>(std::max<s64>(0, args.GetInt("max_size", 0)));
  const std::string pattern = args.GetString("name_regex");
  const std::optional<std::regex> regex = MakeToolRegex(pattern);

  std::string text = EnsureSymbols();
  if (!text.empty())
    text += "\n";

  // Counted with Dolphin's Branch Watch, which records every taken branch from JIT code without
  // debugging mode. (JIT block profiling needs debugging mode, and turning it on after boot
  // crashes Jit64: its block-entry profiling call clobbers the BLR optimization's stack.)
  auto& system = GetSystem();
  auto& branch_watch = system.GetPowerPC().GetBranchWatch();
  {
    Core::CPUThreadGuard guard(system);
    if (branch_watch.GetRecordingActive() || branch_watch.GetCollectionSize() != 0)
    {
      throw ToolError("Dolphin's Branch Watch tool is in use (it is recording or holds recorded "
                      "branches). profile_functions records with it: ask the user to clear it in "
                      "the Branch Watch window, then retry.");
    }
    branch_watch.SetRecordingActive(guard, true);
  }
  const auto stop_recording = [&] {
    Core::CPUThreadGuard guard(system);
    branch_watch.SetRecordingActive(guard, false);
  };
  try
  {
    RunFrames(frames, 3s, {});
  }
  catch (...)
  {
    stop_recording();
    Core::CPUThreadGuard guard(system);
    branch_watch.Clear(guard);
    throw;
  }
  stop_recording();

  struct Stats
  {
    u32 address = 0;
    const Common::Symbol* symbol = nullptr;
    u64 calls = 0;
    u32 call_sites = 0;
  };
  std::string rows;
  {
    Core::CPUThreadGuard guard(system);
    // Moves every recorded branch into the selection, which is the public view of the records.
    branch_watch.IsolateHasExecuted(guard);
    std::map<u32, Stats> by_function;
    for (const Core::BranchWatchSelectionValueType& value : branch_watch.GetSelection())
    {
      const auto& [key, hits] = *value.collection_ptr;
      if (!value.condition || !IsCallInstruction(key.original_inst))
        continue;
      Stats& stats = by_function[key.destin_addr];
      stats.address = key.destin_addr;
      stats.calls += hits.total_hits;
      ++stats.call_sites;
    }
    branch_watch.Clear(guard);

    const auto& symbol_db = system.GetPPCSymbolDB();
    std::vector<Stats> sorted;
    for (auto& [address, stats] : by_function)
    {
      stats.symbol = symbol_db.GetSymbolFromAddr(address);
      if (max_size && stats.symbol && stats.symbol->size > max_size)
        continue;
      if (regex &&
          !std::regex_search(stats.symbol ? std::string(SymbolName(*stats.symbol)) : std::string(),
                             *regex))
        continue;
      sorted.push_back(stats);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Stats& a, const Stats& b) { return a.calls > b.calls; });
    for (size_t i = 0; i < std::min(top, sorted.size()); ++i)
    {
      const Stats& stats = sorted[i];
      u32 fcmp = 0, returns = 0;
      const bool at_entry = stats.symbol && stats.symbol->address == stats.address;
      if (at_entry)
        CountInstructions(guard, *stats.symbol, &fcmp, &returns);
      rows += fmt::format(
          "{:08X} {:5} {:9.1f} {:5} {:4} {:3} {}\n", stats.address,
          at_entry ? stats.symbol->size : 0, static_cast<double>(stats.calls) / frames,
          stats.call_sites, fcmp, returns,
          at_entry ? std::string(SymbolName(*stats.symbol)) : DescribeAddress(stats.address));
    }
  }

  if (rows.empty())
    throw ToolError("No calls were recorded: is the game running (not stuck on a frame)?");
  text += fmt::format(
      "Functions by calls per frame over {} frames (sites = distinct call sites). Culling tests "
      "are usually small functions called once per object per frame, with float compares (fcmp) "
      "and a bool result in r3.\n"
      "address  size calls/frame sites fcmp ret name\n{}",
      frames, rows);
  text += "Next: disassemble or trace_function on candidates, then test_patches.";
  return ToolResult::Text(std::move(text));
}

ToolResult TraceFunction(const ToolArgs& args)
{
  RequireGame();
  EnsureSymbols();
  const u32 address = ResolveCodeAddress(args) & ~3u;
  const Common::Symbol* symbol = GetSystem().GetPPCSymbolDB().GetSymbolFromAddr(address);
  const u32 frames = static_cast<u32>(std::clamp<s64>(args.GetInt("frames", 10), 1, 300));
  const size_t max_hits =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("max_hits", 4000), 1, 100000));
  const bool record_returns = args.GetBool("record_returns", true);
  std::optional<Expression> condition;
  if (const std::string text = args.GetString("condition"); !text.empty())
  {
    condition = Expression::TryParse(text);
    if (!condition)
      throw ToolError("Cannot parse the condition.");
  }

  std::vector<Tracepoints::Point> points = {{address, Tracepoints::HitKind::Entry}};
  if (record_returns && symbol && symbol->address == address)
  {
    Core::CPUThreadGuard guard(GetSystem());
    for (u32 offset = 0; offset < std::min<u32>(symbol->size, 16384); offset += 4)
    {
      const auto word = PowerPC::MMU::HostTryRead<u32>(guard, address + offset);
      if (word && (word->value & 0xFC0007FEu) == 0x4C000020u && !(word->value & 1))
        points.push_back({address + offset, Tracepoints::HitKind::Return});
    }
  }

  EnsureDebugging();
  auto& breakpoints = GetSystem().GetPowerPC().GetBreakPoints();
  std::vector<u32> added;
  std::string skipped;
  {
    Core::CPUThreadGuard guard(GetSystem());
    for (const Tracepoints::Point& point : points)
    {
      if (breakpoints.IsAddressBreakPoint(point.address))
      {
        skipped += fmt::format(" {:08X}", point.address);
        continue;
      }
      // The condition (move-only) applies to the entry point, which is the first one.
      std::optional<Expression> point_condition;
      if (point.kind == Tracepoints::HitKind::Entry)
        point_condition = std::move(condition);
      breakpoints.Add(point.address, false, false, std::move(point_condition));
      added.push_back(point.address);
    }
    Tracepoints::Arm(points, max_hits, Counters().GetFieldCounter());
  }

  const u64 start_field = Counters().GetSnapshot().fields;
  RunFrames(frames, 3s, [&] { return Tracepoints::GetHitCount() >= max_hits; });
  const u64 fields = Counters().GetSnapshot().fields - start_field;

  std::vector<Tracepoints::Hit> hits;
  {
    Core::CPUThreadGuard guard(GetSystem());
    hits = Tracepoints::Disarm();
    for (const u32 point : added)
      breakpoints.Remove(point);
  }
  ReleaseDebuggingIfUnused();

  size_t entries = 0;
  std::map<u32, size_t> callers;
  std::map<u32, size_t> entry_r3;
  std::map<u32, size_t> return_r3;
  for (const Tracepoints::Hit& hit : hits)
  {
    if (hit.kind == Tracepoints::HitKind::Entry)
    {
      ++entries;
      ++callers[hit.lr];
      ++entry_r3[hit.gpr[0]];
    }
    else if (hit.kind == Tracepoints::HitKind::Return)
    {
      ++return_r3[hit.gpr[0]];
    }
  }

  const auto top_of = [](const std::map<u32, size_t>& counts, size_t n, auto&& format) {
    std::vector<std::pair<u32, size_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t i = 0; i < std::min(n, sorted.size()); ++i)
      text += fmt::format(" {} x{};", format(sorted[i].first), sorted[i].second);
    return text.empty() ? std::string(" none") : text;
  };

  std::string text = fmt::format(
      "{}: {} call(s) in {} ({}){}; {} return site(s) traced.\n", DescribeAddress(address), entries,
      fields ? fmt::format("{} VI field(s)", fields) : "less than one VI field",
      fields ? fmt::format("{:.1f} per field", static_cast<double>(entries) / fields) :
               "raise max_hits for a per-field rate",
      hits.size() >= max_hits ? " (stopped at max_hits)" : "", points.size() - 1);
  if (!skipped.empty())
    text += "Skipped (already a breakpoint):" + skipped + "\n";
  text += "Callers (return address):" +
          top_of(callers, 10, [](u32 lr) { return DescribeAddress(lr); }) + "\n";
  text +=
      "r3 at entry:" + top_of(entry_r3, 8, [](u32 v) { return fmt::format("{:08X}", v); }) + "\n";
  if (!return_r3.empty())
  {
    size_t zero = 0, one = 0, other = 0;
    for (const auto& [value, count] : return_r3)
      (value == 0 ? zero : value == 1 ? one : other) += count;
    text += fmt::format("r3 at return: 0 x{}, 1 x{}, other x{}{}\n", zero, one, other,
                        other == 0 ? " (bool-like: a test such as a visibility check)" : "");
    text += "Top return values:" +
            top_of(return_r3, 8, [](u32 v) { return fmt::format("{:08X}", v); }) + "\n";
  }
  text += "First hits (kind pc lr r3 r4 r5 r6 field):\n";
  for (size_t i = 0; i < std::min<size_t>(hits.size(), 20); ++i)
  {
    const Tracepoints::Hit& hit = hits[i];
    text += fmt::format("  {} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {}\n",
                        hit.kind == Tracepoints::HitKind::Entry ? "entry " : "return", hit.pc,
                        hit.lr, hit.gpr[0], hit.gpr[1], hit.gpr[2], hit.gpr[3], hit.field);
  }
  return ToolResult::Text(std::move(text));
}

ToolResult TraceMemory(const ToolArgs& args)
{
  RequireGame();
  EnsureSymbols();
  const u32 address = args.RequireAddress("address");
  const u32 size = static_cast<u32>(std::clamp<s64>(args.GetInt("size", 4), 1, 0x1000));
  const u32 frames = static_cast<u32>(std::clamp<s64>(args.GetInt("frames", 10), 1, 300));
  const size_t max_hits =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("max_hits", 2000), 1, 50000));

  TMemCheck check;
  check.start_address = address;
  check.end_address = address + size - 1;
  check.is_ranged = size > 1;
  check.is_break_on_read = args.GetBool("read", false);
  check.is_break_on_write = args.GetBool("write", true);
  check.break_on_hit = false;
  check.log_on_hit = false;
  if (!check.is_break_on_read && !check.is_break_on_write)
    throw ToolError("Trace reads, writes or both.");

  EnsureDebugging();
  auto& memchecks = GetSystem().GetPowerPC().GetMemChecks();
  if (memchecks.GetMemCheck(address, size))
    throw ToolError("A watchpoint already covers this address; remove it first.");
  {
    Core::CPUThreadGuard guard(GetSystem());
    Tracepoints::Arm({}, max_hits, Counters().GetFieldCounter());
  }
  memchecks.Add(std::move(check));

  const u64 start_field = Counters().GetSnapshot().fields;
  RunFrames(frames, 3s, [&] { return Tracepoints::GetHitCount() >= max_hits; });
  const u64 fields = Counters().GetSnapshot().fields - start_field;

  memchecks.Remove(address);
  std::vector<Tracepoints::Hit> hits;
  {
    Core::CPUThreadGuard guard(GetSystem());
    hits = Tracepoints::Disarm();
  }
  ReleaseDebuggingIfUnused();

  struct Access
  {
    size_t count = 0;
    std::set<u64> values;
  };
  std::map<std::pair<u32, bool>, Access> by_pc;
  for (const Tracepoints::Hit& hit : hits)
  {
    Access& access = by_pc[{hit.pc, hit.kind == Tracepoints::HitKind::MemoryWrite}];
    ++access.count;
    if (access.values.size() < 8)
      access.values.insert(hit.value);
  }
  std::string text = fmt::format("{} access(es) to {:08X}-{:08X} in {} field(s){}.\n", hits.size(),
                                 address, address + size - 1, fields,
                                 hits.size() >= max_hits ? " (stopped at max_hits)" : "");
  text += "By instruction (read/write, count, values seen):\n";
  for (const auto& [key, access] : by_pc)
  {
    std::string values;
    for (const u64 value : access.values)
      values += fmt::format(" {:X}", value);
    text += fmt::format("  {} {} x{} ({:.2f}/field):{}\n", DescribeAddress(key.first),
                        key.second ? "write" : "read ", access.count,
                        fields ? static_cast<double>(access.count) / fields : 0.0, values);
  }
  return ToolResult::Text(std::move(text));
}

ToolResult MeasureFrameRate(const ToolArgs& args)
{
  RequireGame();
  const u32 fields = static_cast<u32>(std::clamp<s64>(args.GetInt("fields", 120), 10, 1200));
  const bool was_paused = IsPaused();
  const auto start = Counters().GetSnapshot();
  const auto start_time = std::chrono::steady_clock::now();
  ResumeEmulation();
  WaitUntil([&] { return Counters().GetSnapshot().fields >= start.fields + fields; }, 60s);
  const auto end = Counters().GetSnapshot();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
  if (was_paused)
    PauseEmulation();

  const u64 field_count = end.fields - start.fields;
  const double ratio =
      field_count ? static_cast<double>(end.unique_presents - start.unique_presents) / field_count :
                    0;
  const double xfb_ratio =
      field_count ? static_cast<double>(end.xfb_copies - start.xfb_copies) / field_count : 0;
  const double refresh = GetSystem().GetVideoInterface().GetTargetRefreshRate();
  return ToolResult::Text(fmt::format(
      "Over {} VI fields ({:.1f} s, VI at {:.2f} Hz): {:.2f} unique frames per field, {:.2f} XFB "
      "copies per field: the game renders at about {:.0f} fps of game time ({}). "
      "1.0 = every field, 0.5 = every other field (30 fps on a 60 Hz VI).",
      field_count, seconds, refresh, ratio, xfb_ratio, ratio * refresh,
      ratio > 0.9 ? "full frame rate" :
      ratio > 0.4 ? "half rate" :
                    "a third or less"));
}

ToolResult WatchMemory(const ToolArgs& args)
{
  RequireGame();
  std::vector<u64> addresses = args.GetHexArray("addresses");
  if (addresses.empty() || addresses.size() > 16)
    throw ToolError("addresses is 1 to 16 hex addresses.");
  const std::string type = args.GetString("type", "u32");
  if (type != "u8" && type != "u16" && type != "u32" && type != "f32")
    throw ToolError("type is u8, u16, u32 or f32.");
  const u32 fields = static_cast<u32>(std::clamp<s64>(args.GetInt("fields", 60), 2, 600));

  struct Samples
  {
    std::mutex mutex;
    std::vector<std::vector<u32>> rows;
  };
  auto samples = std::make_shared<Samples>();
  auto& system = GetSystem();
  // CPU thread, at each VI field.
  Common::EventHook hook = system.GetVideoEvents().vi_end_field_event.Register(
      [&system, samples, addresses, type, fields] {
        const Core::CPUThreadGuard guard(system);
        std::vector<u32> row;
        for (const u64 address : addresses)
        {
          const u32 at = static_cast<u32>(address);
          u32 value = 0xFFFFFFFF;
          if (type == "u8")
          {
            if (const auto v = PowerPC::MMU::HostTryRead<u8>(guard, at))
              value = v->value;
          }
          else if (type == "u16")
          {
            if (const auto v = PowerPC::MMU::HostTryRead<u16>(guard, at))
              value = v->value;
          }
          else if (const auto v = PowerPC::MMU::HostTryRead<u32>(guard, at))
          {
            value = v->value;
          }
          row.push_back(value);
        }
        std::lock_guard lock(samples->mutex);
        if (samples->rows.size() < fields)
          samples->rows.push_back(std::move(row));
      });

  const bool was_paused = IsPaused();
  ResumeEmulation();
  WaitUntil(
      [&] {
        std::lock_guard lock(samples->mutex);
        return samples->rows.size() >= fields;
      },
      60s);
  if (was_paused)
    PauseEmulation();
  hook.reset();

  std::lock_guard lock(samples->mutex);
  const auto format_value = [&](u32 value) {
    return type == "f32" ? fmt::format("{:g}", std::bit_cast<float>(value)) :
                           fmt::format("{:X}", value);
  };
  std::string text = fmt::format("{} fields sampled.\n", samples->rows.size());
  for (size_t a = 0; a < addresses.size(); ++a)
  {
    std::vector<u32> series;
    for (const auto& row : samples->rows)
      series.push_back(row[a]);
    std::set<u32> distinct(series.begin(), series.end());
    size_t increments = 0, changes = 0;
    for (size_t i = 1; i < series.size(); ++i)
    {
      changes += series[i] != series[i - 1];
      increments += series[i] == series[i - 1] + 1;
    }
    std::string pattern = distinct.size() == 1            ? "constant" :
                          increments + 1 >= series.size() ? "counts up by 1 each field" :
                          changes == 0                    ? "constant" :
                                         fmt::format("changes {} times, {} distinct values",
                                                     changes, distinct.size());
    text += fmt::format("{:08X}: {}. First values:", addresses[a], pattern);
    for (size_t i = 0; i < std::min<size_t>(series.size(), 24); ++i)
      text += " " + format_value(series[i]);
    text += "\n";
  }
  return ToolResult::Text(std::move(text));
}

// The agent's cheat search session.
std::unique_ptr<Cheats::CheatSearchSessionBase> s_cheat_session;

ToolResult CheatSearch(const ToolArgs& args)
{
  RequireGame();
  const std::string action = args.GetString("action", "next");
  auto& system = GetSystem();

  if (action == "new")
  {
    static const std::map<std::string, Cheats::DataType> types = {
        {"u8", Cheats::DataType::U8},   {"u16", Cheats::DataType::U16},
        {"u32", Cheats::DataType::U32}, {"s8", Cheats::DataType::S8},
        {"s16", Cheats::DataType::S16}, {"s32", Cheats::DataType::S32},
        {"f32", Cheats::DataType::F32}, {"f64", Cheats::DataType::F64}};
    const auto type = types.find(args.GetString("data_type", "u32"));
    if (type == types.end())
      throw ToolError("data_type is u8, u16, u32, s8, s16, s32, f32 or f64.");
    std::vector<Cheats::MemoryRange> ranges = {
        {Memory::MEM1_BASE_ADDR, system.GetMemory().GetRamSizeReal()}};
    if (system.IsWii())
      ranges.emplace_back(0x90000000u, system.GetMemory().GetExRamSizeReal());
    s_cheat_session =
        Cheats::MakeSession(std::move(ranges), PowerPC::RequestedAddressSpace::Virtual,
                            args.GetBool("aligned", true), type->second);
  }
  if (!s_cheat_session)
    throw ToolError("Start with action=new.");

  if (action == "new" || action == "next")
  {
    static const std::map<std::string, Cheats::CompareType> compares = {
        {"equal", Cheats::CompareType::Equal},
        {"not_equal", Cheats::CompareType::NotEqual},
        {"less", Cheats::CompareType::Less},
        {"less_or_equal", Cheats::CompareType::LessOrEqual},
        {"greater", Cheats::CompareType::Greater},
        {"greater_or_equal", Cheats::CompareType::GreaterOrEqual}};
    const auto compare = compares.find(args.GetString("compare", "equal"));
    if (compare == compares.end())
      throw ToolError(
          "compare is equal, not_equal, less, less_or_equal, greater or greater_or_equal.");
    s_cheat_session->SetCompareType(compare->second);

    const std::string filter = args.GetString("filter", args.Has("value") ? "value" : "previous");
    if (filter == "value")
    {
      s_cheat_session->SetFilterType(Cheats::FilterType::CompareAgainstSpecificValue);
      if (!s_cheat_session->SetValueFromString(args.RequireString("value"), false))
        throw ToolError("The value does not fit the data type.");
    }
    else if (filter == "previous")
    {
      s_cheat_session->SetFilterType(s_cheat_session->WasFirstSearchDone() ?
                                         Cheats::FilterType::CompareAgainstLastValue :
                                         Cheats::FilterType::DoNotFilter);
    }
    else if (filter == "none")
    {
      s_cheat_session->SetFilterType(Cheats::FilterType::DoNotFilter);
    }
    else
    {
      throw ToolError("filter is value, previous or none.");
    }

    Cheats::SearchErrorCode error;
    {
      Core::CPUThreadGuard guard(system);
      error = s_cheat_session->RunSearch(guard);
    }
    if (error == Cheats::SearchErrorCode::VirtualAddressesCurrentlyNotAccessible)
      throw ToolError("The game's memory mapping is off right now; advance a frame and retry.");
    if (error == Cheats::SearchErrorCode::DisabledInHardcoreMode)
      throw ToolError("Memory search is disabled in RetroAchievements hardcore mode.");
    if (error != Cheats::SearchErrorCode::Success)
      throw ToolError("The search failed.");
  }
  else if (action != "results")
  {
    throw ToolError("action is new, next or results.");
  }

  const size_t count = s_cheat_session->GetResultCount();
  const size_t page_size =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("page_size", 50), 1, 200));
  std::string text = fmt::format("{} candidate address(es).", count);
  if (count > 0 && count <= 5000)
  {
    // Refresh the shown values.
    std::unique_ptr<Cheats::CheatSearchSessionBase> view =
        s_cheat_session->ClonePartial(0, std::min(count, page_size));
    view->SetFilterType(Cheats::FilterType::DoNotFilter);
    {
      Core::CPUThreadGuard guard(system);
      view->RunSearch(guard);
    }
    text += " Address = current value:\n";
    for (size_t i = 0; i < view->GetResultCount(); ++i)
    {
      text += fmt::format("  {}: {}\n", DescribeAddress(view->GetResultAddress(i)),
                          view->GetResultValueAsString(i, false));
    }
  }
  else if (count > 5000)
  {
    text += " Narrow it down with more searches (change the game state between searches, or "
            "use filter=previous with compare=equal to keep the values that did not change).";
  }
  return ToolResult::Text(std::move(text));
}

ToolResult TestMemoryWrite(const ToolArgs& args)
{
  const ExperimentOptions options = ParseExperimentOptions(args);
  const picojson::array* writes = args.GetArray("writes");
  if (!writes || writes->empty())
    throw ToolError("writes is a list of {address, type, value}.");

  struct Write
  {
    u32 address;
    u32 size;
    u32 value;
  };
  auto parsed = std::make_shared<std::vector<Write>>();
  for (const picojson::value& item : *writes)
  {
    if (!item.is<picojson::object>())
      throw ToolError("Each write is {address, type, value}.");
    const ToolArgs write(item.get<picojson::object>());
    const std::string type = write.GetString("type", "u32");
    const u32 size = type == "u8" ? 1 : type == "u16" ? 2 : type == "u32" ? 4 : 0;
    if (!size)
      throw ToolError("type is u8, u16 or u32.");
    const std::optional<u64> value = write.GetHex("value");
    if (!value)
      throw ToolError("Each write needs a hex value.");
    parsed->push_back({write.RequireAddress("address"), size, static_cast<u32>(*value)});
  }
  const bool every_field = args.GetBool("every_field", true);

  const auto write_all = [parsed](const Core::CPUThreadGuard& guard) {
    for (const Write& write : *parsed)
    {
      if (write.size == 1)
        PowerPC::MMU::HostTryWrite<u8>(guard, static_cast<u8>(write.value), write.address);
      else if (write.size == 2)
        PowerPC::MMU::HostTryWrite<u16>(guard, static_cast<u16>(write.value), write.address);
      else
        PowerPC::MMU::HostTryWrite<u32>(guard, write.value, write.address);
    }
  };
  auto& system = GetSystem();
  ToolResult images;
  const ExperimentResult result = RunExperiment(
      options, write_all, [](const Core::CPUThreadGuard&) {},
      every_field ?
          std::function<void()>([&system, write_all] { write_all(Core::CPUThreadGuard(system)); }) :
          std::function<void()>{},
      &images);

  std::string text =
      fmt::format("{} write(s){}.\n", parsed->size(),
                  every_field ? ", repeated at every VI field like an AR 04 code" : " once");
  text += FormatExperimentResult(result);
  text += "\nCompare frames/field with a baseline (test_patch with no patches) and check that the "
          "game does not run at double speed (a timer or a frame counter address).";
  ToolResult output = ToolResult::Text(std::move(text));
  for (picojson::value& block : images.content)
    output.content.push_back(std::move(block));
  return output;
}
}  // namespace

ExperimentOptions ParseExperimentOptions(const ToolArgs& args)
{
  ExperimentOptions options;
  options.reference = args.GetString("reference");
  options.settle_frames =
      static_cast<u32>(std::clamp<s64>(args.GetInt("settle_frames", 10), 0, 600));
  options.measure_frames =
      static_cast<u32>(std::clamp<s64>(args.GetInt("measure_frames", 30), 1, 1200));
  options.turbo = args.GetBool("turbo", true);
  options.screenshot = args.GetBool("screenshot", true);
  options.max_width = static_cast<u32>(std::clamp<s64>(args.GetInt("max_width", 480), 64, 1280));
  options.hang_timeout =
      std::chrono::milliseconds(std::clamp<s64>(args.GetInt("hang_ms", 3000), 500, 30000));
  options.reload_after = args.GetBool("reload_after", true);
  if (args.Has("look_yaw_deg") || args.Has("look_pitch_deg"))
  {
    options.look_offset =
        std::array<float, 2>{static_cast<float>(args.GetNumber("look_yaw_deg", 0)),
                             static_cast<float>(args.GetNumber("look_pitch_deg", 0))};
  }
  return options;
}

ExperimentResult RunExperiment(const ExperimentOptions& options,
                               const std::function<void(const Core::CPUThreadGuard&)>& apply,
                               const std::function<void(const Core::CPUThreadGuard&)>& revert,
                               const std::function<void()>& per_field, ToolResult* images)
{
  RequireGame();
  auto& system = GetSystem();
  std::string reference_path;
  if (!options.reference.empty())
  {
    RequireSavestatesAllowed();
    reference_path = AgentStatePath(options.reference);
    if (!File::Exists(reference_path))
    {
      throw ToolError(fmt::format("There is no agent savestate \"{}\": make one with save_state "
                                  "label=... first.",
                                  options.reference));
    }
  }

  const ScopedAlertCapture alert_capture;
  const bool was_paused = IsPaused();
  const Server& server = Server::GetInstance();
  const bool had_turbo = server.IsTurboOverrideActive();
  const bool had_mute = server.IsMuteOverrideActive();
  SetAgentSpeed(options.turbo, options.turbo);

  std::optional<std::array<float, 3>> previous_look;
  if (options.look_offset)
  {
    RequireStereoSession();
    previous_look = VR::g_openxr->GetAgentLookOffset();
    VR::g_openxr->SetAgentLookOffset((*options.look_offset)[0], (*options.look_offset)[1], 0.0f);
  }

  const auto restore = [&] {
    if (previous_look && VR::g_openxr)
      VR::g_openxr->SetAgentLookOffset((*previous_look)[0], (*previous_look)[1],
                                       (*previous_look)[2]);
    SetAgentSpeed(had_turbo, had_mute);
  };

  ExperimentResult result;
  const auto start_time = std::chrono::steady_clock::now();
  try
  {
    INFO_LOG_FMT(OPENXR, "AgentServer: experiment: pausing");
    PauseEmulation();
    if (!reference_path.empty())
      LoadStateAndWait(reference_path, 30s);
    INFO_LOG_FMT(OPENXR, "AgentServer: experiment: applying");
    {
      const Core::CPUThreadGuard guard(system);
      apply(guard);
    }

    Common::EventHook field_hook;
    if (per_field)
      field_hook = system.GetVideoEvents().vi_end_field_event.Register(per_field);

    const auto start = Counters().GetSnapshot();
    INFO_LOG_FMT(OPENXR, "AgentServer: experiment: running");
    ResumeEmulation();
    result.hung = !WaitForXfbCopies(start.xfb_copies + options.settle_frames, options.hang_timeout);
    const auto measure_start = Counters().GetSnapshot();
    if (!result.hung)
    {
      result.hung = !WaitForXfbCopies(measure_start.xfb_copies + options.measure_frames,
                                      options.hang_timeout);
    }
    const auto measure_end = Counters().GetSnapshot();

    const auto samples = Counters().GetSamplesAfter(measure_start.xfb_copies);
    if (!samples.empty())
    {
      result.draw_calls_min = std::numeric_limits<u32>::max();
      for (const FrameCounters::FrameSample& sample : samples)
      {
        if (sample.xfb_copy > measure_end.xfb_copies)
          break;
        result.draw_calls_avg += sample.draw_calls;
        result.prims_avg += sample.prims;
        result.draw_calls_min = std::min(result.draw_calls_min, sample.draw_calls);
        result.draw_calls_max = std::max(result.draw_calls_max, sample.draw_calls);
        ++result.xfb_copies;
      }
      if (result.xfb_copies)
      {
        result.draw_calls_avg /= result.xfb_copies;
        result.prims_avg /= result.xfb_copies;
      }
      else
      {
        result.draw_calls_min = 0;
      }
    }
    const u64 fields = measure_end.fields - measure_start.fields;
    if (fields)
    {
      result.frames_per_field =
          static_cast<double>(measure_end.unique_presents - measure_start.unique_presents) / fields;
    }

    INFO_LOG_FMT(OPENXR, "AgentServer: experiment: measured {} frames{}", result.xfb_copies,
                 result.hung ? " (hung)" : "");
    if (options.screenshot && images && !result.hung)
    {
      try
      {
        result.screenshot_text = AppendScreenshots(*images, options.max_width, true, true);
      }
      catch (const ToolError& e)
      {
        result.screenshot_text = std::string("no screenshot: ") + e.what();
      }
    }

    INFO_LOG_FMT(OPENXR, "AgentServer: experiment: pausing to revert");
    PauseEmulation();
    field_hook.reset();
    {
      const Core::CPUThreadGuard guard(system);
      revert(guard);
    }
    if (!reference_path.empty() && options.reload_after)
      LoadStateAndWait(reference_path, 30s);
  }
  catch (const ToolError& e)
  {
    restore();
    std::string message = e.what();
    for (const std::string& alert : ScopedAlertCapture::Take())
      message += "\nAlert raised (answered automatically): " + alert;
    throw ToolError(message);
  }
  catch (...)
  {
    restore();
    throw;
  }
  restore();
  result.alerts = ScopedAlertCapture::Take();
  if (!was_paused)
    ResumeEmulation();

  result.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
  return result;
}

std::string FormatExperimentResult(const ExperimentResult& result)
{
  std::string alerts;
  for (const std::string& alert : result.alerts)
    alerts += "\nAlert raised (answered automatically): " + alert;
  if (result.hung)
  {
    return fmt::format("HUNG: no new frame for the hang timeout (the game froze or stopped "
                       "drawing). {} frame(s) measured before. ({:.1f} s){}",
                       result.xfb_copies, result.seconds, alerts);
  }
  std::string text = fmt::format(
      "{} frames measured: draw calls avg {:.1f} (min {}, max {}), primitives avg {:.0f}, {:.2f} "
      "unique frames per VI field. ({:.1f} s)",
      result.xfb_copies, result.draw_calls_avg, result.draw_calls_min, result.draw_calls_max,
      result.prims_avg, result.frames_per_field, result.seconds);
  if (!result.screenshot_text.empty())
    text += " Screenshot: " + result.screenshot_text + ".";
  text += alerts;
  return text;
}

void RegisterExperimentTools(ToolRegistry& registry, Server& server)
{
  server.AddEmulationStoppedCallback([] { s_cheat_session.reset(); });

  const auto patch_schema = [] {
    return Schema()
        .Hex("address", "Function entry (or any instruction) address.")
        .String("template", "li_r3_1 (return 1), li_r3_0 (return 0) or nop_blr (return now).",
                {"li_r3_1", "li_r3_0", "nop_blr"})
        .HexArray("words", "Instruction words instead of a template.")
        .Build();
  };
  const auto experiment_schema = [](Schema schema) {
    schema
        .String("reference", "Agent savestate label to start from (save_state label=...); "
                             "default: the current state.")
        .Integer("settle_frames", "Frames before measuring (default 10).", 0, 600)
        .Integer("measure_frames", "Frames measured (default 30).", 1, 1200)
        .Boolean("turbo", "Run unthrottled and muted (default true).")
        .Boolean("screenshot", "Screenshot both eyes at the end (default true).")
        .Integer("max_width", "Screenshot width (default 480).", 64, 1280)
        .Integer("hang_ms", "No new frame for this long = hung (default 3000).", 500, 30000)
        .Boolean("reload_after", "Reload the reference afterwards (default true).")
        .Number("look_yaw_deg", "Turn the virtual head during the test (VR), e.g. 90 or 180.")
        .Number("look_pitch_deg", "Look up/down during the test (VR).");
    return schema;
  };

  registry.Add(
      {.name = "profile_functions",
       .title = "Profile functions",
       .description =
           "Runs the game for some frames while Dolphin's Branch Watch records every call, and "
           "lists the most called functions per frame with their number of call sites, size, "
           "float compares and returns: the starting point to find a culling test (or a "
           "frame-pacing function). Slows the game while it records.",
       .input_schema = Schema()
                           .Integer("frames", "Frames profiled (default 60).", 5, 600)
                           .Integer("top", "Rows (default 40).", 1, 200)
                           .Integer("max_size", "Only functions up to this many bytes.")
                           .String("name_regex", "Filter by name.")
                           .Build(),
       .handler = ProfileFunctions});

  registry.Add(
      {.name = "trace_function",
       .title = "Trace a function",
       .description =
           "Records each call of a function over some frames without stopping the game: calls "
           "per field, callers, r3 at entry and at return (a bool-like return points to a test "
           "such as a visibility check).",
       .input_schema =
           Schema()
               .Hex("address", "Function address.")
               .String("symbol", "Function name.")
               .Integer("frames", "Frames traced (default 10).", 1, 300)
               .Integer("max_hits", "Stop after this many hits (default 4000).", 1, 100000)
               .Boolean("record_returns", "Trace the returns too (default true).")
               .String("condition", "Only entries where this holds, e.g. \"r3 != 0\".")
               .Build(),
       .handler = TraceFunction});

  registry.Add(
      {.name = "trace_memory",
       .title = "Trace memory accesses",
       .description =
           "Records which instructions read or write an address over some frames, with the "
           "values, without stopping the game. Finds the code behind a variable from "
           "cheat_search (e.g. the frame interval for a 60fps code).",
       .input_schema = Schema()
                           .Hex("address", "Address.")
                           .Integer("size", "Bytes (default 4).", 1, 0x1000)
                           .Boolean("write", "Default true.")
                           .Boolean("read", "Default false.")
                           .Integer("frames", "Default 10.", 1, 300)
                           .Integer("max_hits", "Default 2000.", 1, 50000)
                           .Required({"address"})
                           .Build(),
       .handler = TraceMemory});

  registry.Add(
      {.name = "test_patch",
       .title = "Test a code patch",
       .description =
           "Experiment: loads the reference savestate, writes the patch(es), runs settle + "
           "measure frames (optionally with the virtual head turned), reports draw calls, "
           "primitives and frame rate, takes a screenshot, then restores the code and reloads "
           "the reference. No patch = baseline. A patch that freezes the game is reported as "
           "hung.",
       .input_schema =
           experiment_schema(
               Schema().ArrayOf("patches", "Patches {address, template | words}.", patch_schema()))
               .Build(),
       .handler = TestPatch});

  registry.Add(
      {.name = "test_patches",
       .title = "Compare code patches",
       .description =
           "Runs test_patch for a baseline and each candidate (one patch each) and returns a "
           "comparison table, with screenshots of the baseline and the best candidates.",
       .input_schema =
           experiment_schema(Schema()
                                 .ArrayOf("candidates",
                                          "1 to 12 patches {address, template | words}.",
                                          patch_schema())
                                 .Boolean("include_baseline", "Default true.")
                                 .String("images", "Default best.", {"best", "all", "none"}))
               .Required({"candidates"})
               .Build(),
       .handler = TestPatches});

  registry.Add({.name = "save_patch_as_ar_code",
                .title = "Save a patch as an AR code",
                .description = "Turns a patch (template or words at an address) into an Action "
                               "Replay code and saves it to the game's codes (see add_code).",
                .input_schema = Schema()
                                    .Hex("address", "Patch address.")
                                    .String("template", "li_r3_1, li_r3_0 or nop_blr.",
                                            {"li_r3_1", "li_r3_0", "nop_blr"})
                                    .HexArray("words", "Instruction words instead of a template.")
                                    .String("name", "Code name.")
                                    .Boolean("enabled", "Default true.")
                                    .Boolean("replace", "Replace a code of the same name.")
                                    .Required({"address"})
                                    .Build(),
                .destructive = true,
                .handler = SavePatchAsARCode});

  registry.Add(
      {.name = "measure_frame_rate",
       .title = "Measure the game's frame rate",
       .description =
           "Unique frames per VI field over some fields: about 0.5 for a 30 fps game on a 60 Hz "
           "VI, 1.0 for 60 fps. The reference for 60fps codes.",
       .input_schema = Schema().Integer("fields", "Default 120.", 10, 1200).Build(),
       .read_only = true,
       .handler = MeasureFrameRate});

  registry.Add({.name = "watch_memory",
                .title = "Sample memory each field",
                .description = "Reads up to 16 addresses at every VI field and summarizes each "
                               "(constant, counts up by 1 each field, changes...).",
                .input_schema = Schema()
                                    .HexArray("addresses", "Addresses.")
                                    .String("type", "Default u32.", {"u8", "u16", "u32", "f32"})
                                    .Integer("fields", "Default 60.", 2, 600)
                                    .Required({"addresses"})
                                    .Build(),
                .read_only = true,
                .handler = WatchMemory});

  registry.Add(
      {.name = "cheat_search",
       .title = "Search memory",
       .description =
           "Dolphin's cheat search: action=new starts a search over RAM (with a first filter), "
           "next filters the candidates against a value or against their previous values, "
           "results lists them with current values. Change the game state between searches.",
       .input_schema = Schema()
                           .String("action", "Default next.", {"new", "next", "results"})
                           .String("data_type", "For new; default u32.",
                                   {"u8", "u16", "u32", "s8", "s16", "s32", "f32", "f64"})
                           .String("compare", "Default equal.",
                                   {"equal", "not_equal", "less", "less_or_equal", "greater",
                                    "greater_or_equal"})
                           .String("filter", "value (compare with value), previous or none.",
                                   {"value", "previous", "none"})
                           .String("value", "Value to compare with (decimal, or 0x hex).")
                           .Boolean("aligned", "For new; default true.")
                           .Integer("page_size", "Results shown (default 50).", 1, 200)
                           .Build(),
       .handler = CheatSearch});

  registry.Add(
      {.name = "test_memory_write",
       .title = "Test memory writes",
       .description =
           "Experiment like test_patch with data writes instead of code: writes the values (at "
           "every VI field by default, like an AR 04 code), measures the frame rate and draws, "
           "screenshots, then reloads the reference. For 60fps codes: write the frame interval "
           "variable.",
       .input_schema =
           experiment_schema(Schema()
                                 .ArrayOf("writes",
                                          "Writes {address, type (u8/u16/u32), value (hex)}.",
                                          Schema()
                                              .Hex("address", "Address.")
                                              .String("type", "Default u32.", {"u8", "u16", "u32"})
                                              .Hex("value", "Value.")
                                              .Build())
                                 .Boolean("every_field", "Rewrite at every field (default true)."))
               .Required({"writes"})
               .Build(),
       .handler = TestMemoryWrite});
}
}  // namespace AgentServer
