// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/HotkeyScheduler.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include <fmt/format.h>

#include <QApplication>
#include <QCoreApplication>

#include "AudioCommon/AudioCommon.h"

#include "Common/Config/Config.h"
#include "Common/Thread.h"

#include "Core/AchievementManager.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/IOS.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#ifdef HAS_LIBMGBA
#include "DolphinQt/GBAWidget.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#endif
#include "DolphinQt/Settings.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "VideoCommon/CullingCodeFinder.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VideoConfig.h"
#ifdef ENABLE_VR
#include "VideoCommon/VR/OpenXRManager.h"
#endif

constexpr const char* DUBOIS_ALGORITHM_SHADER = "dubois";
constexpr float HOTKEY_VR_UNITS_PER_METER_MIN = 0.01f;
constexpr float HOTKEY_VR_UNITS_PER_METER_MAX = 1000.0f;

HotkeyScheduler::HotkeyScheduler() : m_stop_requested(false)
{
  HotkeyManagerEmu::Enable(true);
}

HotkeyScheduler::~HotkeyScheduler()
{
  Stop();
}

void HotkeyScheduler::Start()
{
  m_stop_requested.Set(false);
  m_thread = std::thread(&HotkeyScheduler::Run, this);
}

void HotkeyScheduler::Stop()
{
  m_stop_requested.Set(true);

  if (m_thread.joinable())
    m_thread.join();
}

static bool IsHotkey(int id, bool held = false)
{
  return HotkeyManagerEmu::IsPressed(id, held);
}

static void HandleFrameStepHotkeys()
{
  constexpr int MAX_FRAME_STEP_DELAY = 60;
  constexpr int FRAME_STEP_DELAY = 30;

  static int frame_step_count = 0;
  static int frame_step_delay = 1;
  static int frame_step_delay_count = 0;
  static bool frame_step_hold = false;

  if (IsHotkey(HK_FRAME_ADVANCE_INCREASE_SPEED))
  {
    frame_step_delay = std::max(frame_step_delay - 1, 0);
    return;
  }

  if (IsHotkey(HK_FRAME_ADVANCE_DECREASE_SPEED))
  {
    frame_step_delay = std::min(frame_step_delay + 1, MAX_FRAME_STEP_DELAY);
    return;
  }

  if (IsHotkey(HK_FRAME_ADVANCE_RESET_SPEED))
  {
    frame_step_delay = 1;
    return;
  }

  if (IsHotkey(HK_FRAME_ADVANCE, true))
  {
    if (frame_step_delay_count < frame_step_delay && frame_step_hold)
      frame_step_delay_count++;

    if ((frame_step_count == 0 || frame_step_count == FRAME_STEP_DELAY) && !frame_step_hold)
    {
      if (frame_step_count > 0)
        Settings::Instance().SetIsContinuouslyFrameStepping(true);
      Core::QueueHostJob([](auto& system) { Core::DoFrameStep(system); });
      frame_step_hold = true;
    }

    if (frame_step_count < FRAME_STEP_DELAY)
    {
      frame_step_count++;
      frame_step_hold = false;
    }

    if (frame_step_count == FRAME_STEP_DELAY && frame_step_hold &&
        frame_step_delay_count >= frame_step_delay)
    {
      frame_step_hold = false;
      frame_step_delay_count = 0;
    }

    return;
  }
  else if (frame_step_count > 0)
  {
    // Reset frame advance
    frame_step_count = 0;
    frame_step_hold = false;
    frame_step_delay_count = 0;
    Settings::Instance().SetIsContinuouslyFrameStepping(false);
    emit Settings::Instance().EmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  }
}

