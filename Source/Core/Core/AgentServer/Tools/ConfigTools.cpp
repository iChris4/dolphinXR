// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// VR settings (the [VR] keys a game profile's [Graphics.VR] section can set) and the log.

#include <algorithm>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <type_traits>

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
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/VideoConfig.h"

namespace AgentServer
{
namespace
{
struct VRSetting
{
  Config::Location location;
  const char* type;
  std::string default_value;
  // Parses a value and returns it in the INI's canonical form, or nullopt when it is invalid.
  std::optional<std::string> (*normalize)(const std::string&);
  const char* note;
};

template <typename T>
VRSetting Entry(const Config::Info<T>& info, const char* note = "")
{
  const char* type = "int";
  if constexpr (std::is_same_v<T, bool>)
    type = "bool";
  else if constexpr (std::is_floating_point_v<T>)
    type = "float";
  else if constexpr (std::is_enum_v<T>)
    type = "enum (integer)";
  return {info.GetLocation(), type, ValueToString(info.GetDefaultValue()),
          [](const std::string& text) -> std::optional<std::string> {
            const std::optional<T> value = Config::detail::TryParse<T>(text);
            if (!value)
              return std::nullopt;
            return ValueToString(*value);
          },
          note};
}

// The keys a GameSettingsVR [Graphics.VR] section can set (the list ConfigManager.cpp's
// ApplyGameVRConfigOverrides applies), plus a few global diagnostics.
const std::vector<VRSetting>& GetVRSettings()
{
  static const std::vector<VRSetting> settings = {
      Entry(Config::GFX_VR_ENABLE_OPENXR, "restart the game"),
      Entry(Config::GFX_VR_FLAT_SCREEN,
            "flat screen instead of stereo; switches live (OpenGL: restart the game)"),
      Entry(Config::GFX_VR_UNITS_PER_METER, "world scale: game units per meter"),
      Entry(Config::GFX_VR_ENABLE_LEAN_BACK_ANGLE),
      Entry(Config::GFX_VR_LEAN_BACK_ANGLE, "degrees"),
      Entry(Config::GFX_VR_ENABLE_CAMERA_FORWARD),
      Entry(Config::GFX_VR_CAMERA_FORWARD, "meters"),
      Entry(Config::GFX_VR_ENABLE_CAMERA_HEIGHT),
      Entry(Config::GFX_VR_CAMERA_HEIGHT, "meters"),
      Entry(Config::GFX_VR_ENABLE_CAMERA_ANCHOR),
      Entry(Config::GFX_VR_CAMERA_ANCHOR_SMOOTHING),
      Entry(Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR),
      Entry(Config::GFX_VR_VIRTUAL_SCREEN, "2D content on a world-locked screen"),
      Entry(Config::GFX_VR_SCREEN_DISTANCE, "meters"),
      Entry(Config::GFX_VR_SCREEN_SIZE, "meters"),
      Entry(Config::GFX_VR_HEAD_LOCKED_CURVATURE),
      Entry(Config::GFX_VR_DONT_CLEAR_SCREEN),
      Entry(Config::GFX_VR_LOAD_CUSTOM_SHADERS),
      Entry(Config::GFX_VR_HEAD_CPU_CULL, "head-cone CPU culling of draws"),
      Entry(Config::GFX_VR_NONBLOCKING_EFB_ACCESS),
      Entry(Config::GFX_VR_ASYNC_EFB_COPIES),
      Entry(Config::GFX_VR_NONBLOCKING_PERF_QUERIES),
      Entry(Config::GFX_VR_MIRROR_VIEW, "0 both eyes, 1 left, 2 right, 3 none"),
      Entry(Config::GFX_VR_REFERENCE_SPACE_MODE, "0 local, 1 stage height, 2 stage"),
      Entry(Config::GFX_VR_TRACKING_MODE, "0 6DoF, 1 3DoF, 2 none"),
      Entry(Config::GFX_VR_USE_OPENXR_PLAY_SPACE_CENTER),
      Entry(Config::GFX_VR_EAGER_HEARTBEAT),
      Entry(Config::GFX_VR_FORCED_VBI_FREQUENCY, "-1 auto, 0 off, 72/90/120; restart the game"),
      Entry(Config::GFX_VR_AUTO_VBI_FROM_HMD, "restart the game"),
      Entry(Config::GFX_VR_EXACT_SCREEN_DEPTH),
      Entry(Config::GFX_VR_AUTO_NATIVE_EFB_EFFECTS,
            "ortho draws sampling EFB copies (bloom, blur) go fullscreen"),
      Entry(Config::GFX_VR_HUD_THICKNESS),
      Entry(Config::GFX_VR_REMOVE_BARS, "remove cinematic bars"),
      Entry(Config::GFX_VR_FRAME_SIZE_FROM_XFB),
      Entry(Config::GFX_VR_PANES_ON_SCREEN, "small 3D viewports on the virtual screen"),
      Entry(Config::GFX_VR_DETECT_RENDER_TARGETS),
      Entry(Config::GFX_VR_ORTHO_SCISSOR_FIX),
      Entry(Config::GFX_VR_DETECT_SKYBOX),
      Entry(Config::GFX_VR_SKIP_BACKGROUND_FILL),
      Entry(Config::GFX_VR_METROID_THERMAL_VISOR_FIX),
      Entry(Config::GFX_VR_METROID_D3D_THERMAL_PALETTE_FIX),
      Entry(Config::GFX_VR_CLEAR_EFB_COPIES, "EFB copy width threshold, 0 = off"),
      Entry(Config::GFX_VR_GAMMA),
      Entry(Config::GFX_VR_USE_VULKAN_MULTIVIEW, "restart the game"),
      Entry(Config::GFX_VR_ANDROID_DIRECT_TO_HMD, "restart the game"),
      Entry(Config::GFX_VR_RESOLUTION_SCALE, "eye resolution scale"),
      Entry(Config::GFX_VR_FOVEATION_LEVEL, "0-3; read at session start"),
      Entry(Config::GFX_VR_FOVEATION_DYNAMIC, "read at session start"),
      Entry(Config::GFX_VR_EFB_FOVEATION),
      Entry(Config::GFX_VR_PASSTHROUGH),
      Entry(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_BG),
      Entry(Config::GFX_VR_PASSTHROUGH_REMOVE_BLACK_CLEARS),
      Entry(Config::GFX_VR_PASSTHROUGH_SCENE_OPACITY),
      Entry(Config::GFX_VR_PASSTHROUGH_COVERAGE_MODE, "0 exact, 1 fast"),
      Entry(Config::GFX_VR_PERF_COUNTERS, "global only: VKPERF per-draw timing"),
      Entry(Config::GFX_VR_EFB_COPY_DIAGNOSTICS, "global only: EFBCopyRAM log lines"),
  };
  return settings;
}

const VRSetting* FindSetting(const std::string& key)
{
  for (const VRSetting& setting : GetVRSettings())
  {
    if (Common::CaseInsensitiveEquals(setting.location.key, key))
      return &setting;
  }
  return nullptr;
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
