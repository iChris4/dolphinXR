// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/AgentServer/CallGate.h"
#include "Core/AgentServer/FrameCounters.h"
#include "Core/AgentServer/HttpServer.h"
#include "Core/AgentServer/OverrideSession.h"
#include "Core/AgentServer/ToolRegistry.h"

namespace Core
{
enum class State;
}

namespace AgentServer
{
class LogBuffer;
class McpServer;

// The Agent Server: an MCP endpoint (Streamable HTTP on 127.0.0.1, see McpServer) through which an
// AI agent such as Claude Code helps the user make VR profiles and find codes. It is off by
// default ([VR] AgentServer in GFX.ini). The frontend calls ApplyConfig() once at startup and
// Shutdown() before exit; the server follows the settings by itself afterwards.
//
// Threads: each request is handled on a thread of its own, and the CallGate lets tool calls in
// one at a time (the `anytime` tools excepted). Tool handlers reach the CPU thread with
// Core::CPUThreadGuard / Core::RunOnCPUThread and the video thread with VR::AgentVideoJobs, and
// only ever wait with timeouts (see ToolHelpers.h).
class Server final
{
public:
  static Server& GetInstance();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Starts, stops or restarts the endpoint to match the settings. Host thread.
  void ApplyConfig();
  void Shutdown();

  bool IsRunning() const;
  u16 GetPort() const;
  // One line for the settings UI: the URL, or why the server is not running.
  std::string GetStatusText() const;

  FrameCounters& GetFrameCounters() { return m_frame_counters; }
  OverrideSession& GetOverrideSession() { return m_override_session; }
  LogBuffer* GetLogBuffer() const { return m_log_buffer; }
  const ToolRegistry& GetRegistry() const { return m_registry; }
  CallGate& GetCallGate() { return m_call_gate; }
  // Set while the server shuts down: waits in tool handlers give up.
  bool IsStopping() const { return m_stopping.load(); }

  // Turbo (unthrottled) and muted audio for experiments; the Qt hotkey scheduler applies them.
  void SetSpeedOverrides(bool turbo, bool mute);
  bool IsTurboOverrideActive() const { return m_turbo.load(); }
  bool IsMuteOverrideActive() const { return m_mute.load(); }

  // Tool groups register cleanups that run when emulation stops (any thread; the video thread is
  // not running any more), and when the server stops (host thread; emulation may be running, so
  // video-thread work must be posted, not waited for).
  void AddEmulationStoppedCallback(std::function<void()> callback);
  void AddServerStoppedCallback(std::function<void()> callback);

  // The frontend's cheat code lists keep the codes they loaded and write the whole list back on
  // their next change, which would erase the codes a tool wrote to User/GameSettings/<GameID>.ini.
  // The frontend sets this callback to reload them; the code tools call NotifyGameCodesChanged on
  // their call's thread after each write.
  void SetGameCodesChangedCallback(std::function<void(const std::string& game_id)> callback);
  void NotifyGameCodesChanged(const std::string& game_id);

  // What the frontend does for the lifecycle tools (stop_game, boot_game, restart_game,
  // quit_dolphin): it owns the render window and the stop confirmation. The tools call the
  // actions on their call's thread; they queue the work on the UI thread and return. A frontend
  // that sets none (Android) makes the tools say so.
  struct FrontendActions
  {
    // Boots a file (disc image, executable or WAD) as the game list does.
    std::function<void(const std::string& path)> boot;
    // Stops emulation as the Stop button does, without asking. `force` skips the Wii's power
    // button shutdown, which a game that hangs never finishes.
    std::function<void(bool force)> stop;
    // Stops emulation without asking, then closes Dolphin.
    std::function<void(bool force)> quit;
  };
  void SetFrontendActions(FrontendActions actions);
  FrontendActions GetFrontendActions() const;
  // The file the frontend booted last (empty for a NAND title), for restart_game. Host thread.
  void SetLastBootPath(std::string path);
  std::string GetLastBootPath() const;

private:
  Server();
  ~Server();

  void Start(u16 port, const std::string& token);
  void Stop();
  void OnStateChanged(Core::State state);
  void OnConfigChanged();

  std::mutex m_stopped_callbacks_mutex;
  std::vector<std::function<void()>> m_emulation_stopped_callbacks;
  std::vector<std::function<void()>> m_server_stopped_callbacks;

  std::mutex m_game_codes_callback_mutex;
  std::function<void(const std::string& game_id)> m_game_codes_changed_callback;

  mutable std::mutex m_frontend_mutex;
  FrontendActions m_frontend_actions;
  std::string m_last_boot_path;

  ToolRegistry m_registry;
  CallGate m_call_gate;
  // Shared with the calls in progress: one stuck for good outlives Stop.
  std::shared_ptr<McpServer> m_mcp;
  HttpServer m_http;
  FrameCounters m_frame_counters;
  OverrideSession m_override_session;
  LogBuffer* m_log_buffer = nullptr;

  mutable std::mutex m_mutex;
  std::string m_last_error;
  bool m_enabled = false;
  u16 m_port = 0;
  std::string m_token;
  std::atomic<bool> m_stopping{false};
  std::atomic<bool> m_turbo{false};
  std::atomic<bool> m_mute{false};

  std::optional<Config::ConfigChangedCallbackID> m_config_callback;
  std::atomic<bool> m_config_job_pending{false};
};

// Read by DolphinQt/HotkeyScheduler, which owns the throttle and mute runtime overrides.
bool IsTurboOverrideActive();
bool IsMuteOverrideActive();

// The MCP "instructions": what the server is for and how to use the tools (Instructions.cpp).
std::string GetServerInstructions();

// Tool groups (Tools/*.cpp), registered by the Server constructor.
void RegisterEmulationTools(ToolRegistry& registry, Server& server);
void RegisterScreenshotTools(ToolRegistry& registry, Server& server);
void RegisterConfigTools(ToolRegistry& registry, Server& server);
void RegisterDrawTools(ToolRegistry& registry, Server& server);
void RegisterOverrideTools(ToolRegistry& registry, Server& server);
void RegisterCodeTools(ToolRegistry& registry, Server& server);
void RegisterMemoryTools(ToolRegistry& registry, Server& server);
void RegisterExperimentTools(ToolRegistry& registry, Server& server);
void RegisterInputTools(ToolRegistry& registry, Server& server);
void RegisterVRPatchTools(ToolRegistry& registry, Server& server);
}  // namespace AgentServer
