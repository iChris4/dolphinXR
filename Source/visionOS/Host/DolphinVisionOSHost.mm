// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host side of the Dolphin core on visionOS: the Host_* callbacks the core requires, and a host
// thread that boots the core, runs the jobs it queues for the host (Core::QueueHostJob) and shuts
// it down, as DolphinNoGUI's main loop does.

#include "DolphinVisionOSHost.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/Host.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/State.h"
#include "DiscIO/Volume.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/Present.h"

#ifdef ENABLE_VR
#include "Core/AgentServer/AgentServer.h"
#include "vr/visionos/xr_visionos.h"
#endif

namespace
{
std::mutex s_mutex;
std::condition_variable s_cv;
bool s_jobs_pending = false;
bool s_stop_requested = false;
std::thread s_host_thread;
std::atomic<bool> s_running = false;
bool s_initialized = false;
PowerPC::CPUCore s_cpu_core = PowerPC::CPUCore::CachedInterpreter;
std::string s_last_error;
std::atomic<int> s_load_slot_after_boot = 0;
int s_internal_resolution = 0;

constexpr int FIRST_STATE_SLOT = 1;
constexpr int LAST_STATE_SLOT = 10;

bool IsStateSlot(int slot)
{
  return slot >= FIRST_STATE_SLOT && slot <= LAST_STATE_SLOT;
}

// Test hook: lines "savestate N" / "loadstate N" in <user dir>/Commands.txt, which a test copies
// onto the device (devicectl device copy to). Run on the host thread, then the file is deleted.
void RunCommandFile(Core::System& system)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "Commands.txt";
  if (!File::Exists(path))
    return;
  std::string text;
  File::ReadFileToString(path, text);
  File::Delete(path);

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line))
  {
    std::istringstream words(line);
    std::string verb;
    int slot = 0;
    words >> verb >> slot;
    if (!IsStateSlot(slot))
      continue;
    NOTICE_LOG_FMT(COMMON, "visionOS host: command {} {}", verb, slot);
    if (verb == "savestate")
      State::Save(system, slot);
    else if (verb == "loadstate")
      State::Load(system, slot);
  }
}

void SetLastError(std::string error)
{
  ERROR_LOG_FMT(COMMON, "visionOS host: {}", error);
  std::lock_guard lock(s_mutex);
  s_last_error = std::move(error);
}

void WakeHostThread(bool stop)
{
  {
    std::lock_guard lock(s_mutex);
    s_jobs_pending = true;
    if (stop)
      s_stop_requested = true;
  }
  s_cv.notify_one();
}

#ifdef ENABLE_VR
void EnsureAgentServer();
#endif

void HostThreadMain(std::unique_ptr<BootParameters> boot, WindowSystemInfo wsi)
{
  Common::SetCurrentThreadName("Host thread");
  auto& system = Core::System::GetInstance();

  if (!BootManager::BootCore(system, std::move(boot), wsi))
  {
    SetLastError("Could not boot the selected file");
    s_running = false;
    return;
  }

  // The emulation thread reaches Running asynchronously; until it has, Uninitialized is not an
  // exit signal.
  bool started = false;
  for (u32 iteration = 0;; ++iteration)
  {
    {
      std::unique_lock lock(s_mutex);
      s_cv.wait_for(lock, std::chrono::milliseconds(16),
                    [] { return s_jobs_pending || s_stop_requested; });
      s_jobs_pending = false;
      if (s_stop_requested)
        break;
    }
    Core::HostDispatchJobs(system);

    const Core::State state = Core::GetState(system);
    if (state != Core::State::Uninitialized)
      started = true;
    else if (started)
      break;  // Emulation ended on its own (the game shut down, or a fatal error).

    if (state == Core::State::Running)
    {
      if (const int slot = s_load_slot_after_boot.exchange(0); IsStateSlot(slot))
        State::Load(system, slot);
      if (iteration % 16 == 0)  // About four times a second.
        RunCommandFile(system);
    }
#ifdef ENABLE_VR
    if (iteration % 128 == 0)  // About every two seconds.
      EnsureAgentServer();
#endif
  }

  Core::Stop(system);
  Core::Shutdown(system);
  s_running = false;
}

// MARK: Agent Server relay
//
// The Agent Server (MCP, [VR] AgentServer in GFX.ini) listens on 127.0.0.1 only, out of reach of
// the Mac where the agent runs. When it is on and has a token, connections to <its port + 1> on
// every interface are relayed to it: IPv6 too, as the CoreDevice tunnel (USB or Wi-Fi) is IPv6.
// Requests keep their Host header, which the server checks is a loopback name, so the Mac relays
// from its own 127.0.0.1 (Source/visionOS/agent-relay.py). The token guards the endpoint on the
// local network.
bool WriteAll(int fd, const char* data, ssize_t size)
{
  while (size > 0)
  {
    const ssize_t written = write(fd, data, size);
    if (written <= 0)
      return false;
    data += written;
    size -= written;
  }
  return true;
}

