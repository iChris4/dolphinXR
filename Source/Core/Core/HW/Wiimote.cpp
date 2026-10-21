// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Wiimote.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#include "Core/AgentServer/AgentInput.h"
#include "Core/Config/GraphicsSettings.h"
#endif

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"

// Limit the amount of wiimote connect requests, when a button is pressed in disconnected state
static std::array<u8, MAX_BBMOTES> s_last_connect_request_counter;

namespace
{
static std::array<std::atomic<WiimoteSource>, MAX_BBMOTES> s_wiimote_sources;
static std::optional<Config::ConfigChangedCallbackID> s_config_callback_id = std::nullopt;

bool IsEmulatedSource(WiimoteSource source)
{
  return source == WiimoteSource::Emulated || source == WiimoteSource::OpenXR;
}

#ifdef ENABLE_VR
Common::Quaternion ToQuaternion(const std::array<float, 4>& quat)
{
  return {quat[3], quat[0], quat[1], quat[2]};
}

Common::Vec3 ToVec3(const std::array<float, 3>& vec)
{
  return {vec[0], vec[1], vec[2]};
}

struct OpenXRWiimoteState
{
  u64 generation = std::numeric_limits<u64>::max();
  Common::Vec3 acceleration{0.0f, 0.0f, float(MathUtil::GRAVITY_ACCELERATION)};
  // The aim at the input sample time in the Wii Remote's frames (device to world), and its
  // angular velocity about the MotionPlus axes since the previous snapshot.
  bool orientation_valid = false;
  Common::Quaternion orientation = Common::Quaternion::Identity();
  Common::Vec3 angular_velocity{};
  float ir_x = std::numeric_limits<float>::quiet_NaN();
  float ir_y = 0.0f;
  float ir_z = 0.0f;  // Forward/backward distance offset from NEUTRAL_DISTANCE, in meters
};

// What each remote's OpenXR override last fed it, for the Motion Tester's recordings.
struct PublishedOpenXRInput
{
  std::mutex mutex;
  std::optional<Wiimote::OpenXRWiiRemoteInput> input;
};
std::array<PublishedOpenXRInput, MAX_BBMOTES> s_openxr_inputs;

void PublishOpenXRInput(unsigned int index, const OpenXRWiimoteState* state,
                        const Common::Vec3& angular_velocity = {})
{
  std::optional<Wiimote::OpenXRWiiRemoteInput> input;
  if (state)
  {
    input = Wiimote::OpenXRWiiRemoteInput{state->generation, state->acceleration, angular_velocity,
                                          state->ir_x,       state->ir_y,         state->ir_z};
  }
  std::lock_guard lock(s_openxr_inputs[index].mutex);
  s_openxr_inputs[index].input = input;
}

// How far past the screen edge (in screen half-extents) the pointer stays tracked before
// the emulated IR camera "loses" it, matching real hardware: the wiimote camera FOV
// (42°x31.5°) extends well past the cursor's Total Yaw/Pitch range (25°/20°), so real
// dots survive to roughly these excursions.
constexpr float OPENXR_IR_HIDE_MARGIN_U = 1.9f;
constexpr float OPENXR_IR_HIDE_MARGIN_V = 1.5f;
// An off-screen excursion must persist this long before the pointer is hidden. Runtime pose
// spikes during fast controller motion produce brief excursions; without the debounce every
// spike dropped IR tracking entirely.
constexpr s64 OPENXR_IR_HIDE_DELAY_NS = 100'000'000;
// Snapshots further apart than this (a stall, a hand switch) give no angular velocity.
constexpr float OPENXR_MAX_POSE_RATE_INTERVAL = 0.1f;
// Radians per second, above the emulated MotionPlus's fast range (2259 deg/s).
constexpr float OPENXR_MAX_POSE_ANGULAR_SPEED = 45.0f;
// The accelerometer and the gyroscope are differences of the controller samples over these
// windows (BuildOpenXRState). Per-frame polling (30-90 Hz) samples less often than either, so
// it differences consecutive samples as before; the 250 Hz input thread averages its noise.
constexpr s64 OPENXR_ACCEL_WINDOW_NS = 32'000'000;
constexpr s64 OPENXR_GYRO_WINDOW_NS = 16'000'000;
// How far in front of the grip position the emulated accelerometer sits, along the remote. A
// real Wii Remote's accelerometer is that far in front of the palm, so a swing flings it
// outward: the backward (centripetal) push of several g during the swing and the sideways kick
// when it starts and stops, which MotionPlus games read as a swing with a direction, as opposed
// to a thrust. Fitted on a real remote's Skyward Sword swings.
constexpr float OPENXR_ACCELEROMETER_LEVER_M = 0.05f;

// The remote's orientation in the hand: the aim at the input sample time (the pointer pose, the
// frame the IR pointer uses) pitched nose-up by GFX_VR_WIIMOTE_MOTION_PITCH. A Touch
// controller's aim ray points below where a remote held in the same hand points; measured
// against a real remote's Skyward Sword swings, the swing axis then sits 14 degrees from the
// remote's yaw axis instead of 24 (real remote: 13), which is the difference between the game
// reading a horizontal slash and a diagonal one.
Common::Quaternion GetMotionOrientation(const Common::VR::OpenXRControllerState& controller,
                                        float pitch_degrees)
{
  const Common::VR::OpenXRPoseState& pose = controller.pointer_pose.valid ?
                                                controller.pointer_pose :
                                            controller.aim_pose.valid ? controller.aim_pose :
                                                                        controller.grip_pose;
  // Positive about the aim's +X (right) tilts its up axis backward: nose up.
  return (ToQuaternion(pose.orientation).Normalized() *
          Common::Quaternion::RotateX(pitch_degrees * float(MathUtil::TAU / 360)))
      .Normalized();
}

float MotionPitchDegrees()
{
  return Config::Get(Config::GFX_VR_WIIMOTE_MOTION_PITCH);
}

// Nanosecond timestamp used for velocity differentiation: the XR pose sample time when
// available (jitter-free), wall clock otherwise.
s64 OpenXRSampleTimeNs(const Common::VR::OpenXRInputSnapshot& snapshot)
{
  if (snapshot.sample_time_ns != 0)
    return snapshot.sample_time_ns;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

OpenXRWiimoteState BuildOpenXRState(const Common::VR::OpenXRControllerState& controller,
                                    Wiimote::OpenXRPoseHistory* history, s64 sample_time_ns,
                                    float motion_pitch_degrees)
{
  OpenXRWiimoteState out;

  const bool has_grip_pose = controller.grip_pose.valid;
  const bool has_aim_pose = controller.aim_pose.valid;
  if (!has_grip_pose && !has_aim_pose)
    return out;

  const Common::Quaternion reference_orientation =
      GetMotionOrientation(controller, motion_pitch_degrees);
  const Common::Matrix33 world_to_local =
      Common::Matrix33::FromQuaternion(reference_orientation).Transposed();

  const auto get_matrix = [&world_to_local](int row, int col) { return world_to_local.data[row * 3 + col]; };

  Wiimote::OpenXRPoseSample sample;
  sample.time_ns = sample_time_ns;
  sample.orientation = Wiimote::OpenXRToWiimoteRotation(reference_orientation);
  if (has_grip_pose)
  {
    sample.has_position = true;
    sample.position = ToVec3(controller.grip_pose.position);
  }
  const auto seconds_since = [sample_time_ns](const Wiimote::OpenXRPoseSample& reference) {
    return float(sample_time_ns - reference.time_ns) * 1e-9f;
  };
  const auto usable = [&seconds_since](const Wiimote::OpenXRPoseSample* reference) {
    return reference && seconds_since(*reference) > 0.001f &&
           seconds_since(*reference) < OPENXR_MAX_POSE_RATE_INTERVAL;
  };

  // The gyroscope comes from the change of orientation over the gyroscope window, not from the
  // runtime's angular velocity: the spec puts that in the reference space, but Meta's runtime
  // reports it in a controller-local frame (measured with the Motion Tester's recordings), and
  // rotated as world-space it made the game's MotionPlus orientation diverge within seconds.
  out.orientation = sample.orientation;
  out.orientation_valid = true;
  const Wiimote::OpenXRPoseSample* const gyro_reference =
      history->FindOlderThan(sample_time_ns, OPENXR_GYRO_WINDOW_NS);
  Common::Vec3 angular_acceleration{};
  if (usable(gyro_reference))
  {
    const float dt = seconds_since(*gyro_reference);
    const Common::Vec3 angular_velocity =
        Wiimote::GetOpenXRBodyAngularVelocity(gyro_reference->orientation, sample.orientation, dt);
    // Faster than any swing: the pose jumped (the VR menu releasing frozen poses, tracking
    // recovery). The gyroscope steering catches up with it instead.
    if (angular_velocity.Length() < OPENXR_MAX_POSE_ANGULAR_SPEED)
    {
      out.angular_velocity = angular_velocity;
      angular_acceleration = (angular_velocity - gyro_reference->angular_velocity) / dt;
    }
  }
  sample.angular_velocity = out.angular_velocity;

  // The hand's own acceleration, from the grip velocity change over the accelerometer window.
  const Wiimote::OpenXRPoseSample* const accel_reference =
      history->FindOlderThan(sample_time_ns, OPENXR_ACCEL_WINDOW_NS);
  std::optional<Common::Vec3> velocity;
  if (controller.grip_velocity.linear_valid)
    velocity = ToVec3(controller.grip_velocity.linear);
  if (usable(accel_reference) && accel_reference->has_position && sample.has_position)
  {
    // Blend the runtime's velocity with the position change to improve push/pull response on
    // runtimes that heavily smooth linear velocity.
    const Common::Vec3 pose_velocity =
        (sample.position - accel_reference->position) / seconds_since(*accel_reference);
    velocity = velocity ? (*velocity + pose_velocity) * 0.5f : pose_velocity;
  }
  if (velocity)
  {
    sample.has_velocity = true;
    sample.velocity = *velocity;
  }
  Common::Vec3 hand_body{};
  if (usable(accel_reference) && accel_reference->has_velocity && sample.has_velocity)
  {
    const Common::Vec3 world_accel =
        (sample.velocity - accel_reference->velocity) / seconds_since(*accel_reference);
    // The estimate is centered on the reference sample, a window ago. Project it with the
    // orientation of that time: at 2000 deg/s the remote has turned 64 degrees since, and
    // the current frame would turn a sideways push into a forward thrust.
    hand_body = accel_reference->orientation.Conjugate() *
                Common::Vec3(-world_accel.x, world_accel.z, world_accel.y);
  }

  // The accelerometer's own motion about the wrist, from the gyroscope: the tangential kick of
  // a starting or stopping swing and the centripetal push during it. Analytic rather than
  // differenced, so the short kicks keep their shape and timing.
  const Common::Vec3 lever(0.0f, -OPENXR_ACCELEROMETER_LEVER_M, 0.0f);
  const Common::Vec3& w = out.angular_velocity;
  const Common::Vec3 swing_body = angular_acceleration.Cross(lever) + w.Cross(w.Cross(lever));

  // Gravity in the body frame, from the current orientation: world up in aim-local, mapped
  // with v -> (-x, z, y).
  const Common::Vec3 gravity_body(-get_matrix(0, 1), get_matrix(2, 1), get_matrix(1, 1));
  out.acceleration = gravity_body * float(MathUtil::GRAVITY_ACCELERATION) + hand_body + swing_body;
  history->Push(sample);

  // IR pointer: absolute mapping from the aim ray's intersection with the virtual screen,
  // computed by OpenXRManager with the renderer's own screen placement. Aiming at a point
  // on the 2D screen puts the Wii pointer at that point — no reference capture, no
  // recentering. Raw values are stored here; the off-screen hide decision (margins +
  // debounce) lives in the override function, which keeps state across snapshots.
  const Common::VR::OpenXRScreenHit& hit = controller.screen_hit;
  if (hit.valid)
  {
    out.ir_x = hit.u;
    out.ir_y = hit.v;
    // Emulated sensor-bar distance: EmulatePoint uses NEUTRAL_DISTANCE(2m) + ir_z.
    // Feed the real controller-to-screen distance so leaning in/out changes the
    // virtual IR dot spacing physically. The band here only rejects nonsense (screen
    // behind the player, huge rooms) — EmulatePoint applies the Point group's Distance
    // Sensitivity around the resting distance and clamps to the emulated remote's range,
    // so this offset must keep headroom on both sides for that gain to have anywhere to go.
    out.ir_z = std::clamp(hit.distance_m, 0.1f, 8.0f) - 2.0f;
  }

  return out;
}

ControllerEmu::InputOverrideFunction CreateOpenXRInputOverrideFunction(unsigned int wiimote_index,
                                                                       bool prefer_left_hand,
                                                                       bool attachment)
{
  OpenXRWiimoteState cached_state;
  Wiimote::OpenXRPoseHistory left_velocity_history;
  Wiimote::OpenXRPoseHistory right_velocity_history;
  // Sample time the pointer left the screen, 0 while it is on it.
  s64 offscreen_since_ns = 0;
  float held_ir_x = std::numeric_limits<float>::quiet_NaN();
  float held_ir_y = 0.0f;
  float held_ir_z = 0.0f;
  Wiimote::OpenXRGyroSteering gyro_steering;
  u64 gyro_ticks = 0;
  Common::Vec3 gyro{};

  return [wiimote_index, prefer_left_hand, attachment, cached_state, left_velocity_history,
          right_velocity_history, offscreen_since_ns, held_ir_x, held_ir_y, held_ir_z,
          gyro_steering, gyro_ticks,
          gyro](std::string_view group_name, std::string_view control_name,
                ControlState) mutable -> std::optional<ControlState> {
    // Input the agent server holds wins; this override owns the remote, so it reads it here.
    if (const std::optional<ControlState> agent = AgentServer::AgentInput::Lookup(
            attachment ? AgentServer::AgentInput::Device::Attachment :
                         AgentServer::AgentInput::Device::Wiimote,
            wiimote_index, group_name, control_name))
    {
      return agent;
    }

    if (s_wiimote_sources[wiimote_index].load() != WiimoteSource::OpenXR)
      return std::nullopt;

    const Common::VR::OpenXRInputSnapshot snapshot =
        Common::VR::OpenXRInputState::GetGameSnapshot();
    if (!snapshot.runtime_active)
      return std::nullopt;

    if (cached_state.generation != snapshot.generation)
    {
      const auto& left = snapshot.controllers[0];
      const auto& right = snapshot.controllers[1];

      const auto right_valid = right.grip_pose.valid || right.aim_pose.valid;
      const auto left_valid = left.grip_pose.valid || left.aim_pose.valid;

      const s64 sample_time_ns = OpenXRSampleTimeNs(snapshot);
      const float motion_pitch = MotionPitchDegrees();
      if (!prefer_left_hand && right_valid)
      {
        cached_state =
            BuildOpenXRState(right, &right_velocity_history, sample_time_ns, motion_pitch);
      }
      else if (left_valid)
      {
        cached_state = BuildOpenXRState(left, &left_velocity_history, sample_time_ns, motion_pitch);
      }
      else
        cached_state = {};

      // Off-screen debounce: a real wiimote's camera keeps tracking well past the screen
      // edge, and a runtime pose spike during fast motion must not drop the pointer.
      // Brief excursions keep reporting (the cursor pins at the screen edge, invalid-hit
      // blips hold the last good position); only a sustained one hides the pointer like
      // a real wiimote losing the sensor bar.
      const bool on_screen = !std::isnan(cached_state.ir_x) &&
                             std::abs(cached_state.ir_x) <= OPENXR_IR_HIDE_MARGIN_U &&
                             std::abs(cached_state.ir_y) <= OPENXR_IR_HIDE_MARGIN_V;
      if (on_screen)
      {
        offscreen_since_ns = 0;
        held_ir_x = cached_state.ir_x;
        held_ir_y = cached_state.ir_y;
        held_ir_z = cached_state.ir_z;
      }
      else
      {
        if (offscreen_since_ns == 0)
          offscreen_since_ns = sample_time_ns;
        if (sample_time_ns - offscreen_since_ns < OPENXR_IR_HIDE_DELAY_NS)
        {
          if (std::isnan(cached_state.ir_x))
          {
            cached_state.ir_x = held_ir_x;
            cached_state.ir_y = held_ir_y;
            cached_state.ir_z = held_ir_z;
          }
        }
        else
        {
          cached_state.ir_x = std::numeric_limits<float>::quiet_NaN();
          cached_state.ir_y = 0.0f;
          cached_state.ir_z = 0.0f;
        }
      }

      cached_state.generation = snapshot.generation;
      if (!attachment)
        PublishOpenXRInput(wiimote_index, &cached_state, gyro);
    }

    if (group_name == WiimoteEmu::Wiimote::ACCELEROMETER_GROUP)
    {
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.acceleration.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.acceleration.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return cached_state.acceleration.z;
    }
    else if (group_name == WiimoteEmu::Wiimote::GYROSCOPE_GROUP)
    {
      // One step per emulated update; its three axes are queried at the same tick.
      auto& system = Core::System::GetInstance();
      const u64 ticks = system.GetCoreTiming().GetTicks();
      if (ticks != gyro_ticks)
      {
        const double seconds =
            double(s64(ticks - gyro_ticks)) / double(system.GetSystemTimers().GetTicksPerSecond());
        gyro_ticks = ticks;
        if (cached_state.orientation_valid)
        {
          gyro =
              gyro_steering.Step(cached_state.orientation, cached_state.angular_velocity, seconds);
        }
        else
        {
          gyro_steering = {};
          gyro = cached_state.angular_velocity;
        }
        if (!attachment)
          PublishOpenXRInput(wiimote_index, &cached_state, gyro);
      }
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return gyro.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return gyro.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return gyro.z;
    }
    else if (group_name == WiimoteEmu::Wiimote::IR_GROUP)
    {
      // The pointer mapping is absolute (aim ray vs. virtual screen) — there is no
      // reference to reset, so EmulatePoint's "Recenter" signal is a no-op.
      if (control_name == "Recenter")
        return std::nullopt;
      // NaN = pointer not on screen; the cursor hides like a real wiimote losing the bar.
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_x);
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_y);
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_z);
    }

