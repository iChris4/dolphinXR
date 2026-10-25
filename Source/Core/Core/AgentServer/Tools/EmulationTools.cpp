// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Emulation control: status, pause/resume/frame advance, savestates, speed, synthetic head look,
// and the game's lifecycle (stop, boot, restart, reset, quit).

#include <algorithm>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>

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
#include "Core/HW/ProcessorInterface.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
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

// The savestate file the emulation was last loaded from or saved to, and the emulated clock
// then. Loading that file again before the emulation runs leaves the clock where it was, which
// the refused-load check of LoadStateAndWait would misread.
struct StateFileMatch
{
  std::string path;
  std::filesystem::file_time_type write_time;
  u64 ticks = 0;
};
std::mutex s_state_file_match_mutex;
std::optional<StateFileMatch> s_state_file_match;

std::filesystem::file_time_type StateWriteTime(const std::string& path)
{
  std::error_code error;
  const auto time = std::filesystem::last_write_time(StringToPath(path), error);
  return error ? std::filesystem::file_time_type{} : time;
}

u64 GetEmulatedTicks()
{
  auto& system = GetSystem();
  const Core::CPUThreadGuard guard(system);
  return system.GetCoreTiming().GetTicks();
}

void RememberStateFile(const std::string& path, u64 ticks)
{
  std::lock_guard lock(s_state_file_match_mutex);
  s_state_file_match = StateFileMatch{path, StateWriteTime(path), ticks};
}

