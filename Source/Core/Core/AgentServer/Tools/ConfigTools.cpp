// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR settings (the [VR] keys a game profile's [Graphics.VR] section can set) and the log.

#include <algorithm>
#include <mutex>
#include <optional>
#include <regex>
#include <set>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/LogBuffer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/VRGameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/VRPatch/VRSettingsTable.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/VideoConfig.h"

namespace AgentServer
{
namespace
{
using VRPatch::GetVRSettings;
using VRPatch::VRSetting;

const VRSetting* FindSetting(const std::string& key)
{
  return VRPatch::FindVRSetting(key);
}

const VRSetting& RequireSetting(const std::string& key)
{
  const VRSetting* setting = FindSetting(key);
  if (!setting)
    throw ToolError(fmt::format("Unknown VR setting \"{}\"; list_vr_settings shows them.", key));
  return *setting;
}

// Keys the agent set in the CurrentRun layer, cleared when emulation stops (Dolphin clears that
// layer then too).
std::mutex s_touched_mutex;
std::set<std::string> s_touched_keys;

std::string DescribeSetting(const VRSetting& setting)
{
  const std::optional<std::string> value = Config::GetAsString(setting.location);
  const Config::LayerType layer = Config::GetActiveLayerForConfig(setting.location);
  std::string source = Config::GetLayerName(layer);
  if (layer == Config::LayerType::CurrentRun)
  {
    std::lock_guard lock(s_touched_mutex);
    source = s_touched_keys.contains(setting.location.key) ? "agent (this session)" :
                                                             "game profile or this session";
  }
  return fmt::format("{} = {} ({}, default {}, from {}){}{}", setting.location.key,
                     value.value_or(setting.default_value), setting.type, setting.default_value,
                     value ? source : "default", *setting.note ? "; " : "", setting.note);
}

ToolResult ListVRSettings(const ToolArgs& args)
{
  const std::string filter = args.GetString("filter");
  std::string text;
  for (const VRSetting& setting : GetVRSettings())
  {
    if (!filter.empty() && !Common::CaseInsensitiveContains(setting.location.key, filter))
      continue;
    text += DescribeSetting(setting) + "\n";
  }
  if (text.empty())
    text = "No VR setting matches.";
  return ToolResult::Text(std::move(text));
}

ToolResult GetVRSetting(const ToolArgs& args)
{
  return ToolResult::Text(DescribeSetting(RequireSetting(args.RequireString("key"))));
}

ToolResult SetVRSetting(const ToolArgs& args)
{
  const VRSetting& setting = RequireSetting(args.RequireString("key"));
  const std::string raw_value = args.RequireString("value");
  const std::optional<std::string> value = setting.normalize(raw_value);
  if (!value)
    throw ToolError(fmt::format("\"{}\" is not a valid {} value.", raw_value, setting.type));

  if (Config::GetLayer(Config::LayerType::CurrentRun)->Set(setting.location, *value))
    Config::OnConfigChanged();
  {
    std::lock_guard lock(s_touched_mutex);
    s_touched_keys.insert(setting.location.key);
  }

  std::string text = fmt::format("Set for this session (not saved): {}", DescribeSetting(setting));
  if (Core::IsRunning(GetSystem()) && IsPaused() && args.GetBool("advance", true))
  {
    // The renderer picks settings up at the end of a frame.
    AdvanceFrames(1);
    text += ". Advanced one frame so it applies.";
  }
  text += " Use save_vr_settings to keep it in the game's profile.";
  return ToolResult::Text(std::move(text));
}

ToolResult ResetVRSettings(const ToolArgs& args)
{
  std::vector<std::string> keys = args.GetStringArray("keys");
  {
    std::lock_guard lock(s_touched_mutex);
    if (keys.empty())
      keys.assign(s_touched_keys.begin(), s_touched_keys.end());
  }

  for (const std::string& key : keys)
  {
    const VRSetting& setting = RequireSetting(key);
    if (Config::GetLayer(Config::LayerType::CurrentRun)->DeleteKey(setting.location))
      Config::OnConfigChanged();
    std::lock_guard lock(s_touched_mutex);
    s_touched_keys.erase(setting.location.key);
  }
  // Puts back the values of the game's VR profile that the deletions removed.
  if (Core::IsRunning(GetSystem()))
    SConfig::GetInstance().ReloadGameVRConfigOverrides();
  if (Core::IsRunning(GetSystem()) && IsPaused())
    AdvanceFrames(1);
  return ToolResult::Text(
      fmt::format("Reset {} setting(s) to the game's profile or the global value.", keys.size()));
}

ToolResult SaveVRSettings(const ToolArgs& args)
{
  RequireGame();
  std::vector<std::string> keys = args.GetStringArray("keys");
  if (keys.empty())
  {
    std::lock_guard lock(s_touched_mutex);
    keys.assign(s_touched_keys.begin(), s_touched_keys.end());
  }
  if (keys.empty())
    throw ToolError("Nothing to save: no VR setting was changed with set_vr_setting.");

  std::vector<Config::Location> locations;
  for (const std::string& key : keys)
  {
    const VRSetting& setting = RequireSetting(key);
    if (*setting.note && std::string_view(setting.note).starts_with("global only"))
      throw ToolError(fmt::format("{} is a global setting, not a per-game one.", key));
    locations.push_back(setting.location);
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  const u16 revision = SConfig::GetInstance().GetRevision();
  bool saved = false;
  RunOnHostThread([&] {
    saved = ConfigLoaders::SaveCurrentVRSettingsForGame(
        game_id, revision == 0 ? std::nullopt : std::make_optional(revision), locations);
  });
  if (!saved)
    throw ToolError("The game has no ID; nothing was saved.");
  {
    std::lock_guard lock(s_touched_mutex);
    for (const Config::Location& location : locations)
      s_touched_keys.erase(location.key);
  }
  return ToolResult::Text(fmt::format("Saved {} setting(s) to User/GameSettingsVR/{}.ini "
                                      "[Graphics.VR].",
                                      locations.size(), game_id));
}

std::optional<Common::Log::LogLevel> ParseLevel(const std::string& name)
{
  if (name == "notice")
    return Common::Log::LogLevel::LNOTICE;
  if (name == "error")
    return Common::Log::LogLevel::LERROR;
  if (name == "warning")
    return Common::Log::LogLevel::LWARNING;
  if (name == "info")
    return Common::Log::LogLevel::LINFO;
  if (name == "debug")
    return Common::Log::LogLevel::LDEBUG;
  return std::nullopt;
}

ToolResult ReadLog(const ToolArgs& args)
{
  LogBuffer* buffer = Server::GetInstance().GetLogBuffer();
  if (!buffer)
    throw ToolError("The log is not available.");

  const u64 since = static_cast<u64>(std::max<s64>(0, args.GetInt("since_seq", 0)));
  const size_t max_lines =
      static_cast<size_t>(std::clamp<s64>(args.GetInt("max_lines", 100), 1, 500));
  const std::string pattern = args.GetString("regex");
  const std::optional<std::regex> regex = MakeToolRegex(pattern);

  const std::vector<LogBuffer::Line> lines = buffer->GetLines(since);
  std::vector<const LogBuffer::Line*> matches;
  for (const LogBuffer::Line& line : lines)
  {
    if (!regex || std::regex_search(line.text, *regex))
      matches.push_back(&line);
  }

  // The newest lines when there are too many.
  const size_t first = matches.size() > max_lines ? matches.size() - max_lines : 0;
  std::string text;
  for (size_t i = first; i < matches.size(); ++i)
    text += fmt::format("[{}] {}\n", matches[i]->sequence, matches[i]->text);
  text += fmt::format("({} matching line(s), {} shown; last sequence {}. Pass since_seq to read "
                      "only newer lines. Only enabled log types reach the log: see "
                      "set_log_type.)",
                      matches.size(), matches.size() - first, buffer->GetLastSequence());
  return ToolResult::Text(std::move(text));
}

ToolResult SetLogType(const ToolArgs& args)
{
  auto* log_manager = Common::Log::LogManager::GetInstance();
  if (!log_manager)
    throw ToolError("The log is not available.");

  std::string text;
  if (args.Has("type"))
  {
    const std::string type_name = args.RequireString("type");
    const auto types = log_manager->GetLogTypes();
    std::optional<size_t> index;
    for (size_t i = 0; i < types.size(); ++i)
    {
      if (Common::CaseInsensitiveEquals(types[i].m_short_name, type_name) ||
          Common::CaseInsensitiveEquals(types[i].m_full_name, type_name))
      {
        index = i;
      }
    }
    if (!index)
    {
      std::string names;
      for (const auto& type : types)
        names += fmt::format("{}{}", names.empty() ? "" : ", ", type.m_short_name);
      throw ToolError(fmt::format("Unknown log type \"{}\". Types: {}", type_name, names));
    }
    const bool enabled = args.GetBool("enabled", true);
    log_manager->SetEnable(static_cast<Common::Log::LogType>(*index), enabled);
    text += fmt::format("Log type {} {}. ", types[*index].m_short_name,
                        enabled ? "enabled" : "disabled");
  }
  if (args.Has("verbosity"))
  {
    const std::optional<Common::Log::LogLevel> level = ParseLevel(args.RequireString("verbosity"));
    if (!level)
      throw ToolError("verbosity is notice, error, warning, info or debug.");
    // CurrentRun: not saved to the user's logger settings.
    Config::SetCurrent(Common::Log::LOGGER_VERBOSITY, *level);
    text += fmt::format(
        "Verbosity {} for this session (effective at most {}).", args.GetString("verbosity"),
        Common::Log::MAX_EFFECTIVE_LOGLEVEL == Common::Log::LogLevel::LDEBUG ? "debug" : "info");
  }
  if (text.empty())
    throw ToolError("Give a type (with enabled) and/or a verbosity.");
  return ToolResult::Text(std::move(text));
}

ToolResult SetDrawDebugLogging(const ToolArgs& args)
{
  const bool enabled = args.GetBool("enabled", true);
  ShaderHunter::GetInstance().SetDebugLogging(enabled);
  return ToolResult::Text(fmt::format(
      "Debug Log Draws {}: every draw logs a VR_DRAW line (Video log type, info verbosity). "
      "capture_frame/list_draws give the same data per frame without flooding the log.",
      enabled ? "on" : "off"));
}
}  // namespace

void RegisterConfigTools(ToolRegistry& registry, Server& server)
{
  server.AddEmulationStoppedCallback([] {
    std::lock_guard lock(s_touched_mutex);
    s_touched_keys.clear();
  });

  registry.Add({.name = "list_vr_settings",
                .title = "List VR settings",
                .description =
                    "The VR settings a game profile can set ([Graphics.VR] of "
                    "GameSettingsVR/<GameID>.ini), with their current value, type, default and "
                    "where the value comes from.",
                .input_schema = Schema().String("filter", "Substring of the key name.").Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListVRSettings});

  registry.Add(
      {.name = "get_vr_setting",
       .title = "Get a VR setting",
       .description = "One VR setting (see list_vr_settings).",
       .input_schema =
           Schema().String("key", "INI key, e.g. UnitsPerMeter.").Required({"key"}).Build(),
       .read_only = true,
       .idempotent = true,
       .handler = GetVRSetting});

  registry.Add(
      {.name = "set_vr_setting",
       .title = "Try a VR setting",
       .description =
           "Sets a VR setting for this session only (not saved), e.g. UnitsPerMeter, "
           "ScreenDistance, RemoveCinematicBars. It applies from the next frame; when paused, one "
           "frame is stepped. Keep it with save_vr_settings, undo with reset_vr_settings.",
       .input_schema = Schema()
                           .String("key", "INI key, e.g. UnitsPerMeter.")
                           .String("value", "New value (True/False, number, enum integer).")
                           .Boolean("advance", "When paused, step one frame (default true).")
                           .Required({"key", "value"})
                           .Build(),
       .idempotent = true,
       .handler = SetVRSetting});

  registry.Add({.name = "reset_vr_settings",
                .title = "Undo VR settings",
                .description = "Undoes set_vr_setting for the given keys (default: all the agent "
                               "changed), back to the game's profile or the global value.",
                .input_schema = Schema().StringArray("keys", "INI keys (default all).").Build(),
                .idempotent = true,
                .handler = ResetVRSettings});

  registry.Add(
      {.name = "save_vr_settings",
       .title = "Save VR settings to the game profile",
       .description =
           "Writes the current values of the given VR settings (default: all the agent changed) "
           "to the [Graphics.VR] section of User/GameSettingsVR/<GameID>.ini. Ask the user to "
           "confirm in the headset before saving.",
       .input_schema = Schema().StringArray("keys", "INI keys (default all changed).").Build(),
       .handler = SaveVRSettings});

  registry.Add({.name = "read_log",
                .title = "Read the log",
                .description =
                    "The last lines of Dolphin's log (what the enabled log types let through), "
                    "newest last, optionally filtered by a case-insensitive regex.",
                .input_schema = Schema()
                                    .Integer("max_lines", "Default 100, at most 500.", 1, 500)
                                    .String("regex", "ECMAScript regex, e.g. \"OpenXR|VR_\".")
                                    .Integer("since_seq", "Only lines after this sequence number.")
                                    .Build(),
                .read_only = true,
                .handler = ReadLog});

  registry.Add({.name = "set_log_type",
                .title = "Enable a log type",
                .description =
                    "Enables or disables a log type (e.g. OpenXR, Video, PowerPC) and/or "
                    "sets the log verbosity for this session.",
                .input_schema = Schema()
                                    .String("type", "Log type short or full name.")
                                    .Boolean("enabled", "Default true.")
                                    .String("verbosity", "Log verbosity.",
                                            {"notice", "error", "warning", "info", "debug"})
                                    .Build(),
                .idempotent = true,
                .handler = SetLogType});

  registry.Add({.name = "set_draw_debug_logging",
                .title = "Debug Log Draws",
                .description = "The VR settings' Debug Log Draws toggle: one VR_DRAW log line per "
                               "draw (needs the Video log type at info verbosity). Prefer "
                               "capture_frame and list_draws.",
                .input_schema = Schema().Boolean("enabled", "Default true.").Build(),
                .idempotent = true,
                .handler = SetDrawDebugLogging});
}
}  // namespace AgentServer
