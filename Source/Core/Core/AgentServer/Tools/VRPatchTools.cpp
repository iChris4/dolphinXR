// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR game patches (Core/VRPatch): list, author live, save and tune them; plus the helpers that
// writing one needs (the VR input as a patch sees it, an assembler, a transform search).

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/PointerScan.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/AgentServer/TransformScan.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "Core/VRPatch/PatchScript.h"
#include "Core/VRPatch/StickTurn.h"
#include "Core/VRPatch/VRPatch.h"
#include "VideoCommon/VR/CameraAnchorMath.h"
#include "VideoCommon/VR/OpenXRManager.h"

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

// Applies queued patch changes: now when paused, else at the next field. Returns "" once they are
// applied, else a note for the result (the status shown predates them).
std::string ApplyChanges()
{
  if (IsPaused())
    VRPatch::ApplyPendingNow(GetSystem());
  else
    WaitUntil([] { return !VRPatch::HasPendingChanges(); }, 3s);
  if (!VRPatch::HasPendingChanges())
    return {};
  return "\nNot applied yet: the game has not run a field for 3 s (loading, or stalled after a "
         "savestate load). The change applies at its next field; the status above predates it.";
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
  std::string enabled;
  if (info.session_enabled)
  {
    enabled = fmt::format(", {} for this session (saved: {})",
                          *info.session_enabled ? "enabled" : "disabled",
                          info.saved_enabled ? "enabled" : "disabled");
  }
  else if (info.enabled)
  {
    enabled = ", enabled";
  }
  std::string text =
      fmt::format("{} [{}{}] {}", info.id, VRPatch::PatchStateName(info.state), enabled,
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
    text += fmt::format("\n    setting {} = {}", def.name,
                        value == info.values.end() ? "?" : FormatValue(def, value->second));
    if (info.session_settings.contains(def.name))
    {
      const auto saved = info.saved_values.find(def.name);
      text += fmt::format(" for this session (saved {})",
                          saved == info.saved_values.end() ? "?" : FormatValue(def, saved->second));
    }
    text += fmt::format(" (\"{}\", default {}", def.label, FormatValue(def, def.default_value));
    if (def.type == VRPatch::SettingType::Number)
      text += fmt::format(", {} to {}", def.min, def.max);
    text += ")";
  }
  return text;
}

