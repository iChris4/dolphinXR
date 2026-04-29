// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "jni/Input/HotkeyDispatcher.h"

#include <algorithm>
#include <sys/resource.h>
#include <unistd.h>

#include "Common/Config/Config.h"
#include "Common/Thread.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HotkeyManager.h"
#include "Core/State.h"
#include "Core/System.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"


#ifdef ENABLE_VR
#include "VideoCommon/VR/OpenXRManager.h"
#endif

namespace
{
constexpr int kPollPeriodMs = 11;  // ~90 Hz, matches Quest 3 refresh — see plan §"Threading model".
constexpr int kBackgroundNice = 10;
// Number of consecutive 11 ms ticks HK_ANDROID_RETURN_TO_MAIN_MENU must be held before firing
// (~3 seconds, same threshold as the old hard-coded OpenXRManager long-press).
constexpr int kReturnMenuHoldTicks = 3000 / kPollPeriodMs;

bool IsHotkey(int id, bool held = false)
{
  return HotkeyManagerEmu::IsPressed(id, held);
}
}  // namespace

HotkeyDispatcher::~HotkeyDispatcher()
{
  Stop();
}

void HotkeyDispatcher::Start(StopRequestCallback on_stop_requested)
{
  if (m_thread.joinable())
    return;

  m_on_stop_requested = std::move(on_stop_requested);
  m_stop_requested.Clear();

  // HotkeyManagerEmu starts disabled on Android (PC's HotkeyScheduler ctor enables it,
  // but no equivalent runs here). Without this, IsPressed() short-circuits and the loop
  // is a silent no-op — flagged in the design review.
  HotkeyManagerEmu::Enable(true);

  m_thread = std::thread(&HotkeyDispatcher::Run, this);
}

void HotkeyDispatcher::Stop()
{
  if (!m_thread.joinable())
    return;

  m_stop_requested.Set();
  m_thread.join();
  m_on_stop_requested = {};
}

