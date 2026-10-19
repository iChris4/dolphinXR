// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Emulation control: status, pause/resume/frame advance, savestates, speed, synthetic head look.

#include <algorithm>
#include <filesystem>
#include <future>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/AchievementManager.h"
#include "Core/AgentServer/AgentServer.h"
#include "Core/AgentServer/ToolHelpers.h"
#include "Core/AgentServer/Tools/Tools.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VR/OpenXRManager.h"
#include "VideoCommon/VideoConfig.h"

namespace AgentServer
{
namespace
{
std::string AgentStateDirectory()
{
  return File::GetUserPath(D_STATESAVES_IDX) + "agent/";
}

std::string SanitizeLabel(const std::string& label)
{
  std::string result;
  for (const char c : label)
  {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_')
    {
      result += c;
    }
  }
  if (result.empty() || result.size() > 40)
    throw ToolError("A savestate label is 1 to 40 letters, digits, '-' or '_'.");
  return result;
}
}  // namespace

std::string AgentStatePath(const std::string& label)
{
  return fmt::format("{}{}-{}.sav", AgentStateDirectory(), SConfig::GetInstance().GetGameID(),
                     SanitizeLabel(label));
}

void RequireSavestatesAllowed()
{
  RequireGame();
  if (NetPlay::IsNetPlayRunning())
    throw ToolError("Savestates are disabled during NetPlay.");
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    throw ToolError("Savestates are disabled in RetroAchievements hardcore mode.");
}

void LoadStateAndWait(const std::string& path, std::chrono::milliseconds timeout)
{
  RequireSavestatesAllowed();
  if (!File::Exists(path))
    throw ToolError(fmt::format("The savestate {} does not exist.", path));
  auto& system = GetSystem();

  // A refused load (another Dolphin version, a corrupt file) only shows an on-screen message.
  // Detect it from the emulated clock: paused, it moves only when a state is loaded.
  const bool was_running = !IsPaused();
  PauseEmulation();
  const auto ticks = [&system] {
    const Core::CPUThreadGuard guard(system);
    return system.GetCoreTiming().GetTicks();
  };
  const u64 ticks_before = ticks();

  // LoadAs queues the load as a CPU-thread job; jobs run in order, so the next one runs after it.
  INFO_LOG_FMT(OPENXR, "AgentServer: loading {}", path);
  State::LoadAs(system, path);
  auto loaded = std::make_shared<std::promise<void>>();
  std::future<void> done = loaded->get_future();
  Core::RunOnCPUThread(system, [loaded] { loaded->set_value(); });
  if (!WaitUntil([&] { return done.wait_for(0s) == std::future_status::ready; }, timeout, 10ms))
    throw ToolError("The savestate did not finish loading in time.");

  const bool changed = ticks() != ticks_before;
  INFO_LOG_FMT(OPENXR, "AgentServer: load done ({})", changed ? "loaded" : "refused");
  if (was_running)
    ResumeEmulation();
  if (!changed)
  {
    throw ToolError("Dolphin did not load the savestate: it was probably made by another Dolphin "
                    "version or for another revision of the game (the reason is shown on screen). "
                    "Make a new one with save_state.");
  }
}

// Applies the turbo/mute overrides at once. On desktop, HotkeyScheduler keeps applying them (it
// owns these runtime overrides); Android has no such loop, so this is what makes them stick.
void SetAgentSpeed(bool turbo, bool mute)
{
  auto& system = GetSystem();
  Server::GetInstance().SetSpeedOverrides(turbo, mute);
  Core::SetIsThrottlerTempDisabled(turbo);
  if (mute && !Config::Get(Config::MAIN_AUDIO_MUTED))
  {
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);
    AudioCommon::UpdateSoundStream(system);
  }
  else if (!mute && Config::Get(Config::MAIN_AUDIO_MUTED) &&
           Config::GetActiveLayerForConfig(Config::MAIN_AUDIO_MUTED) ==
               Config::LayerType::CurrentRun)
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
    AudioCommon::UpdateSoundStream(system);
  }
}