void HotkeyScheduler::Run()
{
  Common::SetCurrentThreadName("HotkeyScheduler");

  while (!m_stop_requested.IsSet())
  {
    Common::SleepCurrentThread(5);

    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::FreeLook);
    g_controller_interface.UpdateInput();
    FreeLook::UpdateInput();

    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
    g_controller_interface.UpdateInput();

    if (!HotkeyManagerEmu::IsEnabled())
      continue;

    Core::System& system = Core::System::GetInstance();
    if (Core::GetState(system) != Core::State::Stopping)
    {
      // Obey window focus (config permitting) before checking hotkeys.
      Core::UpdateInputGate(Config::Get(Config::MAIN_FOCUSED_HOTKEYS));

      HotkeyManagerEmu::GetStatus(false);

      // Everything else on the host thread (controller config dialog) should always get input.
      ControlReference::SetInputGate(true);

      HotkeyManagerEmu::GetStatus(true);

      // Open
      if (IsHotkey(HK_OPEN))
        emit Open();

      // Refresh Game List
      if (IsHotkey(HK_REFRESH_LIST))
        emit RefreshGameListHotkey();

      // Recording
      if (IsHotkey(HK_START_RECORDING))
        emit StartRecording();

      // Exit
      if (IsHotkey(HK_EXIT))
        emit ExitHotkey();

#ifdef USE_RETRO_ACHIEVEMENTS
      if (IsHotkey(HK_OPEN_ACHIEVEMENTS))
        emit OpenAchievements();
#endif  // USE_RETRO_ACHIEVEMENTS

      if (Core::IsUninitialized(system))
      {
        // Only check for Play Recording hotkey when no game is running
        if (IsHotkey(HK_PLAY_RECORDING))
          emit PlayRecording();

        continue;
      }

      // Disc

      if (IsHotkey(HK_EJECT_DISC))
        emit EjectDisc();

      if (IsHotkey(HK_CHANGE_DISC))
        emit ChangeDisc();

      // Fullscreen
      if (IsHotkey(HK_FULLSCREEN))
      {
        emit FullScreenHotkey();

        // Prevent fullscreen from getting toggled too often
        Common::SleepCurrentThread(100);
      }

      // Pause and Unpause
      if (IsHotkey(HK_PLAY_PAUSE))
        emit TogglePauseHotkey();

      // Stop
      if (IsHotkey(HK_STOP))
        emit StopHotkey();

      // Reset
      if (IsHotkey(HK_RESET))
        emit ResetHotkey();

      // Frame advance
      HandleFrameStepHotkeys();

      // Screenshot
      if (IsHotkey(HK_SCREENSHOT))
        emit ScreenShotHotkey();

      // Unlock Cursor
      if (IsHotkey(HK_UNLOCK_CURSOR))
        emit UnlockCursor();

      if (IsHotkey(HK_CENTER_MOUSE, true))
        g_controller_interface.SetMouseCenteringRequested(true);

      auto& settings = Settings::Instance();

      // Toggle Chat
      if (IsHotkey(HK_ACTIVATE_CHAT))
        emit ActivateChat();

      if (IsHotkey(HK_REQUEST_GOLF_CONTROL))
        emit RequestGolfControl();

      if (IsHotkey(HK_EXPORT_RECORDING))
        emit ExportRecording();

      if (IsHotkey(HK_READ_ONLY_MODE))
        emit ToggleReadOnlyMode();

      // Wiimote
      if (auto bt = WiiUtils::GetBluetoothRealDevice())
        bt->UpdateSyncButtonState(IsHotkey(HK_TRIGGER_SYNC_BUTTON, true));

      if (Config::IsDebuggingEnabled())
      {
        CheckDebuggingHotkeys();
      }

      // TODO: HK_MBP_ADD

      if (Core::System::GetInstance().IsWii())
      {
        if (IsHotkey(HK_WIIMOTE1_CONNECT))
          emit ConnectWiiRemote(0);
        if (IsHotkey(HK_WIIMOTE2_CONNECT))
          emit ConnectWiiRemote(1);
        if (IsHotkey(HK_WIIMOTE3_CONNECT))
          emit ConnectWiiRemote(2);
        if (IsHotkey(HK_WIIMOTE4_CONNECT))
          emit ConnectWiiRemote(3);
        if (IsHotkey(HK_BALANCEBOARD_CONNECT))
          emit ConnectWiiRemote(4);

        if (IsHotkey(HK_TOGGLE_SD_CARD))
          Settings::Instance().SetSDCardInserted(!Settings::Instance().IsSDCardInserted());

        if (IsHotkey(HK_TOGGLE_USB_KEYBOARD))
        {
          Settings::Instance().SetUSBKeyboardConnected(
              !Settings::Instance().IsUSBKeyboardConnected());
        }

        if (IsHotkey(HK_TOGGLE_WII_SPEAK_MUTE))
        {
          const bool muted = !Settings::Instance().IsWiiSpeakMuted();
          Settings::Instance().SetWiiSpeakMuted(muted);
          OSD::AddMessage(muted ? "Wii Speak muted" : "Wii Speak unmuted");
        }
      }

      if (IsHotkey(HK_PREV_WIIMOTE_PROFILE_1))
        m_profile_cycler.PreviousWiimoteProfile(0);
      else if (IsHotkey(HK_NEXT_WIIMOTE_PROFILE_1))
        m_profile_cycler.NextWiimoteProfile(0);

      if (IsHotkey(HK_PREV_WIIMOTE_PROFILE_2))
        m_profile_cycler.PreviousWiimoteProfile(1);
      else if (IsHotkey(HK_NEXT_WIIMOTE_PROFILE_2))
        m_profile_cycler.NextWiimoteProfile(1);

      if (IsHotkey(HK_PREV_WIIMOTE_PROFILE_3))
        m_profile_cycler.PreviousWiimoteProfile(2);
      else if (IsHotkey(HK_NEXT_WIIMOTE_PROFILE_3))
        m_profile_cycler.NextWiimoteProfile(2);

      if (IsHotkey(HK_PREV_WIIMOTE_PROFILE_4))
        m_profile_cycler.PreviousWiimoteProfile(3);
      else if (IsHotkey(HK_NEXT_WIIMOTE_PROFILE_4))
        m_profile_cycler.NextWiimoteProfile(3);

      if (IsHotkey(HK_PREV_GAME_WIIMOTE_PROFILE_1))
        m_profile_cycler.PreviousWiimoteProfileForGame(0);
      else if (IsHotkey(HK_NEXT_GAME_WIIMOTE_PROFILE_1))
        m_profile_cycler.NextWiimoteProfileForGame(0);

      if (IsHotkey(HK_PREV_GAME_WIIMOTE_PROFILE_2))
        m_profile_cycler.PreviousWiimoteProfileForGame(1);
      else if (IsHotkey(HK_NEXT_GAME_WIIMOTE_PROFILE_2))
        m_profile_cycler.NextWiimoteProfileForGame(1);

      if (IsHotkey(HK_PREV_GAME_WIIMOTE_PROFILE_3))
        m_profile_cycler.PreviousWiimoteProfileForGame(2);
      else if (IsHotkey(HK_NEXT_GAME_WIIMOTE_PROFILE_3))
        m_profile_cycler.NextWiimoteProfileForGame(2);

      if (IsHotkey(HK_PREV_GAME_WIIMOTE_PROFILE_4))
        m_profile_cycler.PreviousWiimoteProfileForGame(3);
      else if (IsHotkey(HK_NEXT_GAME_WIIMOTE_PROFILE_4))
        m_profile_cycler.NextWiimoteProfileForGame(3);

      auto ShowVolume = [] {
        OSD::AddMessage(std::string("Volume: ") +
                        (Config::Get(Config::MAIN_AUDIO_MUTED) ?
                             "Muted" :
                             std::to_string(Config::Get(Config::MAIN_AUDIO_VOLUME)) + "%"));
      };

      // Volume
      if (IsHotkey(HK_VOLUME_DOWN))
      {
        settings.DecreaseVolume(3);
        ShowVolume();
      }

      if (IsHotkey(HK_VOLUME_UP))
      {
        settings.IncreaseVolume(3);
        ShowVolume();
      }

      if (IsHotkey(HK_VOLUME_TOGGLE_MUTE))
      {
        AudioCommon::ToggleMuteVolume(system);
        ShowVolume();
      }

      // Graphics
      const auto efb_scale = Config::Get(Config::GFX_EFB_SCALE);
      const auto ShowEFBScale = [](int new_efb_scale) {
        switch (new_efb_scale)
        {
        case EFB_SCALE_AUTO_INTEGRAL:
          OSD::AddMessage("Internal Resolution: Auto (integral)");
          break;
        case 1:
          OSD::AddMessage("Internal Resolution: Native");
          break;
        default:
          OSD::AddMessage(fmt::format("Internal Resolution: {}x", new_efb_scale));
          break;
        }
      };

      if (IsHotkey(HK_INCREASE_IR))
      {
        Config::SetCurrent(Config::GFX_EFB_SCALE, efb_scale + 1);
        ShowEFBScale(efb_scale + 1);
      }
      if (IsHotkey(HK_DECREASE_IR))
      {
        if (efb_scale > EFB_SCALE_AUTO_INTEGRAL)
        {
          Config::SetCurrent(Config::GFX_EFB_SCALE, efb_scale - 1);
          ShowEFBScale(efb_scale - 1);
        }
      }

      if (IsHotkey(HK_TOGGLE_CROP))
        Config::SetCurrent(Config::GFX_CROP, !Config::Get(Config::GFX_CROP));

      if (IsHotkey(HK_TOGGLE_AR))
      {
        const int aspect_ratio = (static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)) + 1) & 3;
        Config::SetCurrent(Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(aspect_ratio));
        switch (static_cast<AspectMode>(aspect_ratio))
        {
        case AspectMode::Stretch:
          OSD::AddMessage("Stretch");
          break;
        case AspectMode::ForceStandard:
          OSD::AddMessage("Force 4:3");
          break;
        case AspectMode::ForceWide:
          OSD::AddMessage("Force 16:9");
          break;
        case AspectMode::Custom:
          OSD::AddMessage("Custom");
          break;
        case AspectMode::CustomStretch:
          OSD::AddMessage("Custom (Stretch)");
          break;
        case AspectMode::Raw:
          OSD::AddMessage("Raw (Square Pixels)");
          break;
        case AspectMode::Auto:
        default:
          OSD::AddMessage("Auto");
          break;
        }
      }

      if (IsHotkey(HK_TOGGLE_SKIP_EFB_ACCESS))
      {
        const bool new_value = !Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE);
        Config::SetCurrent(Config::GFX_HACK_EFB_ACCESS_ENABLE, new_value);
        OSD::AddMessage(fmt::format("{} EFB Access from CPU", new_value ? "Skip" : "Don't skip"));
      }

      if (IsHotkey(HK_TOGGLE_EFBCOPIES))
      {
        const bool new_value = !Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
        Config::SetCurrent(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, new_value);
        OSD::AddMessage(fmt::format("Copy EFB: {}", new_value ? "to Texture" : "to RAM"));
      }

      auto ShowXFBCopies = [] {
        OSD::AddMessage(fmt::format(
            "Copy XFB: {}{}", Config::Get(Config::GFX_HACK_IMMEDIATE_XFB) ? " (Immediate)" : "",
            Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM) ? "to Texture" : "to RAM"));
      };

      if (IsHotkey(HK_TOGGLE_XFBCOPIES))
      {
        Config::SetCurrent(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                           !Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM));
        ShowXFBCopies();
      }
      if (IsHotkey(HK_TOGGLE_IMMEDIATE_XFB))
      {
        Config::SetCurrent(Config::GFX_HACK_IMMEDIATE_XFB,
                           !Config::Get(Config::GFX_HACK_IMMEDIATE_XFB));
        ShowXFBCopies();
      }
      if (IsHotkey(HK_TOGGLE_FOG))
      {
        const bool new_value = !Config::Get(Config::GFX_DISABLE_FOG);
        Config::SetCurrent(Config::GFX_DISABLE_FOG, new_value);
        OSD::AddMessage(fmt::format("Fog: {}", new_value ? "Enabled" : "Disabled"));
      }

      if (IsHotkey(HK_TOGGLE_DUMPTEXTURES))
      {
        const bool enable_dumping = !Config::Get(Config::GFX_DUMP_TEXTURES);
        Config::SetCurrent(Config::GFX_DUMP_TEXTURES, enable_dumping);
        OSD::AddMessage(
            fmt::format("Texture Dumping {}",
                        enable_dumping ? "enabled. This will reduce performance." : "disabled."),
            OSD::Duration::NORMAL);
      }

      if (IsHotkey(HK_TOGGLE_TEXTURES))
        Config::SetCurrent(Config::GFX_HIRES_TEXTURES, !Config::Get(Config::GFX_HIRES_TEXTURES));

      // VR
      auto ToggleVRSetting = [](const auto& setting, const char* label) {
        const bool new_value = !Config::Get(setting);
        Config::SetCurrent(setting, new_value);
        OSD::AddMessage(fmt::format("{}: {}", label, new_value ? "Enabled" : "Disabled"));
      };

      auto AdjustVRFloatSetting = [](const auto& setting, const float delta, const float min_value,
                                     const float max_value, const char* label, const int precision) {
        const float current = Config::Get(setting);
        const float new_value = std::clamp(current + delta, min_value, max_value);
        Config::SetCurrent(setting, new_value);
        OSD::AddMessage(fmt::format("{}: {:.{}f}", label, new_value, precision));
      };

      auto ToggleVRForcedVBI = [] {
        const int current_value =
            Config::NormalizeVRForcedVBIFrequency(Config::Get(Config::GFX_VR_FORCED_VBI_FREQUENCY));
        const int new_value = current_value == Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF ?
                                  Config::GFX_VR_FORCED_VBI_FREQUENCY_AUTO :
                                  Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF;
        Config::SetCurrent(Config::GFX_VR_AUTO_VBI_FROM_HMD, false);
        Config::SetCurrent(Config::GFX_VR_FORCED_VBI_FREQUENCY, new_value);

        const std::string label = new_value == Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF ?
                                      "Off" :
                                  new_value == Config::GFX_VR_FORCED_VBI_FREQUENCY_AUTO ?
                                      "Auto" :
                                      fmt::format("{} Hz", new_value);
        OSD::AddMessage(fmt::format("Force VBI: {}", label));
      };

      if (IsHotkey(HK_VR_TOGGLE_OPENXR))
        ToggleVRSetting(Config::GFX_VR_ENABLE_OPENXR, "OpenXR");

      if (IsHotkey(HK_VR_RESET_POSITION))
      {
#ifdef ENABLE_VR
        if (VR::g_openxr)
        {
          VR::g_openxr->RequestRecenter();
          OSD::AddMessage("VR Position: Recenter requested");
        }
        else
        {
          OSD::AddMessage("VR Position: OpenXR is not active");
        }
#else
        OSD::AddMessage("VR Position: VR backend not enabled");
#endif
      }

      if (IsHotkey(HK_VR_DECREASE_UNITS_PER_METER))
      {
        AdjustVRFloatSetting(Config::GFX_VR_UNITS_PER_METER, -Config::GFX_VR_UNITS_PER_METER_STEP,
                             HOTKEY_VR_UNITS_PER_METER_MIN, HOTKEY_VR_UNITS_PER_METER_MAX,
                             "Units Per Meter", 2);
      }
      if (IsHotkey(HK_VR_INCREASE_UNITS_PER_METER))
      {
        AdjustVRFloatSetting(Config::GFX_VR_UNITS_PER_METER, Config::GFX_VR_UNITS_PER_METER_STEP,
                             HOTKEY_VR_UNITS_PER_METER_MIN, HOTKEY_VR_UNITS_PER_METER_MAX,
                             "Units Per Meter", 2);
      }

      if (IsHotkey(HK_VR_DECREASE_LEAN_BACK_ANGLE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_LEAN_BACK_ANGLE, -Config::GFX_VR_LEAN_BACK_ANGLE_STEP,
                             Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                             Config::GFX_VR_LEAN_BACK_ANGLE_MAX, "Lean Back Angle", 1);
      }
      if (IsHotkey(HK_VR_INCREASE_LEAN_BACK_ANGLE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_LEAN_BACK_ANGLE, Config::GFX_VR_LEAN_BACK_ANGLE_STEP,
                             Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                             Config::GFX_VR_LEAN_BACK_ANGLE_MAX, "Lean Back Angle", 1);
      }

      if (IsHotkey(HK_VR_TOGGLE_ENABLE_CAMERA_FORWARD))
        ToggleVRSetting(Config::GFX_VR_ENABLE_CAMERA_FORWARD, "Enable Camera Forward");

      if (IsHotkey(HK_VR_DECREASE_CAMERA_FORWARD))
      {
        AdjustVRFloatSetting(Config::GFX_VR_CAMERA_FORWARD, -Config::GFX_VR_CAMERA_FORWARD_STEP,
                             Config::GFX_VR_CAMERA_FORWARD_MIN, Config::GFX_VR_CAMERA_FORWARD_MAX,
                             "Camera Forward", 2);
      }
      if (IsHotkey(HK_VR_INCREASE_CAMERA_FORWARD))
      {
        AdjustVRFloatSetting(Config::GFX_VR_CAMERA_FORWARD, Config::GFX_VR_CAMERA_FORWARD_STEP,
                             Config::GFX_VR_CAMERA_FORWARD_MIN, Config::GFX_VR_CAMERA_FORWARD_MAX,
                             "Camera Forward", 2);
      }

      if (IsHotkey(HK_VR_TOGGLE_ENABLE_CAMERA_HEIGHT))
        ToggleVRSetting(Config::GFX_VR_ENABLE_CAMERA_HEIGHT, "Enable Camera Height");

      if (IsHotkey(HK_VR_DECREASE_CAMERA_HEIGHT))
      {
        AdjustVRFloatSetting(Config::GFX_VR_CAMERA_HEIGHT, -Config::GFX_VR_CAMERA_HEIGHT_STEP,
                             Config::GFX_VR_CAMERA_HEIGHT_MIN, Config::GFX_VR_CAMERA_HEIGHT_MAX,
                             "Camera Height", 2);
      }
      if (IsHotkey(HK_VR_INCREASE_CAMERA_HEIGHT))
      {
        AdjustVRFloatSetting(Config::GFX_VR_CAMERA_HEIGHT, Config::GFX_VR_CAMERA_HEIGHT_STEP,
                             Config::GFX_VR_CAMERA_HEIGHT_MIN, Config::GFX_VR_CAMERA_HEIGHT_MAX,
                             "Camera Height", 2);
      }

      if (IsHotkey(HK_VR_TOGGLE_VIRTUAL_SCREEN))
        ToggleVRSetting(Config::GFX_VR_VIRTUAL_SCREEN, "Virtual Screen");

      if (IsHotkey(HK_VR_DECREASE_SCREEN_DISTANCE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_SCREEN_DISTANCE, -Config::GFX_VR_SCREEN_DISTANCE_STEP,
                             Config::GFX_VR_SCREEN_DISTANCE_MIN,
                             Config::GFX_VR_SCREEN_DISTANCE_MAX, "Screen Distance", 2);
      }
      if (IsHotkey(HK_VR_INCREASE_SCREEN_DISTANCE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_SCREEN_DISTANCE, Config::GFX_VR_SCREEN_DISTANCE_STEP,
                             Config::GFX_VR_SCREEN_DISTANCE_MIN,
                             Config::GFX_VR_SCREEN_DISTANCE_MAX, "Screen Distance", 2);
      }

      if (IsHotkey(HK_VR_DECREASE_SCREEN_SIZE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_SCREEN_SIZE, -Config::GFX_VR_SCREEN_SIZE_STEP,
                             Config::GFX_VR_SCREEN_SIZE_MIN, Config::GFX_VR_SCREEN_SIZE_MAX,
                             "Screen Size", 2);
      }
      if (IsHotkey(HK_VR_INCREASE_SCREEN_SIZE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_SCREEN_SIZE, Config::GFX_VR_SCREEN_SIZE_STEP,
                             Config::GFX_VR_SCREEN_SIZE_MIN, Config::GFX_VR_SCREEN_SIZE_MAX,
                             "Screen Size", 2);
      }

      if (IsHotkey(HK_VR_DECREASE_SCREEN_CURVATURE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                             -Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP,
                             Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                             Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX, "Screen Curvature", 2);
      }
      if (IsHotkey(HK_VR_INCREASE_SCREEN_CURVATURE))
      {
        AdjustVRFloatSetting(Config::GFX_VR_HEAD_LOCKED_CURVATURE,
                             Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP,
                             Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                             Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX, "Screen Curvature", 2);
      }

      if (IsHotkey(HK_VR_TOGGLE_DONT_CLEAR_SCREEN))
        ToggleVRSetting(Config::GFX_VR_DONT_CLEAR_SCREEN, "Don't Clear Screen");

      if (IsHotkey(HK_VR_TOGGLE_FORCE_VBI))
        ToggleVRForcedVBI();

      if (IsHotkey(HK_VR_TOGGLE_REMOVE_CINEMATIC_BARS))
        ToggleVRSetting(Config::GFX_VR_REMOVE_BARS, "Remove Cinematic Bars");

      if (IsHotkey(HK_VR_TOGGLE_CAMERA_ANCHOR))
        ToggleVRSetting(Config::GFX_VR_ENABLE_CAMERA_ANCHOR, "Camera Anchor");

      if (IsHotkey(HK_VR_TOGGLE_CONTROLLER_ANCHOR))
        ToggleVRSetting(Config::GFX_VR_ENABLE_CONTROLLER_ANCHOR, "Controller Anchor");

      // VR Shader Hunting
      auto& shader_hunter = ShaderHunter::GetInstance();
      static ShaderHunter::HandlingType shader_hotkey_handling = ShaderHunter::HandlingType::Skip;
      auto ShaderTypeLabel = [](ShaderHunter::ShaderType type) {
        switch (type)
        {
        case ShaderHunter::ShaderType::Pixel:
          return "Pixel";
        case ShaderHunter::ShaderType::Vertex:
          return "Vertex";
        case ShaderHunter::ShaderType::Geometry:
          return "Geometry";
        default:
          return "Unknown";
        }
      };
      auto HandlingTypeLabel = [](ShaderHunter::HandlingType type) {
        switch (type)
        {
        case ShaderHunter::HandlingType::Skip:
          return "Skip";
        case ShaderHunter::HandlingType::Screen:
          return "Screen";
        case ShaderHunter::HandlingType::ScreenPane:
          return "Screen Pane";
        case ShaderHunter::HandlingType::HeadLocked:
          return "Head Locked";
        case ShaderHunter::HandlingType::Fullscreen:
          return "Fullscreen";
        case ShaderHunter::HandlingType::FullscreenMono:
          return "Fullscreen";
        case ShaderHunter::HandlingType::Flag:
          return "Flag";
        case ShaderHunter::HandlingType::UnitsPerMeter:
          return "Units Per Meter";
        case ShaderHunter::HandlingType::Passthrough:
          return "Passthrough";
        case ShaderHunter::HandlingType::CameraAnchor:
          return "Camera Anchor";
        case ShaderHunter::HandlingType::ControllerAnchor:
          return "Controller Anchor";
        default:
          return "Unknown";
        }
      };
      auto ShowSelectedShader = [&shader_hunter, ShaderTypeLabel]() {
        const int pos = shader_hunter.GetSelectedPosition();
        const int total = shader_hunter.GetTotalCount();
        const u64 hash = shader_hunter.GetSelectedHash();
        if (pos < 0 || total <= 0 || hash == ~0ULL)
        {
          OSD::AddMessage(fmt::format("Shader: {} (none)",
                                      ShaderTypeLabel(shader_hunter.GetActiveType())));
          return;
        }

        OSD::AddMessage(fmt::format("Shader: {} {:08x} ({}/{})",
                                    ShaderTypeLabel(shader_hunter.GetActiveType()),
                                    static_cast<u32>(hash), pos + 1, total));
      };

      if (IsHotkey(HK_VR_SHADER_TOGGLE_HUNTING))
      {
        const bool enabled = !shader_hunter.IsEnabled();
        shader_hunter.SetEnabled(enabled);
        OSD::AddMessage(fmt::format("Shader Hunting: {}", enabled ? "Enabled" : "Disabled"));
      }

      if (IsHotkey(HK_VR_SHADER_CYCLE_HUNTING_OPTION))
      {
        const auto new_option = shader_hunter.GetHuntingOption() == ShaderHunter::HuntingOption::Skip ?
                                    ShaderHunter::HuntingOption::Pink :
                                    ShaderHunter::HuntingOption::Skip;
        shader_hunter.SetHuntingOption(new_option);
        OSD::AddMessage(fmt::format("Hunting Option: {}",
                                    new_option == ShaderHunter::HuntingOption::Pink ? "Pink" :
                                                                                       "Skip"));
      }

      if (IsHotkey(HK_VR_SHADER_CYCLE_TYPE))
      {
        const int next_type =
            (static_cast<int>(shader_hunter.GetActiveType()) + 1) %
            static_cast<int>(ShaderHunter::ShaderType::Count);
        shader_hunter.SetActiveType(static_cast<ShaderHunter::ShaderType>(next_type));
        OSD::AddMessage(
            fmt::format("Shader Type: {}", ShaderTypeLabel(shader_hunter.GetActiveType())));
      }

      if (IsHotkey(HK_VR_SHADER_CYCLE_HANDLING))
      {
        switch (shader_hotkey_handling)
        {
        case ShaderHunter::HandlingType::Skip:
          shader_hotkey_handling = ShaderHunter::HandlingType::Screen;
          break;
        case ShaderHunter::HandlingType::Screen:
          shader_hotkey_handling = ShaderHunter::HandlingType::HeadLocked;
          break;
        case ShaderHunter::HandlingType::ScreenPane:
          shader_hotkey_handling = ShaderHunter::HandlingType::HeadLocked;
          break;
        case ShaderHunter::HandlingType::HeadLocked:
          shader_hotkey_handling = ShaderHunter::HandlingType::Fullscreen;
          break;
        case ShaderHunter::HandlingType::Fullscreen:
          shader_hotkey_handling = ShaderHunter::HandlingType::Flag;
          break;
        case ShaderHunter::HandlingType::FullscreenMono:
          shader_hotkey_handling = ShaderHunter::HandlingType::Flag;
          break;
        case ShaderHunter::HandlingType::Flag:
          shader_hotkey_handling = ShaderHunter::HandlingType::UnitsPerMeter;
          break;
        case ShaderHunter::HandlingType::UnitsPerMeter:
          shader_hotkey_handling = ShaderHunter::HandlingType::Passthrough;
          break;
        case ShaderHunter::HandlingType::Passthrough:
          shader_hotkey_handling = ShaderHunter::HandlingType::CameraAnchor;
          break;
        case ShaderHunter::HandlingType::CameraAnchor:
          shader_hotkey_handling = ShaderHunter::HandlingType::ControllerAnchor;
          break;
        case ShaderHunter::HandlingType::ControllerAnchor:
        default:
          shader_hotkey_handling = ShaderHunter::HandlingType::Skip;
          break;
        }
        OSD::AddMessage(
            fmt::format("Override Handling: {}", HandlingTypeLabel(shader_hotkey_handling)));
      }

      if (IsHotkey(HK_VR_SHADER_PREV))
      {
        shader_hunter.PrevShader();
        ShowSelectedShader();
      }
      if (IsHotkey(HK_VR_SHADER_NEXT))
      {
        shader_hunter.NextShader();
        ShowSelectedShader();
      }

      if (IsHotkey(HK_VR_SHADER_PREV_TEXTURE_HASH))
      {
        shader_hunter.PrevTextureHash();
        const u64 texture_hash = shader_hunter.GetSelectedTextureHash();
        if (texture_hash == 0)
          OSD::AddMessage("Texture Hash: (none)");
        else
          OSD::AddMessage(
              fmt::format("Texture Hash: {:016x}", static_cast<unsigned long long>(texture_hash)));
      }
      if (IsHotkey(HK_VR_SHADER_NEXT_TEXTURE_HASH))
      {
        shader_hunter.NextTextureHash();
        const u64 texture_hash = shader_hunter.GetSelectedTextureHash();
        if (texture_hash == 0)
          OSD::AddMessage("Texture Hash: (none)");
        else
          OSD::AddMessage(
              fmt::format("Texture Hash: {:016x}", static_cast<unsigned long long>(texture_hash)));
      }

      if (IsHotkey(HK_VR_SHADER_TOGGLE_TEXTURE_HASH))
      {
        const bool enabled = shader_hunter.ToggleSelectedTextureHashFilter();
        const u64 texture_hash = shader_hunter.GetSelectedTextureHash();
        if (texture_hash == 0)
        {
          OSD::AddMessage("Texture Hash: (none)");
        }
        else
        {
          OSD::AddMessage(
              fmt::format("Texture Hash {}: {:016x}", enabled ? "added" : "removed",
                          static_cast<unsigned long long>(texture_hash)));
        }
      }

      if (IsHotkey(HK_VR_SHADER_SAVE_OVERRIDE))
      {
        const std::string game_id = SConfig::GetInstance().GetGameID();
        if (game_id.empty())
        {
          OSD::AddMessage("Save Shader Override: no game is running");
        }
        else if (shader_hunter.SaveSelectedShaderOverride(game_id, shader_hotkey_handling))
        {
          Settings::Instance().NotifyShaderOverridesChanged();
          OSD::AddMessage(fmt::format("Saved {} shader override to {}.ini",
                                      HandlingTypeLabel(shader_hotkey_handling), game_id));
        }
        else
        {
          OSD::AddMessage("Save Shader Override: no shader selected");
        }
      }

      // VR Texture Hunting
      auto& texture_hunter = TextureElementManager::GetInstance();
      static TextureElementManager::HandlingType texture_hotkey_handling =
          TextureElementManager::HandlingType::Skip;

      if (IsHotkey(HK_VR_TEXTURE_TOGGLE_HUNTING))
      {
        const bool enabled = !texture_hunter.IsHunterActive();
        texture_hunter.SetHunterActive(enabled);
        OSD::AddMessage(fmt::format("Texture Hunting: {}", enabled ? "Enabled" : "Disabled"));
      }

      if (IsHotkey(HK_VR_TEXTURE_CYCLE_HUNTING_OPTION))
      {
        const bool pink = !texture_hunter.IsPreviewPink();
        texture_hunter.SetPreviewPink(pink);
        OSD::AddMessage(fmt::format("Texture Hunting Option: {}", pink ? "Pink" : "Skip"));
      }

      if (IsHotkey(HK_VR_TEXTURE_CYCLE_HANDLING))
      {
        switch (texture_hotkey_handling)
        {
        case TextureElementManager::HandlingType::Skip:
          texture_hotkey_handling = TextureElementManager::HandlingType::Screen;
          break;
        case TextureElementManager::HandlingType::Screen:
          texture_hotkey_handling = TextureElementManager::HandlingType::HeadLocked;
          break;
        case TextureElementManager::HandlingType::HeadLocked:
          texture_hotkey_handling = TextureElementManager::HandlingType::Fullscreen;
          break;
        case TextureElementManager::HandlingType::Fullscreen:
        case TextureElementManager::HandlingType::FullscreenMono:
          texture_hotkey_handling = TextureElementManager::HandlingType::UnitsPerMeter;
          break;
        case TextureElementManager::HandlingType::UnitsPerMeter:
          texture_hotkey_handling = TextureElementManager::HandlingType::Passthrough;
          break;
        case TextureElementManager::HandlingType::Passthrough:
          texture_hotkey_handling = TextureElementManager::HandlingType::CameraAnchor;
          break;
        case TextureElementManager::HandlingType::CameraAnchor:
          texture_hotkey_handling = TextureElementManager::HandlingType::ControllerAnchor;
          break;
        case TextureElementManager::HandlingType::ControllerAnchor:
        case TextureElementManager::HandlingType::Flag:
        default:
          texture_hotkey_handling = TextureElementManager::HandlingType::Skip;
          break;
        }
        OSD::AddMessage(fmt::format("Texture Override Handling: {}",
                                    HandlingTypeLabel(texture_hotkey_handling)));
      }

      const auto ShowSelectedTexture = [&texture_hunter]() {
        const u64 hash = texture_hunter.GetSelectedTextureHash();
        if (hash == 0)
          OSD::AddMessage("Texture: (none)");
        else
          OSD::AddMessage(fmt::format("Texture: {:016x}", static_cast<unsigned long long>(hash)));
      };

      if (IsHotkey(HK_VR_TEXTURE_PREV))
      {
        texture_hunter.PrevTexture();
        ShowSelectedTexture();
      }
      if (IsHotkey(HK_VR_TEXTURE_NEXT))
      {
        texture_hunter.NextTexture();
        ShowSelectedTexture();
      }

      if (IsHotkey(HK_VR_TEXTURE_SAVE_OVERRIDE))
      {
        const std::string game_id = SConfig::GetInstance().GetGameID();
        if (game_id.empty())
        {
          OSD::AddMessage("Save Texture Override: no game is running");
        }
        else if (texture_hunter.SaveSelectedTextureOverride(game_id, texture_hotkey_handling))
        {
          Settings::Instance().NotifyTextureElementOverridesChanged();
          OSD::AddMessage(fmt::format("Saved {} texture override to {}.ini",
                                      HandlingTypeLabel(texture_hotkey_handling), game_id));
        }
        else
        {
          OSD::AddMessage("Save Texture Override: no texture selected");
        }
      }

      const bool throttle_hotkey_active = IsHotkey(HK_TOGGLE_THROTTLE, true);
      const bool finder_forces_turbo =
          CullingCodeFinder::GetInstance().IsTurboRuntimeOverrideActive();
      const bool finder_forces_audio_mute =
          CullingCodeFinder::GetInstance().IsAudioMuteRuntimeOverrideActive();
      const bool wants_temp_audio_mute =
          finder_forces_audio_mute ||
          (throttle_hotkey_active && Config::Get(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT));

      Core::SetIsThrottlerTempDisabled(throttle_hotkey_active || finder_forces_turbo);

      if (wants_temp_audio_mute && !Config::Get(Config::MAIN_AUDIO_MUTED))
      {
        Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);
        AudioCommon::UpdateSoundStream(system);
      }
      else if (!wants_temp_audio_mute && Config::Get(Config::MAIN_AUDIO_MUTED) &&
               Config::GetActiveLayerForConfig(Config::MAIN_AUDIO_MUTED) ==
                   Config::LayerType::CurrentRun)
      {
        Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
        AudioCommon::UpdateSoundStream(system);
      }

      auto ShowEmulationSpeed = [] {
        const float emulation_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
        if (!AchievementManager::GetInstance().IsHardcoreModeActive() ||
            Config::Get(Config::MAIN_EMULATION_SPEED) >= 1.0f ||
            Config::Get(Config::MAIN_EMULATION_SPEED) <= 0.0f)
        {
          OSD::AddMessage(emulation_speed <= 0 ? "Speed Limit: Unlimited" :
                                                 fmt::format("Speed Limit: {}%",
                                                             std::lround(emulation_speed * 100.f)));
        }
      };

      if (IsHotkey(HK_DECREASE_EMULATION_SPEED))
      {
        auto speed = Config::Get(Config::MAIN_EMULATION_SPEED) - 0.1;
        if (speed > 0)
        {
          speed = (speed >= 0.95 && speed <= 1.05) ? 1.0 : speed;
          Config::SetCurrent(Config::MAIN_EMULATION_SPEED, speed);
        }
        ShowEmulationSpeed();
      }

      if (IsHotkey(HK_INCREASE_EMULATION_SPEED))
      {
        auto speed = Config::Get(Config::MAIN_EMULATION_SPEED) + 0.1;
        speed = (speed >= 0.95 && speed <= 1.05) ? 1.0 : speed;
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, speed);
        ShowEmulationSpeed();
      }

      // USB Device Emulation
      if (IsHotkey(HK_SKYLANDERS_PORTAL))
        emit SkylandersPortalHotkey();

      if (IsHotkey(HK_INFINITY_BASE))
        emit InfinityBaseHotkey();

      // Slot Saving / Loading
      if (IsHotkey(HK_SAVE_STATE_SLOT_SELECTED))
        emit StateSaveSlotHotkey();

      if (IsHotkey(HK_LOAD_STATE_SLOT_SELECTED))
        emit StateLoadSlotHotkey();

      if (IsHotkey(HK_INCREMENT_SELECTED_STATE_SLOT))
        emit IncrementSelectedStateSlotHotkey();

      if (IsHotkey(HK_DECREMENT_SELECTED_STATE_SLOT))
        emit DecrementSelectedStateSlotHotkey();

      // Stereoscopy
      if (IsHotkey(HK_TOGGLE_STEREO_SIDE_BY_SIDE))
      {
        if (Config::Get(Config::GFX_STEREO_MODE) != StereoMode::SBS)
        {
          // Disable post-processing shader, as stereoscopy itself is currently a shader
          if (Config::Get(Config::GFX_ENHANCE_POST_SHADER) == DUBOIS_ALGORITHM_SHADER)
            Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, "");

          Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::SBS);
        }
        else
        {
          Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::Off);
        }
      }

      if (IsHotkey(HK_TOGGLE_STEREO_TAB))
      {
        if (Config::Get(Config::GFX_STEREO_MODE) != StereoMode::TAB)
        {
          // Disable post-processing shader, as stereoscopy itself is currently a shader
          if (Config::Get(Config::GFX_ENHANCE_POST_SHADER) == DUBOIS_ALGORITHM_SHADER)
            Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, "");

          Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::TAB);
        }
        else
        {
          Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::Off);
        }
      }

      if (IsHotkey(HK_TOGGLE_STEREO_ANAGLYPH))
      {
        if (Config::Get(Config::GFX_STEREO_MODE) != StereoMode::Anaglyph)
        {
          Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::Anaglyph);
          Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, DUBOIS_ALGORITHM_SHADER);
        }
        else
        {
          Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::Off);
          Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, "");
        }
      }

      CheckGBAHotkeys();
    }

    const auto stereo_depth = Config::Get(Config::GFX_STEREO_DEPTH);

    if (IsHotkey(HK_DECREASE_DEPTH, true))
      Config::SetCurrent(Config::GFX_STEREO_DEPTH, std::max(stereo_depth - 1, 0.0f));

    if (IsHotkey(HK_INCREASE_DEPTH, true))
      Config::SetCurrent(Config::GFX_STEREO_DEPTH,
                         std::min(stereo_depth + 1, Config::GFX_STEREO_DEPTH_MAXIMUM));

    const auto stereo_convergence = Config::Get(Config::GFX_STEREO_CONVERGENCE);

    if (IsHotkey(HK_DECREASE_CONVERGENCE, true))
      Config::SetCurrent(Config::GFX_STEREO_CONVERGENCE, std::max(stereo_convergence - 5, 0.0f));

    if (IsHotkey(HK_INCREASE_CONVERGENCE, true))
      Config::SetCurrent(Config::GFX_STEREO_CONVERGENCE,
                         std::min(stereo_convergence + 5, Config::GFX_STEREO_CONVERGENCE_MAXIMUM));

    // Free Look
    if (IsHotkey(HK_FREELOOK_TOGGLE))
    {
      const bool new_value = !Config::Get(Config::FREE_LOOK_ENABLED);
      Config::SetCurrent(Config::FREE_LOOK_ENABLED, new_value);

      const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
      if (hardcore)
        OSD::AddMessage("Free Look is Disabled in Hardcore Mode");
      else
        OSD::AddMessage(fmt::format("Free Look: {}", new_value ? "Enabled" : "Disabled"));
    }

    // Savestates
    for (u32 i = 0; i < State::NUM_STATES; i++)
    {
      if (IsHotkey(HK_LOAD_STATE_SLOT_1 + i))
        emit StateLoadSlot(i + 1);

      if (IsHotkey(HK_SAVE_STATE_SLOT_1 + i))
        emit StateSaveSlot(i + 1);

      if (IsHotkey(HK_LOAD_LAST_STATE_1 + i))
        emit StateLoadLastSaved(i + 1);

      if (IsHotkey(HK_SELECT_STATE_SLOT_1 + i))
        emit SetStateSlotHotkey(i + 1);
    }

    if (IsHotkey(HK_SAVE_FIRST_STATE))
      emit StateSaveOldest();

    if (IsHotkey(HK_UNDO_LOAD_STATE))
      emit StateLoadUndo();

    if (IsHotkey(HK_UNDO_SAVE_STATE))
      emit StateSaveUndo();

    if (IsHotkey(HK_LOAD_STATE_FILE))
      emit StateLoadFile();

    if (IsHotkey(HK_SAVE_STATE_FILE))
      emit StateSaveFile();
  }
}

