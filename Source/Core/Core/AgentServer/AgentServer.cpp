// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/AgentServer.h"

#include <algorithm>
#include <exception>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/Version.h"
#include "Core/AgentServer/LogBuffer.h"
#include "Core/AgentServer/McpServer.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/VR/AgentVideoJobs.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace AgentServer
{
namespace
{
struct DesiredConfig
{
  bool enabled;
  u16 port;
  std::string token;
};

DesiredConfig ReadConfig()
{
  return {Config::Get(Config::GFX_VR_AGENT_SERVER),
          static_cast<u16>(std::clamp(Config::Get(Config::GFX_VR_AGENT_SERVER_PORT),
                                      Config::GFX_VR_AGENT_SERVER_PORT_MIN,
                                      Config::GFX_VR_AGENT_SERVER_PORT_MAX)),
          Config::Get(Config::GFX_VR_AGENT_SERVER_TOKEN)};
}
}  // namespace

Server& Server::GetInstance()
{
  static Server s_server;
  return s_server;
}

Server::Server()
{
  RegisterEmulationTools(m_registry, *this);
  RegisterScreenshotTools(m_registry, *this);
  RegisterConfigTools(m_registry, *this);
  RegisterDrawTools(m_registry, *this);
  RegisterOverrideTools(m_registry, *this);
  RegisterCodeTools(m_registry, *this);
  RegisterMemoryTools(m_registry, *this);
  RegisterExperimentTools(m_registry, *this);
}

namespace
{
// Agent code is built with exceptions; the Dolphin code that calls its callbacks and jobs may not
// be (see Core/CMakeLists.txt), so nothing may unwind out of them.
template <typename F>
void RunWithoutThrowing(const char* what, F&& function)
{
  try
  {
    function();
  }
  catch (const std::exception& e)
  {
    ERROR_LOG_FMT(OPENXR, "AgentServer: {} failed: {}", what, e.what());
  }
  catch (...)
  {
    ERROR_LOG_FMT(OPENXR, "AgentServer: {} failed", what);
  }
}
}  // namespace

Server::~Server()
{
  Stop();
}

void Server::ApplyConfig()
{
  if (!m_config_callback)
    m_config_callback = Config::AddConfigChangedCallback([this] { OnConfigChanged(); });

  const DesiredConfig desired = ReadConfig();
  std::lock_guard lock(m_mutex);
  m_enabled = desired.enabled;
  if (!desired.enabled)
  {
    Stop();
    m_last_error.clear();
    return;
  }
  if (m_http.IsRunning() && desired.port == m_port && desired.token == m_token)
    return;

  m_port = desired.port;
  m_token = desired.token;
  Start(desired.port, desired.token);
}

void Server::Shutdown()
{
  if (m_config_callback)
  {
    Config::RemoveConfigChangedCallback(*m_config_callback);
    m_config_callback.reset();
  }
  std::lock_guard lock(m_mutex);
  Stop();
}

void Server::OnConfigChanged()
{
  // Any thread, often the server's own (tools change settings): only compare here, and leave the
  // restart to the host thread so the server never joins its own thread.
  const DesiredConfig desired = ReadConfig();
  {
    std::lock_guard lock(m_mutex);
    if (desired.enabled == m_enabled && desired.port == m_port && desired.token == m_token)
      return;
  }
  if (m_config_job_pending.exchange(true))
    return;
  Core::QueueHostJob(
      [this](Core::System&) {
        m_config_job_pending.store(false);
        RunWithoutThrowing("applying the settings", [this] { ApplyConfig(); });
      },
      true);
}

void Server::Start(u16 port, const std::string& token)
{
  Stop();
  m_stopping.store(false);

  if (auto* log_manager = Common::Log::LogManager::GetInstance())
  {
    if (!m_log_buffer)
    {
      auto buffer = std::make_unique<LogBuffer>();
      m_log_buffer = buffer.get();
      log_manager->RegisterListener(Common::Log::LogListener::AGENT_LISTENER, std::move(buffer));
    }
    log_manager->EnableListener(Common::Log::LogListener::AGENT_LISTENER, true);
  }

  m_frame_counters.SetStateCallback([this](Core::State state) { OnStateChanged(state); });
  m_frame_counters.Start(Core::System::GetInstance());

  McpServerOptions options;
  options.server_version = Common::GetScmDescStr();
  options.instructions = GetServerInstructions();
  options.token = token;
  m_mcp = std::make_unique<McpServer>(m_registry, std::move(options));

  std::string error;
  if (!m_http.Start(
          port, [this](const HttpRequest& request) { return m_mcp->Handle(request); }, &error))
  {
    m_last_error = error;
    ERROR_LOG_FMT(OPENXR, "AgentServer: {}", error);
    Core::DisplayMessage(fmt::format("Agent Server: {}", error), 6000);
    m_frame_counters.Stop();
    m_mcp.reset();
    return;
  }

  m_last_error.clear();
  NOTICE_LOG_FMT(OPENXR, "AgentServer: listening on http://127.0.0.1:{}{}", port,
                 MCP_ENDPOINT_PATH);
  Core::DisplayMessage(fmt::format("Agent Server listening on 127.0.0.1:{}", port), 4000);
}

void Server::Stop()
{
  if (!m_http.IsRunning())
    return;

  // Wake up any tool handler waiting on emulation so the server thread can be joined.
  m_stopping.store(true);
  VR::AgentVideoJobs::CancelAll();
  m_http.Stop();
  m_frame_counters.Stop();
  m_mcp.reset();

  if (auto* log_manager = Common::Log::LogManager::GetInstance(); log_manager && m_log_buffer)
    log_manager->EnableListener(Common::Log::LogListener::AGENT_LISTENER, false);

  // Leave nothing the agent changed behind: speed overrides, look offset, previews...
  SetSpeedOverrides(false, false);
  if (VR::g_openxr)
    VR::g_openxr->SetAgentLookOffset(0.0f, 0.0f, 0.0f);
  {
    std::lock_guard lock(m_stopped_callbacks_mutex);
    for (const auto& callback : m_server_stopped_callbacks)
      RunWithoutThrowing("a server-stopped callback", callback);
  }
  m_stopping.store(false);
  NOTICE_LOG_FMT(OPENXR, "AgentServer: stopped");
}

void Server::OnStateChanged(Core::State state)
{
  if (state == Core::State::Stopping)
  {
    // Wake up tool handlers waiting on the video thread.
    VR::AgentVideoJobs::CancelAll();
    return;
  }
  if (state != Core::State::Uninitialized)
    return;

  // Emulation is shut down: the video thread no longer runs.
  VR::AgentVideoJobs::CancelAll();
  SetSpeedOverrides(false, false);
  m_override_session.Forget();
  std::lock_guard lock(m_stopped_callbacks_mutex);
  for (const auto& callback : m_emulation_stopped_callbacks)
    RunWithoutThrowing("an emulation-stopped callback", callback);
}

bool Server::IsRunning() const
{
  return m_http.IsRunning();
}

u16 Server::GetPort() const
{
  std::lock_guard lock(m_mutex);
  return m_port;
}

std::string Server::GetStatusText() const
{
  std::lock_guard lock(m_mutex);
  if (m_http.IsRunning())
    return fmt::format("Listening on http://127.0.0.1:{}{}", m_port, MCP_ENDPOINT_PATH);
  if (m_enabled && !m_last_error.empty())
    return m_last_error;
  return "Not running";
}

void Server::SetSpeedOverrides(bool turbo, bool mute)
{
  m_turbo.store(turbo);
  m_mute.store(mute);
}

void Server::AddEmulationStoppedCallback(std::function<void()> callback)
{
  std::lock_guard lock(m_stopped_callbacks_mutex);
  m_emulation_stopped_callbacks.push_back(std::move(callback));
}

void Server::AddServerStoppedCallback(std::function<void()> callback)
{
  std::lock_guard lock(m_stopped_callbacks_mutex);
  m_server_stopped_callbacks.push_back(std::move(callback));
}

bool IsTurboOverrideActive()
{
  return Server::GetInstance().IsTurboOverrideActive();
}

bool IsMuteOverrideActive()
{
  return Server::GetInstance().IsMuteOverrideActive();
}
}  // namespace AgentServer