namespace
{
ToolResult GetStatus(const ToolArgs&)
{
  auto& system = GetSystem();
  std::string text = DescribeEmulationState();
  picojson::object structured;
  structured["running"] = JsonBool(Core::IsRunning(system));
  structured["paused"] = JsonBool(IsPaused());

  if (Core::IsRunning(system))
  {
    const auto& perf = system.GetPerfMetrics();
    const bool vr = VR::g_openxr && g_ActiveConfig.VRSessionActive();
    const bool stereo = vr && g_ActiveConfig.stereo_mode == StereoMode::OpenXR;
    text += fmt::format("\nSpeed: {:.1f} FPS, {:.1f} VPS ({:.0f}%).", perf.GetFPS(), perf.GetVPS(),
                        perf.GetSpeed() * 100.0);
    text += fmt::format("\nVR: {}. Backend {}, internal resolution {}x, {} core.",
                        stereo ? "stereo OpenXR session" :
                        vr     ? "OpenXR session showing a flat screen" :
                                 "no OpenXR session",
                        Config::Get(Config::MAIN_GFX_BACKEND), g_ActiveConfig.iEFBScale,
                        system.IsDualCoreMode() ? "dual" : "single");
    if (vr)
    {
      const auto look = VR::g_openxr->GetAgentLookOffset();
      text += fmt::format("\nAgent look offset: yaw {:.1f}, pitch {:.1f}, roll {:.1f} degrees.",
                          look[0], look[1], look[2]);
    }
    text += fmt::format("\nCheats {}, debugging mode {}.",
                        Config::AreCheatsEnabled() ? "enabled" : "disabled",
                        Config::IsDebuggingEnabled() ? "on" : "off");
    structured["game_id"] = JsonString(SConfig::GetInstance().GetGameID());
    structured["vr_session"] = JsonBool(vr);
    structured["stereo"] = JsonBool(stereo);
    structured["fps"] = JsonNumber(perf.GetFPS());
    structured["vps"] = JsonNumber(perf.GetVPS());
  }

  const Server& server = Server::GetInstance();
  if (server.IsTurboOverrideActive() || server.IsMuteOverrideActive())
  {
    text += fmt::format("\nAgent speed overrides: turbo {}, mute {}.",
                        server.IsTurboOverrideActive(), server.IsMuteOverrideActive());
  }
  return ToolResult::Text(std::move(text)).SetStructured(std::move(structured));
}

ToolResult Pause(const ToolArgs&)
{
  PauseEmulation();
  return ToolResult::Text(DescribeEmulationState());
}

ToolResult Resume(const ToolArgs&)
{
  ResumeEmulation();
  return ToolResult::Text(DescribeEmulationState());
}

ToolResult AdvanceFramesTool(const ToolArgs& args)
{
  const u32 frames = static_cast<u32>(std::clamp<s64>(args.GetInt("frames", 1), 1, 600));
  const auto timeout = std::chrono::milliseconds(
      std::clamp<s64>(args.GetInt("timeout_ms_per_frame", 3000), 100, 30000));
  const auto before = Server::GetInstance().GetFrameCounters().GetSnapshot();
  AdvanceFrames(frames, timeout);
  const auto& counters = Server::GetInstance().GetFrameCounters();
  const auto samples = counters.GetSamplesAfter(before.xfb_copies);
  std::string text = fmt::format("Advanced {} frame(s); emulation is paused. ", frames);
  if (!samples.empty())
  {
    text += fmt::format("Last XFB copy: {} draw calls, {} primitives. ", samples.back().draw_calls,
                        samples.back().prims);
  }
  return ToolResult::Text(text + DescribeEmulationState());
}

ToolResult SaveStateTool(const ToolArgs& args)
{
  RequireSavestatesAllowed();
  auto& system = GetSystem();
  State::SaveOptions options;
  options.wait_for_completion = true;
  if (args.Has("label"))
  {
    const std::string path = AgentStatePath(args.RequireString("label"));
    File::CreateFullPath(path);
    State::SaveAs(system, path, options);
    if (!File::Exists(path))
      throw ToolError("The savestate was not written (see read_log).");
    return ToolResult::Text(
        fmt::format("Saved the agent savestate \"{}\" ({}).", args.GetString("label"), path));
  }
  if (!args.Has("slot"))
    throw ToolError("Give a \"label\" (agent savestate, recommended) or a \"slot\" (1-10).");
  const s64 slot = args.GetInt("slot", 0);
  if (slot < 1 || slot > static_cast<s64>(State::NUM_STATES))
    throw ToolError("slot must be 1 to 10.");
  if (!args.GetBool("overwrite_user_slot", false))
  {
    throw ToolError(fmt::format("Slot {} belongs to the user. Use a label for agent savestates, "
                                "or pass overwrite_user_slot=true if the user asked for it.",
                                slot));
  }
  State::Save(system, static_cast<u32>(slot), options);
  return ToolResult::Text(fmt::format("Saved slot {}.", slot));
}

ToolResult LoadStateTool(const ToolArgs& args)
{
  RequireSavestatesAllowed();
  std::string path;
  std::string what;
  if (args.Has("label"))
  {
    path = AgentStatePath(args.RequireString("label"));
    what = fmt::format("agent savestate \"{}\"", args.GetString("label"));
  }
  else if (args.Has("slot"))
  {
    const s64 slot = args.GetInt("slot", 0);
    if (slot < 1 || slot > static_cast<s64>(State::NUM_STATES))
      throw ToolError("slot must be 1 to 10.");
    path = fmt::format("{}{}.s{:02d}", File::GetUserPath(D_STATESAVES_IDX),
                       SConfig::GetInstance().GetGameID(), slot);
    what = fmt::format("slot {}", slot);
  }
  else
  {
    throw ToolError("Give a \"label\" or a \"slot\".");
  }
  if (!File::Exists(path))
    throw ToolError(fmt::format("There is no {} for this game ({}).", what, path));

  LoadStateAndWait(path, std::chrono::milliseconds(
                             std::clamp<s64>(args.GetInt("timeout_ms", 30000), 1000, 120000)));
  return ToolResult::Text(fmt::format("Loaded the {}. {}", what, DescribeEmulationState()));
}

ToolResult ListStates(const ToolArgs&)
{
  RequireGame();
  std::string text = "User slots (load only, unless the user asks):\n";
  for (u32 slot = 1; slot <= State::NUM_STATES; ++slot)
    text += fmt::format("  {}: {}\n", slot, State::GetInfoStringOfSlot(slot, false));

  text += "Agent savestates (labels):\n";
  const std::string prefix = SConfig::GetInstance().GetGameID() + "-";
  bool any = false;
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(StringToPath(AgentStateDirectory()), error))
  {
    const std::string name = PathToString(entry.path().filename());
    if (!name.starts_with(prefix) || !name.ends_with(".sav"))
      continue;
    text += fmt::format("  {}\n", name.substr(prefix.size(), name.size() - prefix.size() - 4));
    any = true;
  }
  if (!any)
    text += "  (none)\n";
  return ToolResult::Text(std::move(text));
}