    return std::nullopt;
  };
}

// What UpdateOpenXRInputOverride last installed per remote. Config-changed callbacks call it on
// every config write (a dragged VR slider writes every frame), so it only reinstalls when the
// outcome changes: a reinstall resets the closures' velocity history and IR debounce.
struct OpenXROverrideInstall
{
  bool enabled = false;
  bool prefer_left_hand = false;
  const void* controller = nullptr;
};
std::array<OpenXROverrideInstall, MAX_BBMOTES> s_openxr_overrides{};

void UpdateOpenXRInputOverride(unsigned int index, WiimoteSource source)
{
  auto* wiimote =
      static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
  if (!wiimote)
    return;

  OpenXROverrideInstall& installed = s_openxr_overrides[index];
  const bool enable = source == WiimoteSource::OpenXR;
  const bool prefer_left_hand =
      enable && Config::Get(Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"},
                                               false});
  if (enable == installed.enabled && wiimote == installed.controller &&
      (!enable ||
       (prefer_left_hand == installed.prefer_left_hand && wiimote->HasInputOverrideFunction())))
  {
    return;
  }
  if (!enable && !installed.enabled)
    return;

  // The CPU thread calls the override functions while holding the state lock; replacing a
  // std::function another thread is executing is undefined behavior.
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();

  auto apply_to_attachments = [wiimote](const ControllerEmu::InputOverrideFunction& override_func) {
    auto* attachments_group = static_cast<ControllerEmu::Attachments*>(
        wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
    if (!attachments_group)
      return;

    for (const auto& attachment : attachments_group->GetAttachmentList())
    {
      if (!attachment)
        continue;

      if (override_func)
        attachment->SetInputOverrideFunction(override_func);
      else
        attachment->ClearInputOverrideFunction();
    }
  };

  if (enable)
  {
    wiimote->SetInputOverrideFunction(
        CreateOpenXRInputOverrideFunction(index, prefer_left_hand, false));
    apply_to_attachments(CreateOpenXRInputOverrideFunction(index, !prefer_left_hand, true));
  }
  else
  {
    wiimote->ClearInputOverrideFunction();
    apply_to_attachments({});
  }
  PublishOpenXRInput(index, nullptr);
  installed = {enable, prefer_left_hand, wiimote};
}
#else
void UpdateOpenXRInputOverride(unsigned int, WiimoteSource)
{
}
#endif