void HotkeyDispatcher::Run()
{
  Common::SetCurrentThreadName("HotkeyDispatcher");

  // PRIO_PROCESS + tid is the Linux/Android idiom for a per-thread nice value (despite
  // PRIO_PROCESS's name). Best-effort — failure is benign, the thread just runs at the
  // default priority.
  setpriority(PRIO_PROCESS, gettid(), kBackgroundNice);

  Core::System& system = Core::System::GetInstance();

  while (!m_stop_requested.IsSet())
  {
    Common::SleepCurrentThread(kPollPeriodMs);

    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
    g_controller_interface.UpdateInput();

    if (!HotkeyManagerEmu::IsEnabled())
      continue;

    const Core::State state = Core::GetState(system);
    if (state == Core::State::Stopping || state == Core::State::Uninitialized)
      continue;

    // Fill the rising-edge state for the next IsPressed() calls. PC always passes
    // require_focus=true here; on Android we have no window-focus concept while the
    // EmulationActivity is foreground, so we always allow input.
    Core::UpdateInputGate(true);
    HotkeyManagerEmu::GetStatus(false);
    ControlReference::SetInputGate(true);
    HotkeyManagerEmu::GetStatus(true);

    // ---------- Core emulation ----------

    if (IsHotkey(HK_PLAY_PAUSE))
    {
      const Core::State current = Core::GetState(system);
      if (current == Core::State::Running)
        Core::SetState(system, Core::State::Paused);
      else if (current == Core::State::Paused)
        Core::SetState(system, Core::State::Running);
    }

    if (IsHotkey(HK_STOP))
    {
      {
        Core::Stop(system);
      }
      // Wake the Run() wait loop so it observes !Core::IsRunning and exits, which then
      // triggers finishEmulationActivity() (and the previous task's MainActivity-revival
      // fix). The next loop iteration will see the resulting state == Stopping and bail
      // out of the dispatch block, but we still want to keep polling until Stop() is
      // called on us so that no spurious actions fire mid-shutdown.
      if (m_on_stop_requested)
        m_on_stop_requested();
      continue;
    }

    if (IsHotkey(HK_RESET))
    {
      system.GetProcessorInterface().ResetButton_Tap();
    }

    if (IsHotkey(HK_SCREENSHOT))
    {
      Core::SaveScreenShot();
    }

    if (IsHotkey(HK_FRAME_ADVANCE))
    {
      Core::DoFrameStep(system);
    }

    // Held hotkey: throttle is disabled while the bound input is held.
    {
      const bool throttle_held = IsHotkey(HK_TOGGLE_THROTTLE, true);
      Core::SetIsThrottlerTempDisabled(throttle_held);
    }

    if (IsHotkey(HK_DECREASE_EMULATION_SPEED))
    {
      auto speed = Config::Get(Config::MAIN_EMULATION_SPEED) - 0.1f;
      if (speed > 0.0f)
      {
        if (speed >= 0.95f && speed <= 1.05f)
          speed = 1.0f;
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, speed);
      }
    }
    if (IsHotkey(HK_INCREASE_EMULATION_SPEED))
    {
      auto speed = Config::Get(Config::MAIN_EMULATION_SPEED) + 0.1f;
      if (speed >= 0.95f && speed <= 1.05f)
        speed = 1.0f;
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, speed);
    }

    // Save/Load state slots 1..10. The HK_*_STATE_SLOT_1 hotkey is slot 1 (1-indexed)
    // but State::Save/Load take 0-based slot numbers.
    for (int slot = 1; slot <= 10; ++slot)
    {
      if (IsHotkey(HK_LOAD_STATE_SLOT_1 + (slot - 1)))
      {
        State::Load(system, slot);
      }
      if (IsHotkey(HK_SAVE_STATE_SLOT_1 + (slot - 1)))
      {
        State::Save(system, slot);
      }
    }

    // ---------- VR ----------

    if (IsHotkey(HK_VR_TOGGLE_OPENXR))
    {
      Config::SetCurrent(Config::GFX_VR_ENABLE_OPENXR,
                         !Config::Get(Config::GFX_VR_ENABLE_OPENXR));
    }

#ifdef ENABLE_VR
    if (IsHotkey(HK_VR_RESET_POSITION))
    {
      if (VR::g_openxr)
        VR::g_openxr->RequestRecenter();
    }