ToolResult SetLookOffset(const ToolArgs& args)
{
  RequireStereoSession();
  const float yaw = static_cast<float>(std::clamp(args.GetNumber("yaw_deg", 0.0), -180.0, 180.0));
  const float pitch = static_cast<float>(std::clamp(args.GetNumber("pitch_deg", 0.0), -89.0, 89.0));
  const float roll = static_cast<float>(std::clamp(args.GetNumber("roll_deg", 0.0), -180.0, 180.0));
  VR::g_openxr->SetAgentLookOffset(yaw, pitch, roll);
  std::string text = fmt::format(
      "Look offset set to yaw {:.1f}, pitch {:.1f}, roll {:.1f} degrees.", yaw, pitch, roll);
  if (IsPaused() && args.GetBool("advance", true))
  {
    AdvanceFrames(1);
    text += " Advanced one frame so the next screenshot shows it.";
  }
  return ToolResult::Text(std::move(text));
}

ToolResult ClearLookOffset(const ToolArgs& args)
{
  RequireVRSession();
  VR::g_openxr->SetAgentLookOffset(0.0f, 0.0f, 0.0f);
  if (IsPaused() && args.GetBool("advance", true))
    AdvanceFrames(1);
  return ToolResult::Text("Look offset cleared.");
}

ToolResult SetSpeed(const ToolArgs& args)
{
  RequireGame();
  const bool turbo = args.GetBool("turbo", false);
  const bool mute = args.GetBool("mute", turbo);
  SetAgentSpeed(turbo, mute);
  return ToolResult::Text(fmt::format("Turbo {}, mute {}. Call set_speed with turbo=false when the "
                                      "experiment is over.",
                                      turbo ? "on" : "off", mute ? "on" : "off"));
}
}  // namespace