WiimoteSource GetSource(unsigned int index)
{
  return s_wiimote_sources[index];
}

void OnSourceChanged(unsigned int index, WiimoteSource source)
{
  const WiimoteSource previous_source = s_wiimote_sources[index].exchange(source);

  if (previous_source == source)
  {
    // No change. Do nothing.
    UpdateOpenXRInputOverride(index, source);
    return;
  }

  UpdateOpenXRInputOverride(index, source);

  WiimoteReal::HandleWiimoteSourceChange(index);

  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  WiimoteCommon::UpdateSource(index);
}

void RefreshConfig()
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
    OnSourceChanged(i, Config::Get(Config::GetInfoForWiimoteSource(i)));
}

}  // namespace

namespace WiimoteCommon
{
void UpdateSource(unsigned int index)
{
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  if (bluetooth == nullptr)
    return;

  bluetooth->AccessWiimoteByIndex(index)->SetSource(GetHIDWiimoteSource(index));
}

HIDWiimote* GetHIDWiimoteSource(unsigned int index)
{
  HIDWiimote* hid_source = nullptr;

  switch (GetSource(index))
  {
  case WiimoteSource::Emulated:
  case WiimoteSource::OpenXR:
    hid_source = static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
    break;

  case WiimoteSource::Real:
    hid_source = WiimoteReal::g_wiimotes[index].get();
    break;

  default:
    break;
  }

  return hid_source;
}

}  // namespace WiimoteCommon

