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
// Threads: requests are served one at a time on the server's own thread. Tool handlers reach the
// CPU thread with Core::CPUThreadGuard / Core::RunOnCPUThread and the video thread with
// VR::AgentVideoJobs, and only ever wait with timeouts (see ToolHelpers.h).
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

  ToolRegistry m_registry;
  std::unique_ptr<McpServer> m_mcp;
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
}  // namespace AgentServer
