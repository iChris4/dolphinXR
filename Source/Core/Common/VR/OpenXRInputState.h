// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace Common::VR
{
struct OpenXRPoseState
{
  bool valid = false;
  std::array<float, 3> position{};
  std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct OpenXRVelocityState
{
  bool linear_valid = false;
  bool angular_valid = false;
  std::array<float, 3> linear{};
  std::array<float, 3> angular{};
};

// Where the controller's aim ray intersects the VR virtual screen (the ortho 2D screen in
// immersive mode, or the flat panel quad in flat-screen mode). Computed by OpenXRManager on
// the frame thread with the exact same transform chain the renderer uses to place the screen,
// so u/v are an absolute "what the user is aiming at" — the Wii IR pointer maps to it 1:1.
struct OpenXRScreenHit
{
  bool valid = false;      // Aim ray hits the screen plane in front of the controller
  float u = 0.0f;          // -1..+1 across the screen width, +right (may exceed ±1 off-screen)
  float v = 0.0f;          // -1..+1 across the screen height, +up
  float distance_m = 0.0f; // Controller-to-screen distance along the aim ray, in meters
};

struct OpenXRControllerState
{
  bool connected = false;
  bool primary_button = false;
  bool secondary_button = false;
  bool menu_button = false;
  bool trigger_button = false;
  bool squeeze_button = false;
  bool thumbstick_button = false;
  float trigger_value = 0.0f;
  float squeeze_value = 0.0f;
  float squeeze_force = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  OpenXRPoseState aim_pose;
  // Aim pose at the input sample time (measured "now") instead of the predicted display time.
  // Pointing UIs ray-cast with it: prediction extrapolates fast wrist motion and sprays the ray.
  OpenXRPoseState pointer_pose;
  OpenXRPoseState grip_pose;
  OpenXRVelocityState grip_velocity;
  OpenXRScreenHit screen_hit;
};

// In-game VR menu state, decided on the input thread from the raw controller state.
struct OpenXRMenuInput
{
  bool open = false;
  // Game-facing input reads as neutral: the menu is open, or a button pressed while it was
  // open (including the one that closed it) is still held.
  bool input_withheld = false;
  // Increments each time the menu opens.
  uint32_t open_serial = 0;
  // Per hand (0 = left, 1 = right): monotonic counts of select presses/releases (trigger or
  // A/X) and the accumulated scroll in wheel steps. Readers diff them against their previous
  // read, so a click shorter than the reader's frame is never lost.
  std::array<uint32_t, 2> select_presses{};
  std::array<uint32_t, 2> select_releases{};
  std::array<float, 2> wheel_steps{};
};

struct OpenXRInputSnapshot
{
  std::array<OpenXRControllerState, 2> controllers{};
  OpenXRPoseState head_pose;  // HMD head orientation for IR pointer reference
  bool runtime_active = false;
  uint64_t generation = 0;
  std::array<std::string, 2> interaction_profiles;  // Active profile path per hand
  bool session_focused = false;
  // XrTime (nanoseconds) the poses/velocities were sampled at. Consumers differentiating
  // velocities should use deltas of this instead of wall-clock time. 0 when unavailable.
  int64_t sample_time_ns = 0;
  OpenXRMenuInput menu;
};

struct OpenXRHapticsState
{
  std::array<float, 2> amplitude{};
  // One-shot UI pulses (VR menu clicks), consumed by the input thread.
  std::array<float, 2> pulse_amplitude{};
  std::array<int64_t, 2> pulse_duration_ns{};
};

class OpenXRInputState final
{
public:
  // The raw controller state, for the VR menu, controller-bound rendering (Controller Anchor)
  // and diagnostics.
  static OpenXRInputSnapshot GetSnapshot()
  {
    std::lock_guard lk(s_state_mutex);
    return s_state;
  }

  // What the emulated controllers see. While the VR menu withholds input, buttons and sticks
  // read as released and the poses stay where they were when the menu opened, so motion reads
  // as at rest and the Wii pointer holds still.
  static OpenXRInputSnapshot GetGameSnapshot()
  {
    std::lock_guard lk(s_state_mutex);
    if (!s_state.menu.input_withheld)
      return s_state;

    OpenXRInputSnapshot snapshot = s_state;
    for (size_t hand = 0; hand < snapshot.controllers.size(); ++hand)
    {
      OpenXRControllerState& controller = snapshot.controllers[hand];
      const OpenXRControllerState& frozen = s_frozen_controllers[hand];
      controller.primary_button = false;
      controller.secondary_button = false;
      controller.menu_button = false;
      controller.trigger_button = false;
      controller.squeeze_button = false;
      controller.thumbstick_button = false;
      controller.trigger_value = 0.0f;
      controller.squeeze_value = 0.0f;
      controller.squeeze_force = 0.0f;
      controller.thumbstick_x = 0.0f;
      controller.thumbstick_y = 0.0f;
      controller.aim_pose = frozen.aim_pose;
      controller.pointer_pose = frozen.pointer_pose;
      controller.grip_pose = frozen.grip_pose;
      controller.grip_velocity = {};
      controller.screen_hit = frozen.screen_hit;
    }
    return snapshot;
  }

  static void SetControllers(const std::array<OpenXRControllerState, 2>& controllers,
                             bool runtime_active,
                             const OpenXRPoseState& head_pose = {})
  {
    std::lock_guard lk(s_state_mutex);
    s_state.controllers = controllers;
    s_state.head_pose = head_pose;
    s_state.runtime_active = runtime_active;
    ++s_state.generation;
  }

  static void SetControllers(const std::array<OpenXRControllerState, 2>& controllers,
                             bool runtime_active,
                             const OpenXRPoseState& head_pose,
                             const std::array<std::string, 2>& profiles,
                             bool focused,
                             int64_t sample_time_ns = 0,
                             const OpenXRMenuInput& menu = {})
  {
    std::lock_guard lk(s_state_mutex);
    // Freeze the last sample from before input was withheld: the sample that opened the
    // menu already carries the press.
    if (menu.input_withheld && !s_state.menu.input_withheld)
      s_frozen_controllers = s_state.controllers;
    s_state.menu = menu;
    s_state.controllers = controllers;
    s_state.head_pose = head_pose;
    s_state.runtime_active = runtime_active;
    s_state.interaction_profiles = profiles;
    s_state.session_focused = focused;
    s_state.sample_time_ns = sample_time_ns;
    ++s_state.generation;
  }

  static std::string GetDiagnosticString()
  {
    std::lock_guard lk(s_state_mutex);
    std::string result;
    result += s_state.session_focused ? "Session: FOCUSED\n" : "Session: NOT FOCUSED\n";
    for (int i = 0; i < 2; ++i)
    {
      const char* hand = i == 0 ? "Left" : "Right";
      const auto& profile = s_state.interaction_profiles[i];
      result += std::string(hand) + " Profile: " +
                (profile.empty() ? "<none>" : profile) + "\n";
      result += std::string(hand) + ": " +
                (s_state.controllers[i].connected ? "Connected" : "Not connected") + "\n";
    }
    return result;
  }

  static void Reset()
  {
    std::lock_guard lk(s_state_mutex);
    s_state = {};
    s_haptics = {};
    ++s_state.generation;
  }

  static OpenXRHapticsState GetHaptics()
  {
    std::lock_guard lk(s_state_mutex);
    return s_haptics;
  }

  static void SetRumble(float amplitude)
  {
    SetRumble(amplitude, amplitude);
  }

  static void SetRumble(float left_amplitude, float right_amplitude)
  {
    std::lock_guard lk(s_state_mutex);
    s_haptics.amplitude[0] = Clamp01(left_amplitude);
    s_haptics.amplitude[1] = Clamp01(right_amplitude);
  }

  static void SetRumbleForHand(std::size_t hand_index, float amplitude)
  {
    if (hand_index >= s_haptics.amplitude.size())
      return;

    std::lock_guard lk(s_state_mutex);
    s_haptics.amplitude[hand_index] = Clamp01(amplitude);
  }

  // Queue a single short vibration on one hand (UI feedback), independent of game rumble.
  static void PulseHaptic(std::size_t hand_index, float amplitude, int64_t duration_ns)
  {
    if (hand_index >= s_haptics.pulse_amplitude.size())
      return;

    std::lock_guard lk(s_state_mutex);
    s_haptics.pulse_amplitude[hand_index] = Clamp01(amplitude);
    s_haptics.pulse_duration_ns[hand_index] = duration_ns;
  }

  // Returns the haptics state and clears the queued one-shot pulses.
  static OpenXRHapticsState TakeHaptics()
  {
    std::lock_guard lk(s_state_mutex);
    const OpenXRHapticsState haptics = s_haptics;
    s_haptics.pulse_amplitude = {};
    s_haptics.pulse_duration_ns = {};
    return haptics;
  }

private:
  static float Clamp01(float value)
  {
    if (value < 0.0f)
      return 0.0f;
    if (value > 1.0f)
      return 1.0f;
    return value;
  }

  static inline std::mutex s_state_mutex;
  static inline OpenXRInputSnapshot s_state{};
  static inline std::array<OpenXRControllerState, 2> s_frozen_controllers{};
  static inline OpenXRHapticsState s_haptics{};
};
}  // namespace Common::VR

#endif  // ENABLE_VR