// With several runs of the patch, a line marks where each one starts.
std::string FormatLog(const std::vector<VRPatch::LogLine>& lines)
{
  std::string text;
  const bool several_runs = !lines.empty() && lines.front().run != lines.back().run;
  for (size_t i = 0; i < lines.size(); ++i)
  {
    const VRPatch::LogLine& line = lines[i];
    if (several_runs && (i == 0 || lines[i - 1].run != line.run))
      text += fmt::format("  --- run {} ---\n", line.run);
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
  const std::vector<VRPatch::LogLine> log = VRPatch::GetLog(id, log_lines, true);
  if (!log.empty())
    text += "\nLog (this run):\n" + FormatLog(log);
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
  std::string pending = ApplyChanges();
  const s64 frames = std::clamp<s64>(args.GetInt("frames", 0), 0, 600);
  if (frames > 0)
    AdvanceFrames(static_cast<u32>(frames));
  std::string text = StatusAndLog(id, 20);
  text += "\nLoaded live for this session (not saved); save_vr_patch writes it to the user's "
          "folder.";
  if (IsPaused() && frames == 0)
    text += " Emulation is paused: on_frame runs once frames advance (advance_frames).";
  if (frames == 0)
    text += pending;
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
  const std::string pending = ApplyChanges();
  return ToolResult::Text(fmt::format(
      "Saved {}{}. To share it, give that file: it goes in User/VRPatches/{}/ (or a folder named "
      "after the first 3 letters of the game ID for every region).{}",
      path, enable ? " and enabled it for this game" : "", GameId(), pending));
}

ToolResult DiscardSource(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.GetString("id");
  if (id.empty())
    VRPatch::ClearSessionSources();
  else
    VRPatch::ClearSessionSource(id);
  VRPatch::ClearSessionValues(id);
  const std::string pending = ApplyChanges();
  return ToolResult::Text("Session changes discarded (live sources, and the enable states and "
                          "settings set without save): the saved files and values apply again." +
                          pending);
}

ToolResult EnablePatch(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.RequireString("id");
  const bool enabled = args.GetBool("enabled", true);
  const bool save = args.GetBool("save", false);
  RequirePatch(id);
  if (save && !VRPatch::SetPatchEnabled(GameId(), id, enabled))
    throw ToolError("The user's GameSettingsVR file could not be written.");
  if (!save && !VRPatch::SetSessionEnabled(id, enabled))
    throw ToolError("No game is running.");
  const std::string pending = ApplyChanges();
  // A live source runs whether the patch is enabled or not.
  const std::string_view live =
      !enabled && VRPatch::HasSessionSource(id) ?
          " Its live source from set_vr_patch_source still runs: discard_vr_patch_source stops "
          "it." :
          "";
  const std::string where =
      save ? fmt::format("saved in GameSettingsVR/{}.ini [VRPatch]", GameId()) :
             "for this session only: the user's files are unchanged, save=true writes it, "
             "discard_vr_patch_source drops it";
  return ToolResult::Text(fmt::format("{} {} for {} ({}).{}\n{}{}", id,
                                      enabled ? "enabled" : "disabled", GameId(), where, live,
                                      StatusAndLog(id, 10), pending));
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
  const bool save = args.GetBool("save", false);
  if (save && !VRPatch::SetPatchSetting(GameId(), id, name, value))
    throw ToolError("The setting could not be saved.");
  if (!save && !VRPatch::SetSessionSetting(id, name, value))
    throw ToolError("No game is running.");
  const std::string pending = ApplyChanges();
  return ToolResult::Text(fmt::format("{}.{} set {}.\n{}{}", id, name,
                                      save ? "and saved in the user's GameSettingsVR file" :
                                             "for this session only (save=true writes it)",
                                      DescribePatch(RequirePatch(id), true), pending));
}

ToolResult Reload(const ToolArgs&)
{
  RequireGame();
  VRPatch::ReloadPatches();
  const std::string pending = ApplyChanges();
  std::string text = "Reloaded:\n";
  for (const VRPatch::PatchInfo& info : VRPatch::ListPatches(GameId(), Revision()))
    text += DescribePatch(info, false) + "\n";
  return ToolResult::Text(text + pending);
}

ToolResult GetLog(const ToolArgs& args)
{
  const std::string id = args.RequireString("id");
  const s64 lines = std::clamp<s64>(args.GetInt("lines", 40), 1, 300);
  const bool all_runs = args.GetBool("all_runs", false);
  const std::vector<VRPatch::LogLine> log =
      VRPatch::GetLog(id, static_cast<size_t>(lines), !all_runs);
  if (log.empty())
  {
    return ToolResult::Text(fmt::format("{} has logged nothing {}.", id,
                                        all_runs ? "in this game session" : "since it started"));
  }
  return ToolResult::Text(fmt::format("{} ({}, [field] line):\n{}", id,
                                      all_runs ? "every run in this game session" :
                                                 fmt::format("run {}", log.back().run),
                                      FormatLog(log)));
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
  text += "head view (eyes' center from the game camera): " + FormatPose(s.head_view_pose) + "\n";
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
  // near_x/y/z with camera: rank by the distance from a point of the game's view space.
  const std::optional<double> near_x = args.GetOptionalNumber("near_x");
  const std::optional<double> near_y = args.GetOptionalNumber("near_y");
  const std::optional<double> near_z = args.GetOptionalNumber("near_z");
  const std::optional<u32> camera = args.GetAddress("camera");
  const bool rank = near_x || near_y || near_z;
  if (rank && (!near_x || !near_y || !near_z || !camera))
    throw ToolError("Ranking needs near_x, near_y, near_z and camera.");
  const auto camera_kind =
      VR::CameraAnchorMath::ParseCameraKind(args.GetString("camera_kind", "view"));
  if (!camera_kind)
    throw ToolError("camera_kind is view (world -> view) or world (the camera's transform).");
  std::optional<VR::CameraAnchorMath::Mtx34> view;

  TransformScan::Result result;
  {
    Core::CPUThreadGuard guard(GetSystem());
    if (rank)
    {
      VR::CameraAnchorMath::Mtx34 matrix;
      for (u32 i = 0; i < 12; ++i)
      {
        const auto word = PowerPC::MMU::HostTryRead<u32>(guard, *camera + i * 4);
        if (!word)
          throw ToolError(fmt::format("The camera at {:08X} is not readable.", *camera));
        matrix[i] = std::bit_cast<float>(word->value);
      }
      view = VR::CameraAnchorMath::ViewFromCamera(matrix, *camera_kind);
      if (!view)
        throw ToolError(fmt::format("The camera at {:08X} is not a transform.", *camera));
    }
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
  picojson::array ranked_json;
  // The hits (or the changed ones) by their distance from the point, seen through the camera.
  const auto append_ranked = [&](std::span<const TransformScan::Hit> hits) {
    const std::array<float, 3> point = {static_cast<float>(*near_x), static_cast<float>(*near_y),
                                        static_cast<float>(*near_z)};
    const std::vector<TransformScan::Ranked> ranked =
        TransformScan::RankByViewDistance(hits, *view, point);
    text += fmt::format("Nearest to ({:.1f} {:.1f} {:.1f}) in the view of the camera at {:08X} "
                        "({}):\n",
                        point[0], point[1], point[2], *camera,
                        *camera_kind == VR::CameraAnchorMath::CameraKind::View ? "view" : "world");
    for (size_t i = 0; i < ranked.size() && i < max_results; ++i)
    {
      const TransformScan::Ranked& entry = ranked[i];
      text +=
          fmt::format("  {:08X} d={:.2f} view=({:.2f} {:.2f} {:.2f}) {}\n", entry.hit.address,
                      entry.distance, entry.view_position[0], entry.view_position[1],
                      entry.view_position[2], TransformScan::FormatHit(entry.hit, options.layout));
      picojson::array position, translation, rotation;
      for (float v : entry.view_position)
        position.push_back(JsonFloat(v));
      for (float v : entry.hit.translation)
        translation.push_back(JsonFloat(v));
      for (float v : entry.hit.rotation)
        rotation.push_back(JsonFloat(v));
      ranked_json.push_back(picojson::value(
          picojson::object{{"address", JsonString(fmt::format("{:08X}", entry.hit.address))},
                           {"distance", JsonFloat(entry.distance)},
                           {"view", picojson::value(position)},
                           {"translation", picojson::value(translation)},
                           {"rotation", picojson::value(rotation)}}));
    }
  };
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
    text += fmt::format("{} {} transform(s) now; {} of the {} in \"{}\" turned at least {} "
                        "degrees or moved at least {} (largest turn first):\n",
                        result.hits.size(), TransformScan::LayoutName(options.layout),
                        changes.size(), before.hits.size(), compare_to,
                        args.GetNumber("min_angle_deg", 2.0), args.GetNumber("min_move", 0.0));
    if (rank)
    {
      std::vector<TransformScan::Hit> changed;
      for (const TransformScan::Change& change : changes)
        changed.push_back(change.after);
      append_ranked(changed);
    }
    for (size_t i = 0; !rank && i < changes.size() && i < max_results; ++i)
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
    if (rank)
      append_ranked(result.hits);
    for (size_t i = 0; !rank && i < result.hits.size() && i < max_results; ++i)
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
  ToolResult tool_result = ToolResult::Text(std::move(text));
  if (rank)
    tool_result.SetStructured({{"camera", JsonString(fmt::format("{:08X}", *camera))},
                               {"hits", picojson::value(ranked_json)}});
  return tool_result;
}

// ---- test_camera_anchor ----

// A Lua expression for an address argument: a hex address or a path ("[[805C4F98]+2184]+3C",
// "mem.chain(...)"), which is followed again at every field.
std::string AddressExpression(const ToolArgs& args, const char* key, bool required)
{
  const std::string text = std::string(StripWhitespace(args.GetString(key)));
  if (text.empty())
  {
    if (required)
      throw ToolError(
          fmt::format("{} is required: an address or a path like [[805C4F98]+2184]+3C.", key));
    return "nil";
  }
  if (const std::optional<PointerScan::Path> path = PointerScan::ParsePath(text))
    return PointerScan::FormatLua(*path);
  if (const std::optional<u64> address = ParseHexString(text); address && *address <= 0xFFFFFFFF)
    return fmt::format("0x{:08X}", *address);
  throw ToolError(fmt::format("{} is not an address or a path: {}", key, text));
}

std::string LuaString(std::string_view text)
{
  std::string out = "\"";
  for (const char c : text)
  {
    if (c == '\\' || c == '"')
      out += '\\';
    if (c == '\n')
      out += "\\n";
    else
      out += c;
  }
  return out + "\"";
}

std::string LuaNumber(double value)
{
  // Locale-independent, unlike the C library's.
  return fmt::format("{}", value);
}

// The patch test_camera_anchor runs, and the agent saves once the user confirms it.
std::string CameraAnchorPatchSource(const ToolArgs& args, const std::string& title)
{
  static constexpr std::array<std::string_view, 5> ROTATIONS = {"level", "fixed", "full", "camera",
                                                                "yaw"};
  const std::string rotation = args.GetString("rotation", "level");
  const auto rotation_it = std::ranges::find(ROTATIONS, rotation);
  if (rotation_it == ROTATIONS.end())
    throw ToolError("rotation is level, fixed, full, camera or yaw.");
  const std::string kind = args.GetString("camera_kind", "view");
  if (!VR::CameraAnchorMath::ParseCameraKind(kind))
    throw ToolError("camera_kind is view (world -> view) or world (the camera's transform).");
  for (const char* key : {"up_axis", "forward_axis", "bone_up_axis"})
  {
    if (!VR::CameraAnchorMath::ParseAxis(args.GetString(key, "y")))
      throw ToolError(fmt::format("{} is x, y, z, -x, -y or -z.", key));
  }
  const std::string head = AddressExpression(args, "head", true);
  const std::string camera = AddressExpression(args, "camera", true);
  const std::string heading = AddressExpression(args, "heading", false);
  static constexpr std::array<std::string_view, 3> STICKS = {"facing", "head", "camera"};
  const auto stick_it = std::ranges::find(STICKS, args.GetString("stick", "facing"));
  if (stick_it == STICKS.end())
    throw ToolError("stick is facing, head or camera.");
  const double stick_lead = std::clamp(args.GetNumber("stick_lead", 0), 0.0, 4.0);
  const std::string free_byte = AddressExpression(args, "free_byte", false);
  const s64 free_value = args.GetInt("free_value", 0);
  if (free_value < 0 || free_value > 255)
    throw ToolError("free_value is a byte, 0 to 255.");

  std::string hide;
  for (const u64 hash : args.GetHexArray("hide_textures"))
    hide += fmt::format("{}\"{:016x}\"", hide.empty() ? "" : ", ", hash);
  const std::string hide_flag = args.GetString("hide_flag");

  std::string source = fmt::format(
      "-- {}: first-person view from a bone (made with the agent server's test_camera_anchor).\n"
      "-- The VR camera sits in the head bone; the view is level and turns with the character's\n"
      "-- heading, whatever the game's camera does. Needs Camera Anchor on (VR settings).\n"
      "--\n"
      "-- SPDX-License-Identifier: GPL-2.0-or-later\n\n"
      "patch = {{\n"
      "  title = {},\n"
      "  description = \"The VR camera in the character's head (Camera Anchor on).\",\n"
      "  games = {{\"{}\"}},\n",
      GameId(), LuaString(title), GameId());
  if (!hide.empty())
    source += fmt::format("  hide = {{texture = {{{}}}}},\n", hide);
  source += fmt::format(
      "  settings = {{\n"
      "    {{name = \"rotation\", label = \"View rotation\", type = \"choice\",\n"
      "     options = {{\"Level\", \"Fixed\", \"Full\", \"Camera\", \"Yaw\"}}, default = {},\n"
      "     description = \"Level: turns with the character, the game camera does not move the \"\n"
      "       .. \"view. Fixed: the headset does all the turning. Full: the bone's orientation. "
      "\"\n"
      "       .. \"Camera and Yaw: the game camera's turns move the view.\"}},\n"
      "    {{name = \"yaw\", label = \"Yaw offset\", unit = \"deg\", default = {}, min = -180, "
      "max = 180, step = 1}},\n"
      "    {{name = \"right\", label = \"Right\", unit = \"m\", default = {}, min = -1, max = 1, "
      "step = 0.01}},\n"
      "    {{name = \"up\", label = \"Up\", unit = \"m\", default = {}, min = -1, max = 1, "
      "step = 0.01}},\n"
      "    {{name = \"forward\", label = \"Forward\", unit = \"m\", default = {}, min = -1, "
      "max = 1, step = 0.01}},\n"
      "    {{name = \"upm\", label = \"Units per meter (0: the profile's)\", default = {}, "
      "min = 0, max = 2000, step = 0.5}},\n"
      "    {{name = \"smoothing\", label = \"Smoothing (-1: the Camera Anchor setting)\", "
      "default = {}, min = -1, max = 0.95, step = 0.05}},\n"
      "    {{name = \"hide_radius\", label = \"Hide the head's draws within (game units)\", "
      "default = {}, min = 0, max = 200, step = 0.5}},\n"
      "    {{name = \"hide_below\", label = \"Down to under the head (game units, -1: any)\", "
      "default = {}, min = -1, max = 200, step = 0.5}},\n"
      "    {{name = \"stick\", label = \"Stick forward\", type = \"choice\",\n"
      "     options = {{\"The character's facing\", \"Where you look\", \"The game's camera\"}},\n"
      "     default = {},\n"
      "     description = \"What up on the stick means: where the character faces, the \"\n"
      "       .. \"headset's direction, or the game camera's.\"}},\n"
      "  }},\n"
      "}}\n\n",
      rotation_it - ROTATIONS.begin(), LuaNumber(args.GetNumber("yaw_deg", 0)),
      LuaNumber(args.GetNumber("right_m", 0)), LuaNumber(args.GetNumber("up_m", 0)),
      LuaNumber(args.GetNumber("forward_m", 0)), LuaNumber(args.GetNumber("upm", 0)),
      LuaNumber(args.GetNumber("smoothing", -1)), LuaNumber(args.GetNumber("hide_radius", 0)),
      LuaNumber(args.GetNumber("hide_below", -1)), stick_it - STICKS.begin());
  source += fmt::format(
      "local ROTATIONS = {{\"level\", \"fixed\", \"full\", \"camera\", \"yaw\"}}\n"
      "local STICKS = {{\"facing\", \"head\", \"camera\"}}\n\n"
      "local function apply_options()\n"
      "  vr.set_camera_anchor_options{{\n"
      "    rotation = ROTATIONS[settings.rotation + 1], yaw = settings.yaw,\n"
      "    right = settings.right, up = settings.up, forward = settings.forward,\n"
      "    upm = settings.upm > 0 and settings.upm or nil,\n"
      "    smoothing = settings.smoothing >= 0 and settings.smoothing or nil,\n"
      "    hide_radius = settings.hide_radius, hide_below = settings.hide_below,\n"
      "    stick = STICKS[settings.stick + 1], stick_lead = {},\n"
      "    up_axis = \"{}\", forward_axis = \"{}\", bone_up_axis = \"{}\",\n"
      "  }}\n"
      "end\n\n"
      "function on_load() apply_options() end\n"
      "function on_setting_changed() apply_options() end\n\n"
      "-- Followed again at every field: the objects move between rooms.\n"
      "local function head() return {} end\n"
      "local function camera() return {} end\n"
      "local function heading() return {} end\n\n"
      "local fields = 0\n"
      "function on_frame()\n"
      "  local h, c = head(), camera()\n"
      "{}"
      "  local ok = h ~= nil and c ~= nil and\n"
      "             vr.set_camera_anchor_matrices(h, c, \"{}\", heading())\n",
      LuaNumber(stick_lead), args.GetString("up_axis", "y"), args.GetString("forward_axis", "z"),
      args.GetString("bone_up_axis", "y"), head, camera, heading,
      free_byte == "nil" ?
          "" :
          fmt::format("  -- The stick walks only while the game is in free play.\n"
                      "  local byte = {}\n"
                      "  vr.set_camera_anchor_stick(byte ~= nil and mem.u8(byte) == "
                      "{})\n",
                      free_byte, free_value),
      kind);
  if (!hide_flag.empty())
  {
    source +=
        fmt::format("  vr.set_flag({}, ok and vr.camera_anchor_active())\n", LuaString(hide_flag));
  }
  source +=
      "  fields = fields + 1\n"
      "  if fields % 120 == 1 then\n"
      "    log(ok and \"anchor in the head\" or \"no anchor\", h and mem.hex(h) or \"no head\")\n"
      "  end\n"
      "end\n";
  return source;
}

ToolResult TestCameraAnchor(const ToolArgs& args)
{
  RequireGame();
  const std::string id = args.GetString("id", "camera-anchor-test");
  const std::string title = args.GetString("title", "First-person view");
  const std::string source = CameraAnchorPatchSource(args, title);
  if (const std::string error = VRPatch::SetSessionSource(id, source); !error.empty())
    throw ToolError("The generated patch does not load: " + error + "\n" + source);
  std::string pending = ApplyChanges();
  const s64 frames = std::clamp<s64>(args.GetInt("frames", 60), 0, 600);
  if (frames > 0)
    AdvanceFrames(static_cast<u32>(frames));

  const bool enabled = Config::Get(Config::GFX_VR_ENABLE_CAMERA_ANCHOR);
  const bool session = VR::g_openxr && VR::g_openxr->IsSessionRunning();
  const bool flat = Config::Get(Config::GFX_VR_FLAT_SCREEN);
  const bool gate_open = enabled && session && !flat;
  std::string text = StatusAndLog(id, 20);
  text += fmt::format(
      "\nCamera Anchor engine gate: {} (EnableCameraAnchor={}, VR session={}, flat={}).{}\n",
      gate_open ? "open: the view is in the head" : "closed: nothing moves the view", enabled,
      session, flat,
      gate_open ? "" :
                  " The patch still composes the anchor and logs it; without a stereo session "
                  "compare its log with get_draw's position matrix of the head.");
  const std::optional<float> stick = VRPatch::StickTurn::Get();
  text += stick ? fmt::format("Main stick: turned {:.0f} degrees (+ = left) from the game camera's "
                              "heading to the view's.\n",
                              *stick * 57.29578f) :
                  "Main stick: not turned (stick=camera, free_byte not in free play, or the "
                  "anchor does not hold the camera).\n";
  if (frames == 0)
    text += pending;
  text += "\nLoaded live for this session (not saved): tune with set_vr_patch_setting, save with "
          "save_vr_patch id=" +
          id + " once the user confirms it in the headset.\nSource:\n" + source;
  ToolResult result = ToolResult::Text(std::move(text));
  result.SetStructured(
      {{"id", JsonString(id)},
       {"gate_open", JsonBool(gate_open)},
       {"stick_turn_deg", stick ? JsonNumber(*stick * 57.29578f) : picojson::value()},
       {"source", JsonString(source)}});
  return result;
}
}  // namespace

void RegisterVRPatchTools(ToolRegistry& registry, Server& server)
{
  // Live sources, enable states and settings set without save belong to the agent's session.
  server.AddServerStoppedCallback([] {
    VRPatch::ClearSessionSources();
    VRPatch::ClearSessionValues({});
  });

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
                .title = "Discard session changes to VR patches",
                .description = "Drops the agent's session changes to a patch (to all, without "
                               "id): its live source, and the enable state and settings set "
                               "without save. Its saved file and values apply again.",
                .input_schema = Schema().String("id", "Patch id; empty = all.").Build(),
                .idempotent = true,
                .handler = DiscardSource});

  registry.Add({.name = "enable_vr_patch",
                .title = "Enable or disable a VR patch",
                .description = "Turns a patch on or off for this session (the user's files "
                               "are unchanged) and returns its status and log. save=true writes "
                               "it to the user's GameSettingsVR [VRPatch] section: only when the "
                               "user asked to keep it.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .Boolean("enabled", "Default true.")
                                    .Boolean("save", "Write it to the user's file (default false).")
                                    .Required({"id"})
                                    .Build(),
                .idempotent = true,
                .handler = EnablePatch});

  registry.Add({.name = "set_vr_patch_setting",
                .title = "Change a VR patch setting",
                .description = "Sets one of a patch's settings (calibration offsets, test "
                               "switches) for this session; the running patch sees it at once. "
                               "save=true writes it to the user's GameSettingsVR file: only for a "
                               "value the user confirmed.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .String("name", "Setting name.")
                                    .Number("value", "A number (true/false for a switch).")
                                    .Boolean("save", "Write it to the user's file (default false).")
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
                               "numbers, since it last started (a reload, a new live source, "
                               "being enabled again); all_runs adds the earlier runs of this "
                               "game session, each marked.",
                .input_schema = Schema()
                                    .String("id", "Patch id.")
                                    .Integer("lines", "Last lines, default 40.", 1, 300)
                                    .Boolean("all_runs", "Earlier runs too (default false).")
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
               .Number("min_angle_deg",
                       "With compare_to: smallest turn listed, default 2 (0: not a criterion).")
               .Number("min_move", "With compare_to: smallest move listed, default 0 (not a "
                                   "criterion); a change is listed when it passes either.")
               .Number("near_x", "With camera: rank by the distance from this point of the "
                                 "game's view space (e.g. get_draw's position matrix "
                                 "translation of a character's head).")
               .Number("near_y", "See near_x.")
               .Number("near_z", "See near_x.")
               .Hex("camera", "The game camera's matrix (a GameCube Mtx) for near_x/y/z.")
               .String("camera_kind",
                       "view (default: world -> view) or world (the camera's own transform).",
                       {"view", "world"})
               .Build(),
       .read_only = true,
       .handler = FindTransforms});
  registry.Add(
      {.name = "test_camera_anchor",
       .title = "Try a first-person camera from a bone",
       .description =
           "Generates and runs (live, this session) a VR patch that puts the VR camera in a "
           "character's head from memory: the head bone's world transform and the game "
           "camera's matrix (Workflow 7). rotation=level keeps the view level and turned with "
           "the character, so the game's third-person camera no longer moves it. hide_textures "
           "hides the head's draws while the anchor holds the camera. Returns the patch log, "
           "whether the engine applies it (Camera Anchor on, a stereo VR session) and the Lua "
           "source to save with save_vr_patch.",
       .input_schema =
           Schema()
               .String("head", "The head bone's world transform: an address, or a path "
                               "([[805C4F98]+2184]+3C) followed at every field.")
               .String("camera", "The game camera's matrix: an address or a path.")
               .String("camera_kind",
                       "view (default: world -> view, what the game multiplies its models by) "
                       "or world (the camera's own transform).",
                       {"view", "world"})
               .String("heading", "Another bone's transform for the heading (a body root turns "
                                  "more smoothly than a head); default the head.")
               .String("rotation",
                       "level (default), fixed (the heading at engagement), full (the bone's "
                       "orientation), camera or yaw (the game camera's turns move the view).",
                       {"level", "fixed", "full", "camera", "yaw"})
               .String("up_axis", "The world's up: y (default), z, -y, -z, x, -x.",
                       {"x", "y", "z", "-x", "-y", "-z"})
               .String("forward_axis",
                       "The bone axis pointing where the character faces: z (default), x, y, "
                       "-x, -y, -z.",
                       {"x", "y", "z", "-x", "-y", "-z"})
               .String("bone_up_axis", "rotation=full: the bone axis pointing up (default y).",
                       {"x", "y", "z", "-x", "-y", "-z"})
               .Number("yaw_deg", "Degrees about up, + turns left (default 0).")
               .Number("right_m", "Meters to the right (default 0).")
               .Number("up_m", "Meters up (default 0).")
               .Number("forward_m", "Meters forward (default 0).")
               .Number("upm", "The anchor's world scale in game units per meter (default 0: the "
                              "profile's).")
               .Number("smoothing", "Fraction kept per field, 0 to 0.95 (default -1: the Camera "
                                    "Anchor Smoothing setting).")
               .Number("hide_radius",
                       "Game units: the draws centred within this distance of the head bone (and "
                       "not lower than hide_below under it) are hidden while the anchor holds the "
                       "camera, for heads sharing the body's textures. Default 0.")
               .Number("hide_below", "Game units a hidden draw's centre can be under the head "
                                     "bone along up_axis (the bone is at the neck: the head's "
                                     "parts are above it, the shoulders under). Default -1: any.")
               .HexArray("hide_textures", "Texture hashes of the head's draws, hidden while the "
                                          "anchor holds the camera.")
               .String("hide_flag", "Also raise this override flag while the anchor applies, for "
                                    "profile entries (condition=<flag>) that hide by shader.")
               .String("stick",
                       "What up on the main stick means while the anchor holds the camera: facing "
                       "(default: where the character faces), head (where the headset looks) or "
                       "camera (the game's own).",
                       {"facing", "head", "camera"})
               .Number("stick_lead",
                       "Camera updates the game's control angle runs ahead of the camera matrix "
                       "(0 to 4, default 0). 1 when the character's path bends with each swing of "
                       "the game camera while up is held (Wind Waker, Twilight Princess).")
               .String("free_byte",
                       "A byte (an address or a path) holding free_value only while the character "
                       "moves freely (not in a menu, a dialog or a cutscene): the stick is turned "
                       "only then. Find it with cheat_search or a logging patch (Workflow 7).")
               .Integer("free_value", "free_byte's value in free play (default 0).", 0, 255)
               .Integer("frames", "Frames to advance after loading (default 60).", 0, 600)
               .String("id", "Patch id (default camera-anchor-test).")
               .String("title", "Patch title (default \"First-person view\").")
               .Required({"head", "camera"})
               .Build(),
       .destructive = true,
       .handler = TestCameraAnchor});
}
}  // namespace AgentServer