void RegisterEmulationTools(ToolRegistry& registry, Server&)
{
  registry.Add({.name = "get_status",
                .title = "Emulation status",
                .description =
                    "Emulation state, running game, VR session (stereo or flat screen), speed, "
                    "backend, frame counters, agent look offset and speed overrides. Works "
                    "without a game.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = GetStatus});

  registry.Add({.name = "pause",
                .title = "Pause emulation",
                .description = "Pauses emulation. In a VR session the headset keeps showing the "
                               "paused frame and screenshots still work.",
                .input_schema = Schema().Build(),
                .idempotent = true,
                .handler = Pause});

  registry.Add({.name = "resume",
                .title = "Resume emulation",
                .description = "Resumes emulation.",
                .input_schema = Schema().Build(),
                .idempotent = true,
                .handler = Resume});

  registry.Add({.name = "advance_frames",
                .title = "Advance frames",
                .description = "Pauses (if needed), then steps the given number of presented "
                               "frames and stays paused. Use after changing a setting or an "
                               "override while paused, so the next frame is drawn with it.",
                .input_schema =
                    Schema()
                        .Integer("frames", "Frames to step (default 1).", 1, 600)
                        .Integer("timeout_ms_per_frame",
                                 "Give up when one frame takes longer (default 3000).", 100, 30000)
                        .Build(),
                .handler = AdvanceFramesTool});

  registry.Add(
      {.name = "save_state",
       .title = "Save a savestate",
       .description =
           "Saves an agent savestate under a label (User/StateSaves/agent/<GameID>-<label>.sav), "
           "the reference point for experiments. User slots 1-10 are only written with "
           "overwrite_user_slot=true, when the user asked for it.",
       .input_schema = Schema()
                           .String("label", "Agent savestate name: letters, digits, '-' or '_'.")
                           .Integer("slot", "User slot 1-10 instead of a label.", 1, 10)
                           .Boolean("overwrite_user_slot", "Required to write a user slot.")
                           .Build(),
       .handler = SaveStateTool});

  registry.Add({.name = "load_state",
                .title = "Load a savestate",
                .description = "Loads an agent savestate (label) or a user slot (1-10) and waits "
                               "until it is loaded. The pause state is kept.",
                .input_schema = Schema()
                                    .String("label", "Agent savestate name.")
                                    .Integer("slot", "User slot 1-10.", 1, 10)
                                    .Integer("timeout_ms", "Default 30000.", 1000, 120000)
                                    .Build(),
                .handler = LoadStateTool});

  registry.Add({.name = "list_states",
                .title = "List savestates",
                .description = "The user's savestate slots for this game and the agent savestates.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .handler = ListStates});

  registry.Add(
      {.name = "set_look_offset",
       .title = "Turn the virtual head",
       .description =
           "Rotates the game camera as if the user turned their head (VR stereo session only), "
           "to look at what is around or behind without wearing the headset: culling tests, "
           "edges of the view. Positive yaw turns right and positive pitch looks up. It does "
           "not move the virtual screen or head-locked "
           "elements, and the head-cone CPU cull follows it. Stays until clear_look_offset or "
           "the server stops.",
       .input_schema =
           Schema()
               .Number("yaw_deg", "Degrees, -180 to 180.")
               .Number("pitch_deg", "Degrees, -89 to 89.")
               .Number("roll_deg", "Degrees, -180 to 180.")
               .Boolean("advance", "When paused, step one frame so it is drawn (default true).")
               .Build(),
       .handler = SetLookOffset});

  registry.Add(
      {.name = "clear_look_offset",
       .title = "Reset the virtual head",
       .description = "Removes the look offset set by set_look_offset.",
       .input_schema =
           Schema().Boolean("advance", "When paused, step one frame (default true).").Build(),
       .idempotent = true,
       .handler = ClearLookOffset});

  registry.Add({.name = "set_speed",
                .title = "Turbo and mute",
                .description = "Runs emulation unthrottled (turbo) and/or muted, to make "
                               "experiments faster. Turn it off afterwards.",
                .input_schema = Schema()
                                    .Boolean("turbo", "Unthrottled emulation.")
                                    .Boolean("mute", "Mute audio (default: same as turbo).")
                                    .Build(),
                .idempotent = true,
                .handler = SetSpeed});
}
}  // namespace AgentServer
