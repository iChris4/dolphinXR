// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR game patches (Core/VRPatch): list, author live, save and tune them; plus the helpers that
// writing one needs (the VR input as a patch sees it, an assembler, a transform search).

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/AgentServer/TransformScan.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "Core/VRPatch/PatchScript.h"
#include "Core/VRPatch/VRPatch.h"

namespace AgentServer
{
namespace
{
using namespace std::chrono_literals;

std::string GameId()
{
  return SConfig::GetInstance().GetGameID();
}

std::optional<u16> Revision()
{
  return SConfig::GetInstance().GetRevision();
}

// Applies queued patch changes: now when paused, else at the next field.
void ApplyChanges()
{
  if (IsPaused())
  {
    VRPatch::ApplyPendingNow(GetSystem());
    return;
  }
  WaitUntil([] { return !VRPatch::HasPendingChanges(); }, 3s);
}

std::string FormatValue(const VRPatch::SettingDef& def, double value)
{
  switch (def.type)
  {
  case VRPatch::SettingType::Bool:
    return value != 0 ? "true" : "false";
  case VRPatch::SettingType::Choice:
  {
    const size_t index = static_cast<size_t>(value);
    return index < def.options.size() ? fmt::format("{} ({})", index, def.options[index]) :
                                        fmt::format("{}", value);
  }
  case VRPatch::SettingType::Number:
  default:
    return fmt::format("{}{}", value, def.unit.empty() ? "" : " " + def.unit);
  }
}

std::string DescribePatch(const VRPatch::PatchInfo& info, bool details)
{
  std::string text = fmt::format(
      "{} [{}{}] {}", info.id, VRPatch::PatchStateName(info.state), info.enabled ? ", enabled" : "",
      info.manifest ? "\"" + info.manifest->title + "\"" : "(manifest error)");
  text += fmt::format(" - {}{}", VRPatch::PatchOriginName(info.origin),
                      info.overrides_sys ? " (replaces the Sys file)" : "");
  if (!info.status.empty())
    text += ": " + info.status;
  if (!info.manifest_error.empty())
    text += "\n    manifest: " + info.manifest_error;
  if (!info.for_this_game)
    text += "\n    not for this game (its games or revision do not match)";
  if (info.state == VRPatch::PatchState::Running)
  {
    text += fmt::format("\n    {:.0f} us per field (max {:.0f}), {} KiB", info.average_us,
                        info.max_us, info.memory_bytes / 1024);
    if (info.scratch_size != 0)
      text += fmt::format(", scratch {:08X} ({} bytes)", info.scratch_base, info.scratch_size);
    if (!info.flags.empty())
      text += fmt::format("\n    raises the flags {}", fmt::join(info.flags, ", "));
  }
  if (!details || !info.manifest)
    return text;

  const VRPatch::Manifest& m = *info.manifest;
  if (!info.path.empty())
    text += "\n    file: " + info.path;
  if (!m.author.empty() || !m.version.empty())
    text += fmt::format("\n    by {} {}", m.author, m.version);
  if (!m.description.empty())
    text += "\n    " + m.description;
  if (!m.games.empty())
    text += "\n    games: " + fmt::format("{}", fmt::join(m.games, ", "));
  if (m.revision)
    text += fmt::format(" revision {}", *m.revision);
  for (const VRPatch::AddressRange& range : m.reserve)
    text += fmt::format("\n    reserves {:08X}-{:08X}", range.start, range.end);
  for (const VRPatch::OverrideRef& conflict : m.conflicts)
  {
    text += fmt::format("\n    turns off the {} override \"{}\" while it runs",
                        VRPatch::OverrideKindName(conflict.kind), conflict.name);
  }
  if (!m.gecko_lines.empty())
    text += fmt::format("\n    {} Gecko line(s)", m.gecko_lines.size());
  for (const VRPatch::SettingDef& def : m.settings)
  {
    const auto value = info.values.find(def.name);
    text += fmt::format("\n    setting {} = {} (\"{}\", default {}", def.name,
                        value == info.values.end() ? "?" : FormatValue(def, value->second),
                        def.label, FormatValue(def, def.default_value));
    if (def.type == VRPatch::SettingType::Number)
      text += fmt::format(", {} to {}", def.min, def.max);
    text += ")";
  }
  return text;
}

std::string FormatLog(const std::vector<VRPatch::LogLine>& lines)
{
  std::string text;
  for (const VRPatch::LogLine& line : lines)
  {
    static constexpr const char* LEVELS[] = {"", "warning: ", "error: "};
    text +=
        fmt::format("  [{}] {}{}\n", line.field, LEVELS[std::min<u8>(line.level, 2)], line.text);
  }
  return text;
}

VRPatch::PatchInfo RequirePatch(const std::string& id)
{
  std::optional<VRPatch::PatchInfo> info = VRPatch::FindPatch(GameId(), Revision(), id);
  if (!info)
  {
    throw ToolError(
        fmt::format("The game has no VR patch \"{}\" (list_vr_patches lists them).", id));
  }
  return std::move(*info);
}

std::string StatusAndLog(const std::string& id, size_t log_lines)
{
  std::string text;
  if (const auto info = VRPatch::FindPatch(GameId(), Revision(), id))
    text = DescribePatch(*info, false);
  const std::vector<VRPatch::LogLine> log = VRPatch::GetLog(id, log_lines);
  if (!log.empty())
    text += "\nLog:\n" + FormatLog(log);
  return text;
}

ToolResult ListPatches(const ToolArgs& args)
{
  const std::string game_id = args.GetString("game_id", GameId());
  if (game_id.empty())
    throw ToolError("No game is running; pass game_id.");
  const std::vector<VRPatch::PatchInfo> patches =
      VRPatch::ListPatches(game_id, game_id == GameId() ? Revision() : std::nullopt);
  std::string text = fmt::format("VR patches of {} ({}):\n", game_id, patches.size());
  for (const VRPatch::PatchInfo& info : patches)
    text += DescribePatch(info, false) + "\n";
  text += fmt::format("Folders: {}<GameID>/ (yours) and {}<GameID>/ (shipped).",
                      File::GetUserPath(D_VRPATCHES_IDX), VRPatch::GetSysPatchDirectory());
  return ToolResult::Text(std::move(text));
}

ToolResult GetPatch(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.RequireString("id");
  const VRPatch::PatchInfo info = RequirePatch(id);
  std::string text = DescribePatch(info, true);
  if (args.GetBool("source", false))
    text += "\nSource:\n" + VRPatch::ReadPatchSource(GameId(), Revision(), id);
  return ToolResult::Text(std::move(text));
}

ToolResult DescribeApi(const ToolArgs&)
{
  return ToolResult::Text(std::string(VRPatch::GetApiReference()));
}

ToolResult SetSource(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.RequireString("id");
  std::string source = args.RequireString("source");
  const std::string error = VRPatch::SetSessionSource(id, std::move(source));
  if (!error.empty())
    throw ToolError("The patch does not load: " + error);
  ApplyChanges();
  const s64 frames = std::clamp<s64>(args.GetInt("frames", 0), 0, 600);
  if (frames > 0)
    AdvanceFrames(static_cast<u32>(frames));
  std::string text = StatusAndLog(id, 20);
  text += "\nLoaded live for this session (not saved); save_vr_patch writes it to the user's "
          "folder.";
  if (IsPaused() && frames == 0)
    text += " Emulation is paused: on_frame runs once frames advance (advance_frames).";
  return ToolResult::Text(std::move(text));
}

ToolResult SavePatch(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.RequireString("id");
  const bool enable = args.GetBool("enable", true);
  std::string path;
  const std::string error = VRPatch::SaveSessionSource(id, enable, &path);
  if (!error.empty())
    throw ToolError(error);
  ApplyChanges();
  return ToolResult::Text(fmt::format(
      "Saved {}{}. To share it, give that file: it goes in User/VRPatches/{}/ (or a folder named "
      "after the first 3 letters of the game ID for every region).",
      path, enable ? " and enabled it for this game" : "", GameId()));
}

ToolResult DiscardSource(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.GetString("id");
  if (id.empty())
    VRPatch::ClearSessionSources();
  else
    VRPatch::ClearSessionSource(id);
  ApplyChanges();
  return ToolResult::Text("Live sources discarded; the saved files run again where enabled.");
}

ToolResult EnablePatch(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.RequireString("id");
  const bool enabled = args.GetBool("enabled", true);
  RequirePatch(id);
  if (!VRPatch::SetPatchEnabled(GameId(), id, enabled))
    throw ToolError("The user's GameSettingsVR file could not be written.");
  ApplyChanges();
  // A live source runs whether the patch is enabled or not.
  const std::string_view live =
      !enabled && VRPatch::HasSessionSource(id) ?
          " Its live source from set_vr_patch_source still runs: discard_vr_patch_source stops "
          "it." :
          "";
  return ToolResult::Text(fmt::format(
      "{} {} for {} (saved in GameSettingsVR/{}.ini [VRPatch]).{}\n{}", id,
      enabled ? "enabled" : "disabled", GameId(), GameId(), live, StatusAndLog(id, 10)));
}

ToolResult SetSetting(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.RequireString("id");
  const std::string name = args.RequireString("name");
  const VRPatch::PatchInfo info = RequirePatch(id);
  if (!info.manifest || !info.manifest->FindSetting(name))
    throw ToolError(fmt::format("{} has no setting \"{}\" (get_vr_patch lists them).", id, name));
  double value = 0;
  const picojson::object& raw = args.Raw();
  const auto it = raw.find("value");
  if (it == raw.end())
    throw ToolError("value is required (a number, or true/false).");
  if (it->second.is<bool>())
    value = it->second.get<bool>() ? 1 : 0;
  else if (it->second.is<double>())
    value = it->second.get<double>();
  else
    throw ToolError("value is a number, or true/false.");
  if (!VRPatch::SetPatchSetting(GameId(), id, name, value))
    throw ToolError("The setting could not be saved.");
  ApplyChanges();
  return ToolResult::Text(
      fmt::format("{}.{} set and saved.\n{}", id, name, DescribePatch(RequirePatch(id), true)));
}

ToolResult Reload(const ToolArgs&)
{
  RequireGame();
  VRPatch::ReloadPatches();
  ApplyChanges();
  std::string text = "Reloaded:\n";
  for (const VRPatch::PatchInfo& info : VRPatch::ListPatches(GameId(), Revision()))
    text += DescribePatch(info, false) + "\n";
  return ToolResult::Text(std::move(text));
}

ToolResult GetLog(const ToolArgs& args)
{
  const std::string id = args.RequireString("id");
  const s64 lines = std::clamp<s64>(args.GetInt("lines", 40), 1, 300);
  const std::vector<VRPatch::LogLine> log = VRPatch::GetLog(id, static_cast<size_t>(lines));
  if (log.empty())
    return ToolResult::Text(fmt::format("{} has logged nothing in this game session.", id));
  return ToolResult::Text(fmt::format("{} (field: line):\n{}", id, FormatLog(log)));
}

std::string FormatPose(const Common::VR::OpenXRPoseState& pose)
{
  if (!pose.valid)
    return "invalid";
  return fmt::format("pos ({:.3f} {:.3f} {:.3f}) rot ({:.4f} {:.4f} {:.4f} {:.4f})",
                     pose.position[0], pose.position[1], pose.position[2], pose.orientation[0],
                     pose.orientation[1], pose.orientation[2], pose.orientation[3]);
}

ToolResult GetInput(const ToolArgs&)
{
  const Common::VR::OpenXRInputSnapshot s = Common::VR::OpenXRInputState::GetGameSnapshot();
  std::string text =
      fmt::format("VR input as patches see it (meters, OpenXR reference space, "
                  "+Y up, -Z forward). Runtime {}, focused {}, VR menu {}.\n",
                  s.runtime_active ? "active" : "inactive", s.session_focused ? "yes" : "no",
                  s.menu.input_withheld ? "open (input withheld)" : "closed");
  text += "head: " + FormatPose(s.head_pose) + "\n";
  for (size_t hand = 0; hand < 2; ++hand)
  {
    const Common::VR::OpenXRControllerState& c = s.controllers[hand];
    text += fmt::format("{} ({}):\n", hand == 0 ? "left" : "right",
                        c.connected ? s.interaction_profiles[hand] : "not connected");
    text += "  aim: " + FormatPose(c.aim_pose) + "\n";
    text += "  grip: " + FormatPose(c.grip_pose) + "\n";
    text += "  view (from the game camera): " + FormatPose(c.view_pose) + "\n";
    std::vector<std::string> pressed;
    const std::pair<bool, const char*> buttons[] = {{c.primary_button, "primary"},
                                                    {c.secondary_button, "secondary"},
                                                    {c.button_3, "button_3"},
                                                    {c.button_4, "button_4"},
                                                    {c.menu_button, "menu"},
                                                    {c.shoulder_button, "shoulder"},
                                                    {c.trigger_button, "trigger"},
                                                    {c.squeeze_button, "squeeze"},
                                                    {c.thumbstick_button, "thumbstick"}};
    for (const auto& [down, name] : buttons)
    {
      if (down)
        pressed.emplace_back(name);
    }
    text += fmt::format("  pressed: {}; trigger {:.2f} squeeze {:.2f} stick ({:.2f} {:.2f})\n",
                        pressed.empty() ? "none" : fmt::format("{}", fmt::join(pressed, " ")),
                        c.trigger_value, c.squeeze_value, c.thumbstick_x, c.thumbstick_y);
  }
  return ToolResult::Text(std::move(text));
}

// ---- assemble ----

ToolResult Assemble(const ToolArgs& args)
{
  const std::string source = args.RequireString("text");
  const u32 address = args.RequireAddress("address");
  const std::string format = args.GetString("format", "words");
  if (format != "words" && format != "ar" && format != "gecko_c2")
    throw ToolError("format is words, ar or gecko_c2.");
  const auto result = Common::GekkoAssembler::Assemble(source, address);
  if (Common::GekkoAssembler::IsFailure(result))
    throw ToolError(Common::GekkoAssembler::GetFailure(result).FormatError());

  std::string text;
  for (const Common::GekkoAssembler::CodeBlock& block : Common::GekkoAssembler::GetT(result))
  {
    if (block.instructions.empty())
      continue;
    std::vector<u32> words;
    for (size_t i = 0; i + 3 < block.instructions.size(); i += 4)
    {
      words.push_back(static_cast<u32>(block.instructions[i]) << 24 |
                      static_cast<u32>(block.instructions[i + 1]) << 16 |
                      static_cast<u32>(block.instructions[i + 2]) << 8 | block.instructions[i + 3]);
    }
    if (format == "words")
    {
      text += fmt::format("{:08X}:", block.block_address);
      for (const u32 word : words)
        text += fmt::format(" 0x{:08X},", word);
      text.pop_back();
      text += '\n';
    }
    else if (format == "ar")
    {
      for (size_t i = 0; i < words.size(); ++i)
      {
        text += fmt::format(
            "{:08X} {:08X}\n",
            ((block.block_address + static_cast<u32>(i) * 4) & 0x01FFFFFF) | 0x04000000, words[i]);
      }
    }
    else
    {
      // C2: insert the code at the address; the handler appends the branch back. A line is two
      // words; the last line ends with 00000000.
      const bool odd = words.size() % 2 == 1;
      const size_t lines = (words.size() + (odd ? 1 : 2)) / 2;
      text +=
          fmt::format("C{:07X} {:08X}\n", (block.block_address & 0x01FFFFFF) | 0x02000000, lines);
      std::vector<u32> body = words;
      if (odd)
        body.push_back(0);
      else
      {
        body.push_back(0x60000000);
        body.push_back(0);
      }
      for (size_t i = 0; i + 1 < body.size(); i += 2)
        text += fmt::format("{:08X} {:08X}\n", body[i], body[i + 1]);
    }
  }
  if (format == "gecko_c2")
  {
    text += "The code runs from the Gecko code list: branch only to labels inside it (the branch "
            "back to address + 4 is added by the handler).";
  }
  return ToolResult::Text(std::move(text));
}

// ---- find_transforms ----

struct StoredScan
{
  TransformScan::Layout layout;
  std::vector<TransformScan::Hit> hits;
};

std::mutex s_scans_mutex;
std::deque<std::pair<std::string, StoredScan>> s_scans;
u32 s_scan_counter = 0;

ToolResult FindTransforms(const ToolArgs& args)
{
  RequireGame();
  TransformScan::Options options;
  const auto layout = TransformScan::ParseLayout(args.GetString("layout", "3x4"));
  if (!layout)
    throw ToolError("layout is 3x3, 3x4 or 4x4.");
  options.layout = *layout;
  options.tolerance = static_cast<float>(std::clamp(args.GetNumber("tolerance", 0.02), 1e-4, 0.3));
  options.exclude_identity = args.GetBool("exclude_identity", true);
  options.min_translation = static_cast<float>(args.GetNumber("min_translation", 0));
  options.max_translation = static_cast<float>(args.GetNumber("max_translation", 1e7));
  const std::string region = args.GetString("region", "mem1");
  const size_t max_results =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("max_results", 40), 1, 200));
  const u32 address_min = args.GetAddress("address_min").value_or(0x80000000);
  const u32 address_max = args.GetAddress("address_max").value_or(0xFFFFFFFF);
  const std::string compare_to = args.GetString("compare_to");
  std::string label = args.GetString("label");