void HotkeyScheduler::CheckDebuggingHotkeys()
{
  if (IsHotkey(HK_STEP))
    emit Step();

  if (IsHotkey(HK_STEP_OVER))
    emit StepOver();

  if (IsHotkey(HK_STEP_OUT))
    emit StepOut();

  if (IsHotkey(HK_SKIP))
    emit Skip();

  if (IsHotkey(HK_SHOW_PC))
    emit ShowPC();

  if (IsHotkey(HK_SET_PC))
    emit Skip();

  if (IsHotkey(HK_BP_TOGGLE))
    emit ToggleBreakpoint();

  if (IsHotkey(HK_BP_ADD))
    emit AddBreakpoint();
}

void HotkeyScheduler::CheckGBAHotkeys()
{
#ifdef HAS_LIBMGBA
  GBAWidget* gba_widget = qobject_cast<GBAWidget*>(QApplication::activeWindow());
  if (!gba_widget)
    return;

  if (IsHotkey(HK_GBA_LOAD))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->LoadROM(); });

  if (IsHotkey(HK_GBA_UNLOAD))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->UnloadROM(); });

  if (IsHotkey(HK_GBA_RESET))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->ResetCore(); });

  if (IsHotkey(HK_GBA_VOLUME_DOWN))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->VolumeDown(); });

  if (IsHotkey(HK_GBA_VOLUME_UP))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->VolumeUp(); });

  if (IsHotkey(HK_GBA_TOGGLE_MUTE))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->ToggleMute(); });

  if (IsHotkey(HK_GBA_1X))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->Resize(1); });

  if (IsHotkey(HK_GBA_2X))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->Resize(2); });

  if (IsHotkey(HK_GBA_3X))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->Resize(3); });

  if (IsHotkey(HK_GBA_4X))
    QueueOnObject(gba_widget, [gba_widget] { gba_widget->Resize(4); });
#endif
}