namespace Wiimote
{
static InputConfig s_config(WIIMOTE_INI_NAME, _trans("Wii Remote"), "Wiimote", "Wiimote");

InputConfig* GetConfig()
{
  return &s_config;
}

#ifdef ENABLE_VR
bool IsOpenXRInputOverrideInstalled(unsigned int index)
{
  return index < MAX_BBMOTES && s_openxr_overrides[index].enabled;
}
#endif

std::optional<OpenXRWiiRemoteState> GetOpenXRWiiRemoteState(unsigned int index)
{
#ifdef ENABLE_VR
  if (index >= MAX_WIIMOTES || s_wiimote_sources[index].load() != WiimoteSource::OpenXR)
    return std::nullopt;

  const bool prefer_left_hand =
      Config::Get(Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"},
                                     false});
  std::optional<OpenXRWiiRemoteState> state = GetOpenXRHandState(prefer_left_hand);
  if (!state)
    state = GetOpenXRHandState(!prefer_left_hand);

  // The gyroscope comes from pose changes over time, which only the input override tracks.
  if (const auto input = GetOpenXRWiiRemoteInput(index); state && input)
  {
    state->angular_velocity = {input->angular_velocity.x, input->angular_velocity.y,
                               input->angular_velocity.z};
  }
  return state;
#else
  return std::nullopt;
#endif
}