void RelayConnection(int client, u16 server_port)
{
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server_port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (server >= 0 &&
      connect(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
  {
    const int yes = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
    // Reset rather than close: the server closes first after a response, and a graceful close
    // would leave its port in TIME_WAIT, where the next launch of the app cannot listen on it
    // (SFML's listener does not set SO_REUSEADDR).
    const linger reset{1, 0};
    setsockopt(server, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    pollfd fds[2] = {{client, POLLIN, 0}, {server, POLLIN, 0}};
    char buffer[16384];
    bool open = true;
    while (open && poll(fds, 2, -1) > 0)
    {
      for (int i = 0; i < 2 && open; ++i)
      {
        if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
          continue;
        const ssize_t size = read(fds[i].fd, buffer, sizeof(buffer));
        open = size > 0 && WriteAll(fds[1 - i].fd, buffer, size);
      }
    }
  }
  if (server >= 0)
    close(server);
  close(client);
}

#ifdef ENABLE_VR
std::atomic<bool> s_agent_relay_started = false;

void StartAgentRelay()
{
  auto& agent = AgentServer::Server::GetInstance();
  if (s_agent_relay_started || !agent.IsRunning() ||
      Config::Get(Config::GFX_VR_AGENT_SERVER_TOKEN).empty())
  {
    return;
  }
  const u16 server_port = agent.GetPort();
  const u16 relay_port = server_port + 1;

  const int listener = socket(AF_INET6, SOCK_STREAM, 0);
  const int yes = 1, no = 0;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));  // IPv4 as well.
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(relay_port);
  address.sin6_addr = in6addr_any;
  if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 8) != 0)
  {
    ERROR_LOG_FMT(COMMON, "visionOS host: Agent Server relay cannot listen on port {}", relay_port);
    if (listener >= 0)
      close(listener);
    return;
  }
  std::thread([listener, server_port] {
    Common::SetCurrentThreadName("Agent relay");
    while (true)
    {
      const int client = accept(listener, nullptr, nullptr);
      if (client >= 0)
        std::thread(RelayConnection, client, server_port).detach();
      else if (errno != EINTR)
        break;
    }
  }).detach();
  s_agent_relay_started = true;
  NOTICE_LOG_FMT(COMMON, "visionOS host: Agent Server relayed on port {} (all interfaces)",
                 relay_port);
}

// Starts the Agent Server when the settings ask for it, then its relay. Called again from the host
// thread while it is not running: its port can stay taken for a while after the app relaunches.
void EnsureAgentServer()
{
  if (!Config::Get(Config::GFX_VR_AGENT_SERVER))
    return;
  auto& agent = AgentServer::Server::GetInstance();
  if (!agent.IsRunning())
    agent.ApplyConfig();
  StartAgentRelay();
}
#endif

// MARK: Gamepad
//
// visionOS has no keyboard device, so Dolphin's default GameCube mapping does nothing. GameCube
// pad 1 follows the first connected gamepad (SDL, on Apple's GameController framework) with the
// usual layout: positions, not labels, so A is the bottom face button whatever it is called.
Common::EventHook s_devices_changed_hook;
std::mutex s_gamepad_mutex;
std::string s_gamepad_name;

constexpr std::pair<const char*, const char*> GAMECUBE_LAYOUT[] = {
    {"Buttons/A", "`Button S`"},         {"Buttons/B", "`Button W`"},
    {"Buttons/X", "`Button E`"},         {"Buttons/Y", "`Button N`"},
    {"Buttons/Z", "`Shoulder R`"},       {"Buttons/Start", "`Start`"},
    {"Main Stick/Up", "`Left Y+`"},      {"Main Stick/Down", "`Left Y-`"},
    {"Main Stick/Left", "`Left X-`"},    {"Main Stick/Right", "`Left X+`"},
    {"C-Stick/Up", "`Right Y+`"},        {"C-Stick/Down", "`Right Y-`"},
    {"C-Stick/Left", "`Right X-`"},      {"C-Stick/Right", "`Right X+`"},
    // The digital press comes from the analog value past the Triggers threshold (90%).
    {"Triggers/L", "`Trigger L`"},       {"Triggers/R", "`Trigger R`"},
    {"Triggers/L-Analog", "`Trigger L`"}, {"Triggers/R-Analog", "`Trigger R`"},
    {"D-Pad/Up", "`Pad N`"},             {"D-Pad/Down", "`Pad S`"},
    {"D-Pad/Left", "`Pad W`"},           {"D-Pad/Right", "`Pad E`"},
    {"Rumble/Motor", "`Motor`"},
};