  TransformScan::Result result;
  {
    Core::CPUThreadGuard guard(GetSystem());
    Memory::MemoryManager& memory = GetSystem().GetMemory();
    const auto scan_range = [&](u8* data, u32 base, u32 size) {
      if (!data || size == 0)
        return;
      const u32 start = std::max(base, address_min);
      const u64 end =
          std::min<u64>(static_cast<u64>(base) + size, static_cast<u64>(address_max) + 1);
      if (end <= start)
        return;
      TransformScan::Result part = TransformScan::Scan(
          std::span<const u8>(data + (start - base), static_cast<size_t>(end - start)), start,
          options);
      result.truncated |= part.truncated;
      result.hits.insert(result.hits.end(), part.hits.begin(), part.hits.end());
    };
    if (region == "mem1" || region == "all")
      scan_range(memory.GetRAM(), 0x80000000, memory.GetRamSizeReal());
    if ((region == "mem2" || region == "all") && GetSystem().IsWii())
      scan_range(memory.GetEXRAM(), 0x90000000, memory.GetExRamSizeReal());
    if (region != "mem1" && region != "mem2" && region != "all")
      throw ToolError("region is mem1, mem2 or all.");
  }

  std::string text;
  if (!compare_to.empty())
  {
    StoredScan before;
    {
      std::lock_guard lock(s_scans_mutex);
      const auto it =
          std::ranges::find_if(s_scans, [&](const auto& s) { return s.first == compare_to; });
      if (it == s_scans.end())
        throw ToolError(fmt::format("There is no scan labeled \"{}\".", compare_to));
      before = it->second;
    }
    if (before.layout != options.layout)
      throw ToolError("compare_to is a scan of another layout.");
    const std::vector<TransformScan::Change> changes =
        TransformScan::Compare(before.hits, result.hits, args.GetNumber("min_angle_deg", 2.0),
                               args.GetNumber("min_move", 0.0));
    text +=
        fmt::format("{} {} transform(s) now; {} of the {} in \"{}\" turned at least {} "
                    "degrees (largest turn first):\n",
                    result.hits.size(), TransformScan::LayoutName(options.layout), changes.size(),
                    before.hits.size(), compare_to, args.GetNumber("min_angle_deg", 2.0));
    for (size_t i = 0; i < changes.size() && i < max_results; ++i)
    {
      const TransformScan::Change& change = changes[i];
      text += fmt::format("  {:08X} turned {:.1f} deg, moved {:.2f}: {}\n", change.after.address,
                          change.angle_deg, change.moved,
                          TransformScan::FormatHit(change.after, options.layout));
    }
  }
  else
  {
    text += fmt::format("{}{} {} transform(s):\n", result.hits.size(),
                        result.truncated ? "+ (truncated)" : "",
                        TransformScan::LayoutName(options.layout));
    for (size_t i = 0; i < result.hits.size() && i < max_results; ++i)
    {
      text += fmt::format("  {:08X} {}\n", result.hits[i].address,
                          TransformScan::FormatHit(result.hits[i], options.layout));
    }
  }