#ifdef ENABLE_VR
Common::Quaternion OpenXRToWiimoteRotation(const Common::Quaternion& q)
{
  // BuildOpenXRState maps OpenXR vectors (X right, Y up, Z back) to the Wii Remote's with
  // v -> (-x, z, y). That basis change is a proper rotation, so the rotation keeps its angle and
  // its axis is mapped the same way.
  return Common::Quaternion(q.data.w, -q.data.x, q.data.z, q.data.y);
}

std::optional<OpenXRWiiRemotePose> GetOpenXRWiiRemotePose(unsigned int index)
{
  if (index >= MAX_WIIMOTES || s_wiimote_sources[index].load() != WiimoteSource::OpenXR)
    return std::nullopt;

  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetGameSnapshot();
  if (!snapshot.runtime_active)
    return std::nullopt;

  // Same hand choice as the input override (CreateOpenXRInputOverrideFunction).
  const bool prefer_left_hand =
      Config::Get(Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"}, false});
  const auto is_valid = [](const Common::VR::OpenXRControllerState& controller) {
    return controller.grip_pose.valid || controller.aim_pose.valid;
  };
  bool left_hand;
  if (!prefer_left_hand && is_valid(snapshot.controllers[1]))
    left_hand = false;
  else if (is_valid(snapshot.controllers[0]))
    left_hand = true;
  else
    return std::nullopt;

  const Common::VR::OpenXRControllerState& controller = snapshot.controllers[left_hand ? 0 : 1];
  OpenXRWiiRemotePose out;
  out.orientation = OpenXRToWiimoteRotation(GetMotionOrientation(controller, MotionPitchDegrees()));
  out.screen_distance_valid = controller.screen_hit.valid;
  out.screen_distance_m = controller.screen_hit.distance_m;
  out.left_hand = left_hand;
  out.controller = controller;
  out.generation = snapshot.generation;
  out.sample_time_ns = snapshot.sample_time_ns;
  out.input_withheld = snapshot.menu.input_withheld;
  return out;
}

std::optional<OpenXRWiiRemoteInput> GetOpenXRWiiRemoteInput(unsigned int index)
{
  if (index >= MAX_WIIMOTES || s_wiimote_sources[index].load() != WiimoteSource::OpenXR)
    return std::nullopt;

  std::lock_guard lock(s_openxr_inputs[index].mutex);
  return s_openxr_inputs[index].input;
}

Common::Vec3 GetOpenXRBodyAngularVelocity(const Common::Quaternion& from,
                                          const Common::Quaternion& to, float seconds)
{
  // The rotation vector of from^-1 * to, by the shortest path.
  Common::Quaternion delta = from.Conjugate() * to;
  if (delta.data.w < 0)
    delta = Common::Quaternion(-delta.data.w, -delta.data.x, -delta.data.y, -delta.data.z);
  const Common::Vec3 axis(delta.data.x, delta.data.y, delta.data.z);
  const float sin_half = axis.Length();
  if (!(sin_half > 1e-7f) || !(seconds > 0))
    return {};
  const float angle = 2 * std::atan2(sin_half, delta.data.w);
  return axis * (angle / sin_half / seconds);
}

void OpenXRPoseHistory::Push(const OpenXRPoseSample& sample)
{
  samples[next] = sample;
  next = (next + 1) % CAPACITY;
  count = std::min(count + 1, CAPACITY);
}

void OpenXRPoseHistory::Clear()
{
  count = 0;
  next = 0;
}

const OpenXRPoseSample* OpenXRPoseHistory::FindOlderThan(s64 time_ns, s64 age_ns) const
{
  const OpenXRPoseSample* oldest = nullptr;
  // Newest first.
  for (size_t i = 0; i < count; ++i)
  {
    const OpenXRPoseSample& sample = samples[(next + CAPACITY - 1 - i) % CAPACITY];
    if (time_ns - sample.time_ns >= age_ns)
      return &sample;
    oldest = &sample;
  }
  if (oldest && time_ns - oldest->time_ns >= age_ns / 2)
    return oldest;
  return nullptr;
}

Common::Vec3 OpenXRGyroSteering::Step(const Common::Quaternion& pose, const Common::Vec3& pose_rate,
                                      double seconds)
{
  if (!valid || !(seconds > 0) || seconds > MAX_STEP_SECONDS)
  {
    valid = true;
    integrated = pose;
    rate = pose_rate;
    reports = 0;
    return rate;
  }

  // The game integrated the rate it was given over the time since the previous report.
  integrated =
      (integrated * WiimoteEmu::GetRotationFromGyroscope(rate * float(seconds))).Normalized();

  // In MotionPlus passthrough modes the game only receives every other report's rate. Changing
  // the rate every other report makes the game's integral match this one either way.
  if (++reports % 2 == 0)
  {
    Common::Vec3 correction = GetOpenXRBodyAngularVelocity(integrated, pose, STEERING_SECONDS);
    const float correction_speed = correction.Length();
    if (correction_speed > MAX_STEERING_SPEED)
      correction = correction * (MAX_STEERING_SPEED / correction_speed);
    rate = pose_rate + correction;
  }
  return rate;
}
#endif

std::optional<OpenXRWiiRemoteState> GetOpenXRHandState(bool left_hand)
{
#ifdef ENABLE_VR
  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetGameSnapshot();
  if (!snapshot.runtime_active)
    return std::nullopt;

  const auto& controller = snapshot.controllers[left_hand ? 0 : 1];
  if (!controller.grip_pose.valid && !controller.aim_pose.valid)
    return std::nullopt;

  Wiimote::OpenXRPoseHistory throwaway_velocity_history;
  OpenXRWiimoteState state = BuildOpenXRState(controller, &throwaway_velocity_history,
                                              OpenXRSampleTimeNs(snapshot), MotionPitchDegrees());

  OpenXRWiiRemoteState out;
  out.acceleration = {state.acceleration.x, state.acceleration.y, state.acceleration.z};
  out.angular_velocity = {state.angular_velocity.x, state.angular_velocity.y, state.angular_velocity.z};
  out.ir_x = state.ir_x;
  out.ir_y = state.ir_y;
  out.ir_visible = !std::isnan(state.ir_x);
  return out;
#else
  return std::nullopt;
#endif
}

ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetWiimoteGroup(group);
}

ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetNunchukGroup(group);
}

ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetClassicGroup(group);
}

ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetGuitarGroup(group);
}

ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetDrumsGroup(group);
}

ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetTurntableGroup(group);
}

ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetUDrawTabletGroup(group);
}

ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetDrawsomeTabletGroup(group);
}

ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetTaTaConGroup(group);
}

ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetShinkansenGroup(group);
}