bool IsStateFileUnchangedAt(const std::string& path, u64 ticks)
{
  std::lock_guard lock(s_state_file_match_mutex);
  return s_state_file_match && s_state_file_match->path == path &&
         s_state_file_match->ticks == ticks &&
         s_state_file_match->write_time == StateWriteTime(path);
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
  const u64 ticks_before = GetEmulatedTicks();

  // LoadAs queues the load as a CPU-thread job; jobs run in order, so the next one runs after it.
  INFO_LOG_FMT(OPENXR, "AgentServer: loading {}", path);
  State::LoadAs(system, path);
  auto loaded = std::make_shared<std::promise<void>>();
  std::future<void> done = loaded->get_future();
  Core::RunOnCPUThread(system, [loaded] { loaded->set_value(); });
  if (!WaitUntil([&] { return done.wait_for(0s) == std::future_status::ready; }, timeout, 10ms))
    throw ToolError("The savestate did not finish loading in time.");

  const u64 ticks_after = GetEmulatedTicks();
  // The clock also stays put when the emulation already is at this file's state.
  const bool state_loaded =
      ticks_after != ticks_before || IsStateFileUnchangedAt(path, ticks_after);
  if (state_loaded)
    RememberStateFile(path, ticks_after);
  INFO_LOG_FMT(OPENXR, "AgentServer: load done ({})", state_loaded ? "loaded" : "refused");
  if (was_running)
    ResumeEmulation();
  if (!state_loaded)
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
using AgentPoseTarget = VR::OpenXRManager::AgentPoseTarget;

std::optional<AgentPoseTarget> PoseTarget(const std::string& name)
{
  if (name == "head")
    return AgentPoseTarget::Head;
  if (name == "left")
    return AgentPoseTarget::LeftHand;
  if (name == "right")
    return AgentPoseTarget::RightHand;
  return std::nullopt;
}

const char* PoseTargetName(AgentPoseTarget target)
{
  switch (target)
  {
  case AgentPoseTarget::Head:
    return "head";
  case AgentPoseTarget::LeftHand:
    return "left hand";
  case AgentPoseTarget::RightHand:
  default:
    return "right hand";
  }
}

std::string FormatAgentPose(AgentPoseTarget target, const VR::OpenXRManager::AgentPose& pose)
{
  return fmt::format("{}: yaw {:.1f}, pitch {:.1f}, roll {:.1f} degrees, at ({:.2f}, {:.2f}, "
                     "{:.2f}) m {}",
                     PoseTargetName(target), pose.yaw_deg, pose.pitch_deg, pose.roll_deg,
                     pose.position[0], pose.position[1], pose.position[2],
                     target == AgentPoseTarget::Head ? "from home" : "from the head");
}

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
      for (const AgentPoseTarget target :
           {AgentPoseTarget::Head, AgentPoseTarget::LeftHand, AgentPoseTarget::RightHand})
      {
        if (const auto pose = VR::g_openxr->GetAgentPose(target))
          text += "\nAgent " + FormatAgentPose(target, *pose) + " (set_vr_pose).";
      }
      for (size_t hand = 0; hand < 2; ++hand)
      {
        if (VR::g_openxr->GetAgentControls(hand))
        {
          text += fmt::format("\nAgent input on the {} controller (set_vr_controls).",
                              hand == 0 ? "left" : "right");
        }
      }
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

  Server& server = Server::GetInstance();
  if (server.IsTurboOverrideActive() || server.IsMuteOverrideActive())
  {
    text += fmt::format("\nAgent speed overrides: turbo {}, mute {}.",
                        server.IsTurboOverrideActive(), server.IsMuteOverrideActive());
  }
  if (const auto running = server.GetCallGate().GetRunning())
  {
    text += fmt::format("\nTool call running: {} for {:.1f} s{}{}.", running->tool,
                        running->seconds, running->runs_emulation ? ", letting emulation run" : "",
                        running->cancel_requested ? ", asked to stop" : "");
    picojson::object call;
    call["tool"] = JsonString(running->tool);
    call["seconds"] = JsonNumber(running->seconds);
    call["runs_emulation"] = JsonBool(running->runs_emulation);
    call["cancel_requested"] = JsonBool(running->cancel_requested);
    structured["running_call"] = picojson::value(std::move(call));
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
  const u32 advanced = AdvanceFrames(frames, timeout);
  const auto& counters = Server::GetInstance().GetFrameCounters();
  const auto samples = counters.GetSamplesAfter(before.xfb_copies);
  std::string text = fmt::format("Advanced {} frame(s); emulation is paused. ", advanced);
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
    RememberStateFile(path, GetEmulatedTicks());
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

ToolResult SetVRPose(const ToolArgs& args)
{
  RequireVRSession();
  const std::string name = args.GetString("target", "head");
  const std::optional<AgentPoseTarget> target = PoseTarget(name);
  if (!target)
    throw ToolError("target is \"head\", \"left\" or \"right\".");
  // A hand defaults to where a hand holds the arm cannon: 20 cm to the side, 25 cm below and
  // 35 cm in front of the head.
  const bool hand = *target != AgentPoseTarget::Head;
  const double side = *target == AgentPoseTarget::LeftHand ? -0.2 : 0.2;
  VR::OpenXRManager::AgentPose pose;
  pose.yaw_deg = static_cast<float>(std::clamp(args.GetNumber("yaw_deg", 0.0), -180.0, 180.0));
  pose.pitch_deg = static_cast<float>(std::clamp(args.GetNumber("pitch_deg", 0.0), -89.0, 89.0));
  pose.roll_deg = static_cast<float>(std::clamp(args.GetNumber("roll_deg", 0.0), -180.0, 180.0));
  pose.position = {
      static_cast<float>(std::clamp(args.GetNumber("x", hand ? side : 0.0), -3.0, 3.0)),
      static_cast<float>(std::clamp(args.GetNumber("y", hand ? -0.25 : 0.0), -3.0, 3.0)),
      static_cast<float>(std::clamp(args.GetNumber("z", hand ? -0.35 : 0.0), -3.0, 3.0))};
  VR::g_openxr->SetAgentPose(*target, pose);
  std::string text = "Set the " + FormatAgentPose(*target, pose) + ".";
  if (IsPaused() && args.GetBool("advance", true))
  {
    AdvanceFrames(1);
    text += " Advanced one frame so the next screenshot shows it.";
  }
  return ToolResult::Text(std::move(text));
}

ToolResult ClearVRPose(const ToolArgs& args)
{
  RequireVRSession();
  const std::string name = args.GetString("target", "all");
  if (name == "all")
  {
    for (const AgentPoseTarget target :
         {AgentPoseTarget::Head, AgentPoseTarget::LeftHand, AgentPoseTarget::RightHand})
    {
      VR::g_openxr->SetAgentPose(target, std::nullopt);
    }
  }
  else
  {
    const std::optional<AgentPoseTarget> target = PoseTarget(name);
    if (!target)
      throw ToolError("target is \"head\", \"left\", \"right\" or \"all\".");
    VR::g_openxr->SetAgentPose(*target, std::nullopt);
  }
  if (IsPaused() && args.GetBool("advance", true))
    AdvanceFrames(1);
  if (name == "all")
    return ToolResult::Text("The tracked head and controllers are back.");
  return ToolResult::Text(
      fmt::format("The tracked {} is back.", name == "head" ? "head" : name + " controller"));
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

// ---- Lifecycle: stop, boot, restart, reset, quit ----

constexpr auto STOP_TIMEOUT = 30s;
constexpr auto BOOT_TIMEOUT = 120s;
// Fields a booted game must run before boot_game returns (it is drawing by then).
constexpr u64 BOOT_SETTLE_FIELDS = 60;

Server::FrontendActions RequireFrontend()
{
  Server::FrontendActions actions = Server::GetInstance().GetFrontendActions();
  if (!actions.boot || !actions.stop || !actions.quit)
  {
    throw ToolError("This Dolphin frontend cannot start, stop or close games for the agent (the "
                    "desktop one can). Ask the user to do it.");
  }
  return actions;
}

void RequireNoNetPlay()
{
  if (NetPlay::IsNetPlayRunning())
    throw ToolError("Refused during NetPlay: the other players would lose the session.");
}

std::chrono::milliseconds TimeoutArg(const ToolArgs& args, std::chrono::milliseconds fallback)
{
  return std::chrono::milliseconds(args.GetInt("timeout_ms", fallback.count()));
}

void StopAndWait(const Server::FrontendActions& actions, bool force,
                 std::chrono::milliseconds timeout)
{
  auto& system = GetSystem();
  INFO_LOG_FMT(OPENXR, "AgentServer: stopping emulation{}", force ? " (forced)" : "");
  MarkCallRunsEmulation();
  actions.stop(force);
  if (WaitUntil([&] { return Core::IsUninitialized(system); }, timeout, 20ms))
    return;
  if (force)
  {
    throw ToolError("Emulation did not stop in time: Dolphin may be stuck. Ask the user to close "
                    "it.");
  }
  throw ToolError("Emulation did not stop in time. A Wii game stops when it handles the power "
                  "button, which a game that hangs never does: call stop_game with force=true.");
}

void BootAndWait(const Server::FrontendActions& actions, const std::string& path,
                 std::chrono::milliseconds timeout)
{
  if (!File::Exists(path))
    throw ToolError(fmt::format("{} does not exist.", path));
  auto& system = GetSystem();
  INFO_LOG_FMT(OPENXR, "AgentServer: booting {}", path);
  MarkCallRunsEmulation();
  actions.boot(path);
  if (!WaitUntil([&] { return Core::IsRunning(system); }, timeout, 20ms))
  {
    throw ToolError("The game did not start in time. Dolphin may be showing a dialog (an NKit "
                    "warning, an error): ask the user to look at its window.");
  }
  // Until it has run a second; a game that boots paused (the user's setting) is left paused.
  const FrameCounters& counters = Server::GetInstance().GetFrameCounters();
  const u64 start = counters.GetSnapshot().fields;
  WaitUntil(
      [&] {
        return !Core::IsRunning(system) || IsPaused() ||
               counters.GetSnapshot().fields >= start + BOOT_SETTLE_FIELDS;
      },
      timeout, 20ms);
}

// stop_game and quit_dolphin run alongside other calls, to recover from one that hangs: they ask
// it to stop first, so that it does not wait on a game that is going away.
std::string CancelRunningCall()
{
  CallGate& gate = Server::GetInstance().GetCallGate();
  const std::optional<CallGate::RunningCall> running = gate.GetRunning();
  if (!running || !gate.Cancel())
    return {};
  return fmt::format(" {} (running for {:.1f} s) was asked to stop first.", running->tool,
                     running->seconds);
}

ToolResult StopGame(const ToolArgs& args)
{
  if (Core::IsUninitialized(GetSystem()))
    return ToolResult::Text("No game is running.");
  RequireNoNetPlay();
  const Server::FrontendActions actions = RequireFrontend();
  const std::string cancelled = CancelRunningCall();
  StopAndWait(actions, args.GetBool("force", false), TimeoutArg(args, STOP_TIMEOUT));
  return ToolResult::Text("Emulation stopped." + cancelled +
                          " The agent server keeps running; boot_game or restart_game starts a "
                          "game again.");
}

ToolResult BootGame(const ToolArgs& args)
{
  if (!Core::IsUninitialized(GetSystem()))
  {
    throw ToolError(fmt::format("A game is running ({}): call stop_game first, or restart_game "
                                "to boot the same game again.",
                                SConfig::GetInstance().GetGameID()));
  }
  const Server::FrontendActions actions = RequireFrontend();
  std::string path = args.GetString("path");
  if (path.empty())
    path = Server::GetInstance().GetLastBootPath();
  if (path.empty())
  {
    throw ToolError("No game was booted from a file since Dolphin started: give the \"path\" of "
                    "the game's file.");
  }
  BootAndWait(actions, path, TimeoutArg(args, BOOT_TIMEOUT));
  return ToolResult::Text(fmt::format("Booted {}. {}", path, DescribeEmulationState()));
}

ToolResult RestartGame(const ToolArgs& args)
{
  if (Core::IsUninitialized(GetSystem()))
    throw ToolError("No game is running: boot_game starts the file booted last.");
  RequireNoNetPlay();
  const Server::FrontendActions actions = RequireFrontend();
  const std::string path = Server::GetInstance().GetLastBootPath();
  if (path.empty())
  {
    throw ToolError("This game was not booted from a file (a NAND title or the Wii Menu): stop it "
                    "with stop_game and ask the user to start it.");
  }
  const auto timeout = TimeoutArg(args, BOOT_TIMEOUT);
  StopAndWait(actions, args.GetBool("force", false),
              std::min<std::chrono::milliseconds>(timeout, STOP_TIMEOUT));
  BootAndWait(actions, path, timeout);
  return ToolResult::Text(fmt::format("Restarted {}. {}", path, DescribeEmulationState()));
}

ToolResult ResetGame(const ToolArgs&)
{
  RequireGame();
  RequireNoNetPlay();
  auto& system = GetSystem();
  auto& movie = system.GetMovie();
  if (movie.IsRecordingInput())
    movie.SetReset(true);
  system.GetProcessorInterface().ResetButton_Tap();
  return ToolResult::Text("Pressed the console's Reset button: the game resets as it would on the "
                          "console (it must be running, not paused, to see it).");
}

ToolResult SimulateGPUDeviceLost(const ToolArgs&)
{
  RequireGame();
  bool removed = false;
  RunOnVideoThread([&removed] { removed = g_gfx && g_gfx->RemoveDeviceForTesting(); });
  if (!removed)
    throw ToolError("This video backend cannot lose its device on request (D3D12 can).");
  return ToolResult::Text("The GPU device is lost, as after a driver reset: Dolphin should stop "
                          "emulation and say so. Check get_status and the log.");
}

ToolResult QuitDolphin(const ToolArgs& args)
{
  RequireNoNetPlay();
  const Server::FrontendActions actions = RequireFrontend();
  const std::string cancelled = CancelRunningCall();
  actions.quit(args.GetBool("force", false));
  return ToolResult::Text("Dolphin is closing: emulation stops first, and the agent server stops "
                          "with Dolphin." +
                          cancelled);
}

ToolResult CancelTool(const ToolArgs&)
{
  CallGate& gate = Server::GetInstance().GetCallGate();
  const std::optional<CallGate::RunningCall> running = gate.GetRunning();
  if (!running || !gate.Cancel())
    return ToolResult::Text("No tool call is running.");
  return ToolResult::Text(fmt::format(
      "Asked {} (running for {:.1f} s) to stop. A call that lets emulation run (input holds, "
      "traces, frame steps, captures, experiments) ends at its next check, within milliseconds, "
      "and returns what it has with a note; a quick call finishes anyway. If get_status still "
      "shows it a few seconds from now, it is stuck: stop_game with force=true, or quit_dolphin.",
      running->tool, running->seconds));
}
}  // namespace

void RegisterEmulationTools(ToolRegistry& registry, Server&)
{
  registry.Add({.name = "get_status",
                .title = "Emulation status",
                .description =
                    "Emulation state, running game, VR session (stereo or flat screen), speed, "
                    "backend, frame counters, agent look offset, speed overrides, and the tool "
                    "call in progress. Works without a game, and while another call runs.",
                .input_schema = Schema().Build(),
                .read_only = true,
                .idempotent = true,
                .anytime = true,
                .handler = GetStatus});

  registry.Add(
      {.name = "cancel_tool",
       .title = "Cancel the running call",
       .description =
           "Asks the tool call in progress to stop: an input hold, a trace, frame steps, a "
           "capture or an experiment ends at its next check and returns what it has, with a "
           "note. For a call that takes longer than planned, or was sent by mistake. Works while "
           "another call runs (that is its point); get_status shows the running call.",
       .input_schema = Schema().Build(),
       .idempotent = true,
       .anytime = true,
       .handler = CancelTool});

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
           "Turns the head as if the user turned it (VR stereo session only), to look at what "
           "is around or behind without wearing the headset: culling tests, edges of the view, "
           "and what stays fixed in the room. Positive yaw turns right and positive pitch looks "
           "up. The game view and the virtual screen (with its Screen and Screen Pane draws) "
           "turn as with a real head: world-fixed elements move in the eye images, head-locked "
           "ones stay where they are. The head-cone CPU cull follows it. Stays until "
           "clear_look_offset or the server stops.",
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

  registry.Add(
      {.name = "set_vr_pose",
       .title = "Place the head or a hand",
       .description =
           "Replaces the tracked head or a controller with a pose of yours, as if the user held "
           "it there, to test without the headset: the game is rendered from the head, VR "
           "patches read the head and the hands, and a hand drives the Wii pointer, the "
           "motion and the Controller Anchor (still, and connected even when the controller "
           "is off). Unlike set_look_offset, the frame is submitted with the head: in the "
           "headset the view stays where you look. Angles in degrees (positive yaw turns "
           "right, positive pitch looks up). The head's position is in meters from the home "
           "position (x right, y up, z back; default 0, 0, 0: the game camera); a hand's is "
           "in the head's axes (default 0.2 to its side, -0.25, -0.35) and follows the head. "
           "Stays until clear_vr_pose or the server stops; tell the user while it is set.",
       .input_schema =
           Schema()
               .String("target", "head (default), left or right.", {"head", "left", "right"})
               .Number("yaw_deg", "Degrees, -180 to 180 (default 0).")
               .Number("pitch_deg", "Degrees, -89 to 89 (default 0).")
               .Number("roll_deg", "Degrees, -180 to 180 (default 0).")
               .Number("x", "Meters to the right.")
               .Number("y", "Meters up.")
               .Number("z", "Meters back (negative is forward).")
               .Boolean("advance", "When paused, step one frame so it is drawn (default true).")
               .Build(),
       .handler = SetVRPose});

  registry.Add(
      {.name = "clear_vr_pose",
       .title = "Give the head and hands back",
       .description = "Removes poses set by set_vr_pose: the tracked head and controllers are "
                      "used again.",
       .input_schema = Schema()
                           .String("target", "all (default), head, left or right.",
                                   {"all", "head", "left", "right"})
                           .Boolean("advance", "When paused, step one frame (default true).")
                           .Build(),
       .idempotent = true,
       .handler = ClearVRPose});

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

  registry.Add(
      {.name = "stop_game",
       .title = "Stop emulation",
       .description =
           "Stops the game as the Stop button does, without the confirmation prompt, and waits "
           "until emulation is shut down. The server keeps running. Use it to recover a game "
           "that froze; force=true skips the Wii's power button shutdown, which a frozen game "
           "never finishes. Works while another call runs, which it cancels first: the way out "
           "of a call that hangs. Unsaved game progress is lost: make a savestate first when it "
           "matters.",
       .input_schema = Schema()
                           .Boolean("force", "Stop at once (default false).")
                           .Integer("timeout_ms", "Default 30000.", 1000, 120000)
                           .Build(),
       .destructive = true,
       .idempotent = true,
       .anytime = true,
       .handler = StopGame});

  registry.Add(
      {.name = "boot_game",
       .title = "Start a game",
       .description =
           "Boots a game file (disc image, executable or WAD) as the game list does, when no "
           "game runs, and waits until it runs. Without a path, boots the file booted last. The "
           "game starts from the beginning: load_state afterwards to go back to a savestate.",
       .input_schema = Schema()
                           .String("path", "The game's file. Default: the file booted last.")
                           .Integer("timeout_ms", "Default 120000.", 5000, 600000)
                           .Build(),
       .handler = BootGame});

  registry.Add(
      {.name = "restart_game",
       .title = "Restart the game",
       .description =
           "Stops the running game (as stop_game) and boots the same file again, then waits "
           "until it runs: a clean start after a hang, or to apply settings read at boot. Load a "
           "savestate afterwards to go back where you were.",
       .input_schema = Schema()
                           .Boolean("force", "Stop at once (default false).")
                           .Integer("timeout_ms", "Default 120000.", 5000, 600000)
                           .Build(),
       .destructive = true,
       .handler = RestartGame});

  registry.Add({.name = "reset_game",
                .title = "Press Reset",
                .description = "Presses the console's Reset button: the game resets as it does "
                               "on the console (often back to its title screen).",
                .input_schema = Schema().Build(),
                .destructive = true,
                .handler = ResetGame});

  registry.Add({.name = "simulate_gpu_device_lost",
                .title = "Lose the GPU device",
                .description =
                    "Loses the GPU device as a graphics driver reset does (D3D12 only), to test "
                    "that Dolphin stops emulation cleanly and tells the user instead of crashing. "
                    "Emulation cannot go on afterwards.",
                .input_schema = Schema().Build(),
                .destructive = true,
                .handler = SimulateGPUDeviceLost});

  registry.Add(
      {.name = "quit_dolphin",
       .title = "Close Dolphin",
       .description = "Stops emulation without the confirmation prompt and closes Dolphin, and "
                      "the agent server with it. Only when the user asked for it, or to end a "
                      "session you started yourself. Works while another call runs, which it "
                      "cancels first.",
       .input_schema = Schema().Boolean("force", "Stop the game at once (default false).").Build(),
       .destructive = true,
       .anytime = true,
       .handler = QuitDolphin});
}
}  // namespace AgentServer
