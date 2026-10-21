// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <optional>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#endif

class InputConfig;
class PointerWrap;

namespace ControllerEmu
{
class ControlGroup;
}

namespace WiimoteEmu
{
enum class WiimoteGroup;
enum class NunchukGroup;
enum class ClassicGroup;
enum class GuitarGroup;
enum class DrumsGroup;
enum class TurntableGroup;
enum class UDrawTabletGroup;
enum class DrawsomeTabletGroup;
enum class TaTaConGroup;
enum class ShinkansenGroup;
}  // namespace WiimoteEmu

enum
{
  WIIMOTE_CHAN_0 = 0,
  WIIMOTE_CHAN_1,
  WIIMOTE_CHAN_2,
  WIIMOTE_CHAN_3,
  WIIMOTE_BALANCE_BOARD,
  MAX_WIIMOTES = WIIMOTE_BALANCE_BOARD,
  MAX_BBMOTES = 5,
};

#define WIIMOTE_INI_NAME "WiimoteNew"

enum class WiimoteSource
{
  None = 0,
  Emulated = 1,
  Real = 2,
  OpenXR = 3,
};

namespace WiimoteCommon
{
class HIDWiimote;

// Used to reconnect WiimoteDevice instance to HID source.
// Must be run from CPU thread.
void UpdateSource(unsigned int index);

HIDWiimote* GetHIDWiimoteSource(unsigned int index);

}  // namespace WiimoteCommon

namespace Wiimote
{
enum class InitializeMode
{
  DO_WAIT_FOR_WIIMOTES,
  DO_NOT_WAIT_FOR_WIIMOTES,
};

// The Real Wii Remote sends report every ~5ms (200 Hz).
constexpr int UPDATE_FREQ = 200;

struct OpenXRWiiRemoteState
{
  std::array<float, 3> acceleration{};
  std::array<float, 3> angular_velocity{};
  float ir_x = 0.0f;
  float ir_y = 0.0f;
  bool ir_visible = false;
};

void Shutdown();
void Initialize(InitializeMode init_mode);
void ResetAllWiimotes();
void LoadConfig();
void GenerateDynamicInputTextures();
void Resume();
void Pause();
std::optional<OpenXRWiiRemoteState> GetOpenXRWiiRemoteState(unsigned int index);
std::optional<OpenXRWiiRemoteState> GetOpenXRHandState(bool left_hand);
#ifdef ENABLE_VR
// The OpenXR source's input override is installed on this remote and its attachments (it also
// returns the agent server's input). Hold EmulatedController::GetStateLock().
bool IsOpenXRInputOverrideInstalled(unsigned int index);

// The pose of the VR controller that drives an OpenXR Wii Remote (the same hand and motion
// orientation the input override uses: the aim at the input sample time pitched by
// GFX_VR_WIIMOTE_MOTION_PITCH), in Dolphin's Wii Remote frames: body +X left, +Y backward, +Z up,
// and a world with +Z up whose -Y axis is OpenXR's forward (-Z).
struct OpenXRWiiRemotePose
{
  // Device to world: v_world = orientation * v_device.
  Common::Quaternion orientation = Common::Quaternion::Identity();
  bool screen_distance_valid = false;
  // Distance from the controller to the virtual screen along the aim ray, in meters.
  float screen_distance_m = 0.0f;
  bool left_hand = false;
  // The runtime data it came from, for the Motion Tester's recordings.
  Common::VR::OpenXRControllerState controller;
  u64 generation = 0;
  s64 sample_time_ns = 0;
  bool input_withheld = false;
};
std::optional<OpenXRWiiRemotePose> GetOpenXRWiiRemotePose(unsigned int index);

// What the OpenXR input override last fed the emulated remote: acceleration in m/s² along the
// body axes, angular velocity in rad/s about the MotionPlus axes, and the IR cursor (ir_x is NaN
// while the pointer is hidden).
struct OpenXRWiiRemoteInput
{
  u64 generation = 0;
  Common::Vec3 acceleration{};
  Common::Vec3 angular_velocity{};
  float ir_x = 0.0f;
  float ir_y = 0.0f;
  float ir_z = 0.0f;
};
std::optional<OpenXRWiiRemoteInput> GetOpenXRWiiRemoteInput(unsigned int index);

// The angular velocity about the body axes (the MotionPlus axes) that turns the device-to-world
// rotation `from` into `to` in `seconds`.
Common::Vec3 GetOpenXRBodyAngularVelocity(const Common::Quaternion& from,
                                          const Common::Quaternion& to, float seconds);

// A MotionPlus game turns the gyroscope into an orientation by integrating it over the time
// between reports. The pose's angular velocity alone lets that integral drift away from the
// controller (poses arrive at the game's frame rate on PC, emulation runs in bursts), so the
// reported rate also steers a model of the integral toward the latest pose.
struct OpenXRGyroSteering
{
  // The gyroscope to report `seconds` after the previous report: pose is the latest
  // device-to-world pose and pose_rate its angular velocity, both in body axes.
  Common::Vec3 Step(const Common::Quaternion& pose, const Common::Vec3& pose_rate, double seconds);

  // The integral's error is removed with this time constant, at most this fast (rad/s).
  static constexpr float STEERING_SECONDS = 0.2f;
  static constexpr float MAX_STEERING_SPEED = 4.0f;
  // Longer gaps (pause, savestate load) restart the integral at the pose.
  static constexpr double MAX_STEP_SECONDS = 0.1;

  bool valid = false;
  Common::Quaternion integrated = Common::Quaternion::Identity();
  Common::Vec3 rate{};
  u32 reports = 0;
};

// One controller sample of the OpenXR input override: the orientation in the Wii Remote's
// frames (device to world) and the angular velocity about its body axes; the grip position and
// velocity in the OpenXR reference space.
struct OpenXRPoseSample
{
  s64 time_ns = 0;
  Common::Quaternion orientation = Common::Quaternion::Identity();
  Common::Vec3 angular_velocity{};
  bool has_position = false;
  Common::Vec3 position{};
  bool has_velocity = false;
  Common::Vec3 velocity{};
};

// The last samples, for differences over a fixed time window. The input thread polls every few
// milliseconds, and the difference between two consecutive polls amplifies the tracking noise
// (the runtime does not even advance the pose on every poll).
struct OpenXRPoseHistory
{
  static constexpr size_t CAPACITY = 64;

  void Push(const OpenXRPoseSample& sample);
  void Clear();
  // The newest sample at least age_ns older than time_ns. Until the history reaches that far
  // back, the oldest sample if it is at least half that old, otherwise nullptr.
  const OpenXRPoseSample* FindOlderThan(s64 time_ns, s64 age_ns) const;

  std::array<OpenXRPoseSample, CAPACITY> samples{};
  size_t count = 0;
  size_t next = 0;
};
// An OpenXR rotation (X right, Y up, Z back) expressed in the frames above.
Common::Quaternion OpenXRToWiimoteRotation(const Common::Quaternion& openxr_rotation);
#endif

void DoState(PointerWrap& p);
InputConfig* GetConfig();
ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group);
ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group);
ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group);
ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group);
ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group);
ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group);
ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group);
ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group);
ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group);
ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group);
}  // namespace Wiimote

namespace WiimoteReal
{
void Initialize(::Wiimote::InitializeMode init_mode);
void Stop();
void Shutdown();
void Resume();
void Pause();
void Refresh();

}  // namespace WiimoteReal