void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();
#ifdef ENABLE_VR
  s_openxr_overrides = {};
#endif

  WiimoteReal::Stop();

  if (s_config_callback_id)
  {
    Config::RemoveConfigChangedCallback(*s_config_callback_id);
    s_config_callback_id = std::nullopt;
  }
}

void Initialize(InitializeMode init_mode)
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
      s_config.CreateController<WiimoteEmu::Wiimote>(i);
  }

  s_config.RegisterHotplugCallback();

  LoadConfig();

  if (!s_config_callback_id)
    s_config_callback_id = Config::AddConfigChangedCallback(RefreshConfig);
  RefreshConfig();

  WiimoteReal::Initialize(init_mode);

  // Reload Wiimotes with our settings
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsMovieActive())
    movie.ChangeWiiPads();
}

void ResetAllWiimotes()
{
  for (int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
    static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->Reset();
}

void LoadConfig()
{
  s_config.LoadConfig();
  s_last_connect_request_counter.fill(0);
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

void Resume()
{
  WiimoteReal::Resume();
}

void Pause()
{
  WiimoteReal::Pause();
}

void DoState(PointerWrap& p)
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
  {
    const WiimoteSource source = GetSource(i);
    auto state_wiimote_source = u8(source);
    p.Do(state_wiimote_source);

    if (IsEmulatedSource(WiimoteSource(state_wiimote_source)))
    {
      // Sync complete state of emulated wiimotes.
      static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->DoState(p);
    }

    if (p.IsReadMode())
    {
      // If using a real wiimote or the save-state source does not match the current source,
      // then force a reconnection on load.
      if (source == WiimoteSource::Real || source != WiimoteSource(state_wiimote_source))
        WiimoteCommon::UpdateSource(i);
    }
  }
}

}  // namespace Wiimote