#endif

    if (IsHotkey(HK_VR_DECREASE_UNITS_PER_METER))
    {
      const float v = std::max(Config::GFX_VR_UNITS_PER_METER_MIN,
                               Config::Get(Config::GFX_VR_UNITS_PER_METER) -
                                   Config::GFX_VR_UNITS_PER_METER_STEP);
      Config::SetCurrent(Config::GFX_VR_UNITS_PER_METER, v);
    }
    if (IsHotkey(HK_VR_INCREASE_UNITS_PER_METER))
    {
      const float v = std::min(Config::GFX_VR_UNITS_PER_METER_MAX,
                               Config::Get(Config::GFX_VR_UNITS_PER_METER) +
                                   Config::GFX_VR_UNITS_PER_METER_STEP);
      Config::SetCurrent(Config::GFX_VR_UNITS_PER_METER, v);
    }

    if (IsHotkey(HK_VR_DECREASE_LEAN_BACK_ANGLE))
    {
      const float v = std::max(Config::GFX_VR_LEAN_BACK_ANGLE_MIN,
                               Config::Get(Config::GFX_VR_LEAN_BACK_ANGLE) -
                                   Config::GFX_VR_LEAN_BACK_ANGLE_STEP);
      Config::SetCurrent(Config::GFX_VR_LEAN_BACK_ANGLE, v);
    }
    if (IsHotkey(HK_VR_INCREASE_LEAN_BACK_ANGLE))
    {
      const float v = std::min(Config::GFX_VR_LEAN_BACK_ANGLE_MAX,
                               Config::Get(Config::GFX_VR_LEAN_BACK_ANGLE) +
                                   Config::GFX_VR_LEAN_BACK_ANGLE_STEP);
      Config::SetCurrent(Config::GFX_VR_LEAN_BACK_ANGLE, v);
    }

    if (IsHotkey(HK_VR_TOGGLE_ENABLE_CAMERA_FORWARD))
    {
      Config::SetCurrent(Config::GFX_VR_ENABLE_CAMERA_FORWARD,
                         !Config::Get(Config::GFX_VR_ENABLE_CAMERA_FORWARD));
    }
    if (IsHotkey(HK_VR_DECREASE_CAMERA_FORWARD))
    {
      const float v = std::max(Config::GFX_VR_CAMERA_FORWARD_MIN,
                               Config::Get(Config::GFX_VR_CAMERA_FORWARD) -
                                   Config::GFX_VR_CAMERA_FORWARD_STEP);
      Config::SetCurrent(Config::GFX_VR_CAMERA_FORWARD, v);
    }
    if (IsHotkey(HK_VR_INCREASE_CAMERA_FORWARD))
    {
      const float v = std::min(Config::GFX_VR_CAMERA_FORWARD_MAX,
                               Config::Get(Config::GFX_VR_CAMERA_FORWARD) +
                                   Config::GFX_VR_CAMERA_FORWARD_STEP);
      Config::SetCurrent(Config::GFX_VR_CAMERA_FORWARD, v);
    }

    if (IsHotkey(HK_VR_TOGGLE_ENABLE_CAMERA_HEIGHT))
    {
      Config::SetCurrent(Config::GFX_VR_ENABLE_CAMERA_HEIGHT,
                         !Config::Get(Config::GFX_VR_ENABLE_CAMERA_HEIGHT));
    }
    if (IsHotkey(HK_VR_DECREASE_CAMERA_HEIGHT))
    {
      const float v = std::max(Config::GFX_VR_CAMERA_HEIGHT_MIN,
                               Config::Get(Config::GFX_VR_CAMERA_HEIGHT) -
                                   Config::GFX_VR_CAMERA_HEIGHT_STEP);
      Config::SetCurrent(Config::GFX_VR_CAMERA_HEIGHT, v);
    }
    if (IsHotkey(HK_VR_INCREASE_CAMERA_HEIGHT))
    {
      const float v = std::min(Config::GFX_VR_CAMERA_HEIGHT_MAX,
                               Config::Get(Config::GFX_VR_CAMERA_HEIGHT) +
                                   Config::GFX_VR_CAMERA_HEIGHT_STEP);
      Config::SetCurrent(Config::GFX_VR_CAMERA_HEIGHT, v);
    }

    if (IsHotkey(HK_VR_TOGGLE_VIRTUAL_SCREEN))
    {
      Config::SetCurrent(Config::GFX_VR_VIRTUAL_SCREEN,
                         !Config::Get(Config::GFX_VR_VIRTUAL_SCREEN));
    }
    if (IsHotkey(HK_VR_DECREASE_SCREEN_DISTANCE))
    {
      const float v = std::max(Config::GFX_VR_SCREEN_DISTANCE_MIN,
                               Config::Get(Config::GFX_VR_SCREEN_DISTANCE) -
                                   Config::GFX_VR_SCREEN_DISTANCE_STEP);
      Config::SetCurrent(Config::GFX_VR_SCREEN_DISTANCE, v);
    }
    if (IsHotkey(HK_VR_INCREASE_SCREEN_DISTANCE))
    {
      const float v = std::min(Config::GFX_VR_SCREEN_DISTANCE_MAX,
                               Config::Get(Config::GFX_VR_SCREEN_DISTANCE) +
                                   Config::GFX_VR_SCREEN_DISTANCE_STEP);
      Config::SetCurrent(Config::GFX_VR_SCREEN_DISTANCE, v);
    }
    if (IsHotkey(HK_VR_DECREASE_SCREEN_SIZE))
    {
      const float v = std::max(Config::GFX_VR_SCREEN_SIZE_MIN,
                               Config::Get(Config::GFX_VR_SCREEN_SIZE) -
                                   Config::GFX_VR_SCREEN_SIZE_STEP);
      Config::SetCurrent(Config::GFX_VR_SCREEN_SIZE, v);
    }
    if (IsHotkey(HK_VR_INCREASE_SCREEN_SIZE))
    {
      const float v = std::min(Config::GFX_VR_SCREEN_SIZE_MAX,
                               Config::Get(Config::GFX_VR_SCREEN_SIZE) +
                                   Config::GFX_VR_SCREEN_SIZE_STEP);
      Config::SetCurrent(Config::GFX_VR_SCREEN_SIZE, v);
    }
    if (IsHotkey(HK_VR_DECREASE_SCREEN_CURVATURE))
    {
      const float v = std::max(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MIN,
                               Config::Get(Config::GFX_VR_HEAD_LOCKED_CURVATURE) -
                                   Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP);
      Config::SetCurrent(Config::GFX_VR_HEAD_LOCKED_CURVATURE, v);
    }
    if (IsHotkey(HK_VR_INCREASE_SCREEN_CURVATURE))
    {
      const float v = std::min(Config::GFX_VR_HEAD_LOCKED_CURVATURE_MAX,
                               Config::Get(Config::GFX_VR_HEAD_LOCKED_CURVATURE) +
                                   Config::GFX_VR_HEAD_LOCKED_CURVATURE_STEP);
      Config::SetCurrent(Config::GFX_VR_HEAD_LOCKED_CURVATURE, v);
    }

    if (IsHotkey(HK_VR_TOGGLE_DONT_CLEAR_SCREEN))
    {
      Config::SetCurrent(Config::GFX_VR_DONT_CLEAR_SCREEN,
                         !Config::Get(Config::GFX_VR_DONT_CLEAR_SCREEN));
    }
    if (IsHotkey(HK_VR_TOGGLE_FORCE_VBI))
    {
      const int current_value =
          Config::NormalizeVRForcedVBIFrequency(Config::Get(Config::GFX_VR_FORCED_VBI_FREQUENCY));
      Config::SetCurrent(Config::GFX_VR_AUTO_VBI_FROM_HMD, false);
      Config::SetCurrent(Config::GFX_VR_FORCED_VBI_FREQUENCY,
                         current_value == Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF ?
                             Config::GFX_VR_FORCED_VBI_FREQUENCY_AUTO :
                             Config::GFX_VR_FORCED_VBI_FREQUENCY_OFF);
    }
    if (IsHotkey(HK_VR_TOGGLE_REMOVE_CINEMATIC_BARS))
    {
      Config::SetCurrent(Config::GFX_VR_REMOVE_BARS,
                         !Config::Get(Config::GFX_VR_REMOVE_BARS));
    }

    // ---------- Android navigation ----------

    // HK_ANDROID_RETURN_TO_MAIN_MENU requires a sustained 3-second hold before firing.
    // This prevents accidental exits on shared inputs (e.g. a shoulder button also used for
    // in-game actions). Once the threshold is reached we fire exactly once; the tick counter
    // is reset to a saturated value so it doesn't re-fire until the button is released.
    if (IsHotkey(HK_ANDROID_RETURN_TO_MAIN_MENU, true))
    {
      if (m_return_menu_hold_ticks < kReturnMenuHoldTicks)
        ++m_return_menu_hold_ticks;

      if (m_return_menu_hold_ticks == kReturnMenuHoldTicks)
      {
        {
          Core::Stop(system);
        }
        if (m_on_stop_requested)
          m_on_stop_requested();
        m_return_menu_hold_ticks = kReturnMenuHoldTicks + 1;
        continue;
      }
    }
    else
    {
      m_return_menu_hold_ticks = 0;
    }
  }
}