void MapGamepad()
{
  ControllerEmu::EmulatedController* const pad = Pad::GetConfig()->GetController(0);
  if (!pad)
    return;

  // A connected device already mapped (possibly edited by hand in GCPadNew.ini) stays.
  if (const auto device = g_controller_interface.FindDevice(pad->GetDefaultDevice());
      device && device->GetSource() == "SDL")
  {
    std::lock_guard lock(s_gamepad_mutex);
    s_gamepad_name = device->GetName();
    return;
  }

  std::string gamepad;
  for (const std::string& device : g_controller_interface.GetAllDeviceStrings())
  {
    if (device.starts_with("SDL/"))
    {
      gamepad = device;
      break;
    }
  }
  if (gamepad.empty())
  {
    std::lock_guard lock(s_gamepad_mutex);
    s_gamepad_name.clear();
    return;
  }

  Common::IniFile ini;
  Common::IniFile::Section* section = ini.GetOrCreateSection("GCPad1");
  section->Set("Device", gamepad);
  // std::string: a const char* would pick Set's bool overload and write "True".
  for (const auto& [control, expression] : GAMECUBE_LAYOUT)
    section->Set(control, std::string(expression));
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    pad->LoadConfig(section);
    pad->UpdateReferences(g_controller_interface);
  }
  // Saved, because booting a game reloads the pads from GCPadNew.ini.
  Pad::GetConfig()->SaveConfig();

  const auto device = g_controller_interface.FindDevice(pad->GetDefaultDevice());
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "visionOS host: GameCube pad 1 mapped to {}", gamepad);
  std::lock_guard lock(s_gamepad_mutex);
  s_gamepad_name = device ? device->GetName() : gamepad;
}
}  // namespace

// MARK: Host callbacks

std::vector<std::string> Host_GetPreferredLocales()
{
  std::vector<std::string> locales;
  for (NSString* language in [NSLocale preferredLanguages])
    locales.emplace_back([language UTF8String]);
  return locales;
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserJobDispatch)
    WakeHostThread(false);
  else if (id == HostMessageID::WMUserStop)
    WakeHostThread(true);
}

void Host_UpdateTitle(const std::string& title)
{
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return true;
}

bool Host_RendererHasFullFocus()
{
  return true;
}