  if (label.empty())
    label = fmt::format("scan{}", ++s_scan_counter);
  {
    std::lock_guard lock(s_scans_mutex);
    std::erase_if(s_scans, [&](const auto& s) { return s.first == label; });
    s_scans.emplace_back(label, StoredScan{options.layout, std::move(result.hits)});
    while (s_scans.size() > 4)
      s_scans.pop_front();
  }
  text += fmt::format("Stored as \"{}\": change the view or move the object, then scan again with "
                      "compare_to=\"{}\".",
                      label, label);
  return ToolResult::Text(std::move(text));
}
}  // namespace

void RegisterVRPatchTools(ToolRegistry& registry, Server& server)
{
  // Live sources belong to the agent's session.
  server.AddServerStoppedCallback([] { VRPatch::ClearSessionSources(); });

  registry.Add({.name = "list_vr_patches",
                .title = "List VR game patches",
                .description = "The game's VR patches (Lua files that add VR features such as "
                               "an object following a controller): state, origin, status.",
                .input_schema = Schema().String("game_id", "Default: the running game.").Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListPatches});

  registry.Add({.name = "get_vr_patch",
                .title = "Show a VR patch",
                .description = "A patch's manifest, settings and status, optionally its source.",
                .input_schema = Schema()
                                    .String("id", "Patch id (its file name without .lua).")
                                    .Boolean("source", "Include the Lua source.")
                                    .Required({"id"})
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetPatch});

  registry.Add({.name = "describe_vr_patch_api",
                .title = "VR patch API reference",
                .description = "The file format and the Lua API of VR patches (mem, vr, pad, asm, "
                               "math). Read it before writing a patch.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = DescribeApi});

  registry.Add(
      {.name = "set_vr_patch_source",
       .title = "Run a VR patch live",
       .description =
           "Loads Lua source as patch `id` of the running game for this session (in place of its "
           "file, enabled or not) and returns its status and log. Syntax and manifest errors come "
           "back as the error. Not saved: save_vr_patch writes it.",
       .input_schema =
           Schema()
               .String("id", "Patch id: the file name, e.g. \"arm-cannon\".")
               .String("source", "The whole Lua file.")
               .Integer("frames", "Frames to advance after loading (default 0).", 0, 600)
               .Required({"id", "source"})
               .Build(),
       .destructive = true,
       .handler = SetSource});

  registry.Add({.name = "save_vr_patch",
                .title = "Save a VR patch",
                .description = "Writes the live source of a patch to "
                               "User/VRPatches/<GameID>/<id>.lua (the shareable file) and, by "
                               "default, enables it for the game.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .Boolean("enable", "Enable it for the game (default true).")
                                    .Required({"id"})
                                    .Build(),
                .destructive = true,
                .handler = SavePatch});

  registry.Add({.name = "discard_vr_patch_source",
                .title = "Discard live VR patch sources",
                .description = "Drops the live source of a patch (or of all, without id): its "
                               "saved file runs again if it is enabled.",
                .input_schema = Schema().String("id", "Patch id; empty = all.").Build(),
                .idempotent = true,
                .handler = DiscardSource});

  registry.Add({.name = "enable_vr_patch",
                .title = "Enable or disable a VR patch",
                .description = "Turns a patch on or off for the game (saved in the user's "
                               "GameSettingsVR [VRPatch] section) and returns its status.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .Boolean("enabled", "Default true.")
                                    .Required({"id"})
                                    .Build(),
                .idempotent = true,
                .handler = EnablePatch});

  registry.Add({.name = "set_vr_patch_setting",
                .title = "Change a VR patch setting",
                .description = "Sets one of a patch's settings (calibration offsets...), saved "
                               "for the game; the running patch sees it at once.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .String("name", "Setting name.")
                                    .Number("value", "A number (true/false for a switch).")
                                    .Required({"id", "name", "value"})
                                    .Build(),
                .idempotent = true,
                .handler = SetSetting});

  registry.Add({.name = "reload_vr_patches",
                .title = "Reload the VR patches",
                .description = "Restarts the game's patches from their files (after editing a "
                               "file by hand).",
                .input_schema = Schema().Build(),
                .handler = Reload});

  registry.Add({.name = "get_vr_patch_log",
                .title = "Read a VR patch's log",
                .description = "What a patch logged (log/warn) and its errors, with the field "
                               "numbers.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .Integer("lines", "Last lines, default 40.", 1, 300)
                                    .Required({"id"})
                                    .Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetLog});

  registry.Add({.name = "get_vr_input",
                .title = "Read the VR input",
                .description = "The headset and controller poses and buttons as patches see "
                               "them (vr.*), to check axes and calibrate.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .handler = GetInput});

  registry.Add(
      {.name = "assemble",
       .title = "Assemble PowerPC code",
       .description =
           "Assembles Gekko code at an address: the words (for mem.write_code / ensure_code in a "
           "patch), Action Replay 04 lines, or a Gecko C2 insertion at that address. A bare number "
           "after b/bl is a displacement; write absolute targets in backquotes: b `0x80040FF0`.",
       .input_schema = Schema()
                           .String("text", "Assembly, one instruction per line; labels allowed.")
                           .Hex("address", "Address of the first instruction (C2: the hook).")
                           .String("format", "Default words.", {"words", "ar", "gecko_c2"})
                           .Required({"text", "address"})
                           .Build(),
       .read_only = true,
       .idempotent = true,
       .handler = Assemble});

  registry.Add(
      {.name = "find_transforms",
       .title = "Find transforms in memory",
       .description =
           "Searches RAM for rotation matrices (a camera, a weapon, an actor). Each scan is "
           "stored under a label; scanning again with compare_to after turning the camera or "
           "moving the object lists the ones that turned, largest first. Confirm with "
           "watch_memory or test_memory_write.",
       .input_schema =
           Schema()
               .String("layout",
                       "3x4 (default: GameCube Mtx, translation in column 3), 3x3 or "
                       "4x4.",
                       {"3x3", "3x4", "4x4"})
               .String("region", "mem1 (default), mem2 (Wii) or all.", {"mem1", "mem2", "all"})
               .Hex("address_min", "Lowest address.")
               .Hex("address_max", "Highest address.")
               .Number("tolerance", "Row length and dot product tolerance, default 0.02.")
               .Boolean("exclude_identity", "Skip identity rotations (default true).")
               .Number("min_translation", "Skip transforms closer to the origin (game units).")
               .Number("max_translation", "Skip transforms farther than this.")
               .Integer("max_results", "Lines shown, default 40.", 1, 200)
               .String("label", "Name to store this scan under (default scanN).")
               .String("compare_to", "Label of an earlier scan: list what turned since.")
               .Number("min_angle_deg", "With compare_to: smallest turn listed, default 2.")
               .Number("min_move", "With compare_to: smallest move listed, default 0.")
               .Build(),
       .read_only = true,
       .handler = FindTransforms});
}
}  // namespace AgentServer