bool Host_RendererIsFullscreen()
{
  return false;
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

// MARK: App interface

bool dvx_initialize(const char* user_dir)
{
  if (s_initialized)
    return true;
  if (!user_dir || !*user_dir)
  {
    SetLastError("No user directory");
    return false;
  }

  std::string dir = user_dir;
  if (dir.back() != '/')
    dir += '/';
  File::CreateFullPath(dir);

  UICommon::SetUserDirectory(dir);
  // StateSaves, ScreenShots, Load/... : Dolphin writes into these without creating them.
  UICommon::CreateDirectories();
  UICommon::Init();

  // Metal is the only video backend on visionOS.
  Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Metal"));

  // Controllers have no window to attach to; the game layer is set per boot.
  WindowSystemInfo wsi(WindowSystemType::VisionOS, nullptr, nullptr, nullptr);
  UICommon::InitControllers(wsi);
  // Gamepads arrive asynchronously (SDL's hotplug thread), and may come and go.
  s_devices_changed_hook = g_controller_interface.RegisterDevicesChangedCallback(MapGamepad);
  MapGamepad();

#ifdef ENABLE_VR
  // Off unless [VR] AgentServer is set in GFX.ini; it follows the settings afterwards.
  EnsureAgentServer();
#endif

  s_initialized = true;
  NOTICE_LOG_FMT(COMMON, "visionOS host: initialized, user directory {}", dir);
  return true;
}

void dvx_set_cpu_core(int core)
{
  switch (core)
  {
  case DVX_CPU_INTERPRETER:
    s_cpu_core = PowerPC::CPUCore::Interpreter;
    break;
  case DVX_CPU_STATIC_RECOMP:
    s_cpu_core = PowerPC::CPUCore::StaticRecomp;
    break;
  default:
    s_cpu_core = PowerPC::CPUCore::CachedInterpreter;
    break;
  }
}

bool dvx_has_recomp_module(const char* path)
{
  if (!path || !*path)
    return false;
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(std::string(path));
  if (!volume)
    return false;
  return !FindStaticRecompModule(volume->GetGameID(volume->GetGamePartition())).empty();
}

bool dvx_boot(const char* path, void* metal_layer)
{
  if (!s_initialized)
  {
    SetLastError("dvx_initialize was not called");
    return false;
  }
  if (s_running)
  {
    SetLastError("Emulation is already running");
    return false;
  }
  if (!path || !metal_layer)
  {
    SetLastError("No file or no Metal layer");
    return false;
  }

  std::unique_ptr<BootParameters> boot = BootParameters::GenerateFromFile(path);
  if (!boot)
  {
    SetLastError(std::string("Not a bootable file: ") + path);
    return false;
  }

  if (s_host_thread.joinable())
    s_host_thread.join();

  // This run only: the core and resolution choices are the app's, not saved settings.
  Config::SetCurrent(Config::MAIN_CPU_CORE, s_cpu_core);
  if (s_internal_resolution > 0)
    Config::SetCurrent(Config::GFX_EFB_SCALE, s_internal_resolution);
  NOTICE_LOG_FMT(COMMON, "visionOS host: booting {} (CPU core {}, VR {}, resolution {}x)", path,
                 static_cast<int>(s_cpu_core),
                 Config::Get(Config::GFX_VR_ENABLE_OPENXR) ? "on" : "off",
                 Config::Get(Config::GFX_EFB_SCALE));

  CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer;
  WindowSystemInfo wsi(WindowSystemType::VisionOS, nullptr, nullptr, metal_layer);
  wsi.render_surface_scale = static_cast<float>(layer.contentsScale);

  {
    std::lock_guard lock(s_mutex);
    s_last_error.clear();
    s_jobs_pending = false;
    s_stop_requested = false;
  }
  s_running = true;
  s_host_thread = std::thread(HostThreadMain, std::move(boot), wsi);
  return true;
}

void dvx_stop(void)
{
  if (!s_host_thread.joinable())
    return;
  WakeHostThread(true);
  s_host_thread.join();
}

void dvx_set_paused(bool paused)
{
  if (!s_running)
    return;
  Core::QueueHostJob([paused](Core::System& system) {
    Core::SetState(system, paused ? Core::State::Paused : Core::State::Running);
  });
}

bool dvx_is_running(void)
{
  return s_running;
}

void dvx_surface_resized(void)
{
  if (g_presenter)
    g_presenter->ResizeSurface();
}

void dvx_set_internal_resolution(int scale)
{
  s_internal_resolution = std::clamp(scale, 0, 8);
}

void dvx_save_state(int slot)
{
  if (!s_running || !IsStateSlot(slot))
    return;
  Core::QueueHostJob([slot](Core::System& system) { State::Save(system, slot); });
}

void dvx_load_state(int slot)
{
  if (!s_running || !IsStateSlot(slot))
    return;
  Core::QueueHostJob([slot](Core::System& system) { State::Load(system, slot); });
}

void dvx_load_state_after_boot(int slot)
{
  s_load_slot_after_boot = IsStateSlot(slot) ? slot : 0;
}

const char* dvx_gamepad_name(void)
{
  static thread_local std::string copy;
  std::lock_guard lock(s_gamepad_mutex);
  copy = s_gamepad_name;
  return copy.c_str();
}

const char* dvx_last_error(void)
{
  static thread_local std::string copy;
  std::lock_guard lock(s_mutex);
  copy = s_last_error;
  return copy.c_str();
}

// MARK: VR

bool dvx_vr_available(void)
{
#ifdef ENABLE_VR
  return true;
#else
  return false;
#endif
}

void dvx_set_vr_enabled(bool enabled)
{
#ifdef ENABLE_VR
  // This run only, like the CPU core: OpenXR stereo needs the immersive space the app opened.
  Config::SetCurrent(Config::GFX_VR_ENABLE_OPENXR, enabled);
#endif
}

void dvx_set_layer_renderer(void* layer_renderer)
{
#ifdef ENABLE_VR
  xr_visionos_set_layer_renderer(layer_renderer);
#endif
}

bool dvx_immersive_space_closed(void)
{
#ifdef ENABLE_VR
  return xr_visionos_layer_invalidated();
#else
  return true;
#endif
}

void dvx_spatial_event(unsigned long long event_id, int phase, int chirality, bool has_ray,
                       float origin_x, float origin_y, float origin_z, float direction_x,
                       float direction_y, float direction_z, bool has_pose, float pose_x,
                       float pose_y, float pose_z)
{
#ifdef ENABLE_VR
  xr_visionos_spatial_event(event_id, phase, chirality, has_ray, origin_x, origin_y, origin_z,
                            direction_x, direction_y, direction_z, has_pose, pose_x, pose_y,
                            pose_z);
#endif
}
