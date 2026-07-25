// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Wiimote.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#ifdef ENABLE_VR
#include "Common/VR/OpenXRInputState.h"
#endif

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteEmu/Camera.h"
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

struct OpenXRVelocityHistory
{
  bool has_pose_sample = false;
  bool has_velocity_sample = false;
  Common::Vec3 previous_position{};
  Common::Vec3 previous_velocity{};
  s64 previous_time_ns = 0;
};

struct OpenXRWiimoteState
{
  u64 generation = std::numeric_limits<u64>::max();
  Common::Vec3 acceleration{0.0f, 0.0f, float(MathUtil::GRAVITY_ACCELERATION)};
  Common::Vec3 angular_velocity{};
  float ir_x = std::numeric_limits<float>::quiet_NaN();
  float ir_y = 0.0f;
  float ir_z = 0.0f;  // Forward/backward distance offset from NEUTRAL_DISTANCE, in meters
};

// How far past the screen edge (in screen half-extents) the pointer stays tracked before
// the emulated IR camera "loses" it, matching real hardware: the wiimote camera FOV
// (42°x31.5°) extends well past the cursor's Total Yaw/Pitch range (25°/20°), so real
// dots survive to roughly these excursions.
constexpr float OPENXR_IR_HIDE_MARGIN_U = 1.9f;
constexpr float OPENXR_IR_HIDE_MARGIN_V = 1.5f;
// An off-screen excursion must persist this many input snapshots (~100ms at 72-90Hz)
// before the pointer is hidden. Runtime pose spikes during fast controller motion produce
// brief excursions; without the debounce every spike dropped IR tracking entirely.
constexpr int OPENXR_IR_HIDE_DELAY_SNAPSHOTS = 9;

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
                                    OpenXRVelocityHistory* velocity_history, s64 sample_time_ns)
{
  OpenXRWiimoteState out;

  const bool has_grip_pose = controller.grip_pose.valid;
  const bool has_aim_pose = controller.aim_pose.valid;
  if (!has_grip_pose && !has_aim_pose)
    return out;

  // Use aim pose for the motion reference frame. The aim pose orientation determines how
  // world-space gravity maps to Wiimote-local accelerometer axes. Using grip would produce
  // wrong accel values because grip/aim can differ by 30-60 degrees on many controllers.
  const Common::Quaternion reference_orientation =
      (has_aim_pose ? ToQuaternion(controller.aim_pose.orientation) :
                      ToQuaternion(controller.grip_pose.orientation))
          .Normalized();
  const Common::Matrix33 world_to_local =
      Common::Matrix33::FromQuaternion(reference_orientation).Transposed();

  const auto get_matrix = [&world_to_local](int row, int col) { return world_to_local.data[row * 3 + col]; };

  Common::Vec3 relative_acceleration{};
  const float dt =
      velocity_history->has_pose_sample ?
          float(sample_time_ns - velocity_history->previous_time_ns) * 1e-9f :
          0.0f;

  Common::Vec3 current_position{};
  std::optional<Common::Vec3> pose_velocity;
  if (has_grip_pose)
  {
    current_position = ToVec3(controller.grip_pose.position);
    if (velocity_history->has_pose_sample && dt > 0.001f)
      pose_velocity = (current_position - velocity_history->previous_position) / dt;
  }

  std::optional<Common::Vec3> current_velocity;
  if (controller.grip_velocity.linear_valid)
    current_velocity = ToVec3(controller.grip_velocity.linear);

  if (pose_velocity)
  {
    // Blend runtime velocity with pose-derived velocity to improve push/pull response on
    // runtimes that heavily smooth linear velocity.
    if (current_velocity)
      current_velocity = (*current_velocity + *pose_velocity) * 0.5f;
    else
      current_velocity = *pose_velocity;
  }

  if (current_velocity && velocity_history->has_velocity_sample && dt > 0.001f)
  {
    const Common::Vec3 world_accel = (*current_velocity - velocity_history->previous_velocity) / dt;
    relative_acceleration = world_to_local * world_accel;
  }

  if (has_grip_pose)
  {
    velocity_history->previous_position = current_position;
    velocity_history->previous_time_ns = sample_time_ns;
    velocity_history->has_pose_sample = true;
  }
  else
  {
    velocity_history->has_pose_sample = false;
    velocity_history->has_velocity_sample = false;
  }

  if (current_velocity)
  {
    velocity_history->previous_velocity = *current_velocity;
    velocity_history->has_velocity_sample = true;
  }
  else if (!velocity_history->has_pose_sample)
  {
    velocity_history->has_velocity_sample = false;
  }

  float gx = -get_matrix(0, 1);
  float gz = get_matrix(1, 1);
  float gy = get_matrix(2, 1);

  gx -= relative_acceleration.x / float(MathUtil::GRAVITY_ACCELERATION);
  gz += relative_acceleration.y / float(MathUtil::GRAVITY_ACCELERATION);
  gy += relative_acceleration.z / float(MathUtil::GRAVITY_ACCELERATION);

  out.acceleration = Common::Vec3(gx, gy, gz) * float(MathUtil::GRAVITY_ACCELERATION);

  if (controller.grip_velocity.angular_valid)
  {
    const Common::Vec3 world_angular_velocity = ToVec3(controller.grip_velocity.angular);
    const Common::Vec3 local_angular_velocity = world_to_local * world_angular_velocity;
    // Aim-local (X=right, Y=up, Z=back) to Dolphin's gyro convention
    // (+x = pitch down, +y = roll left, +z = yaw left), a proper rotation:
    // pitch up is +x in aim-local but -x for the wiimote.
    out.angular_velocity = Common::Vec3(-local_angular_velocity.x, local_angular_velocity.z,
                                        local_angular_velocity.y);
  }

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
                                                                       bool prefer_left_hand)
{
  OpenXRWiimoteState cached_state;
  OpenXRVelocityHistory left_velocity_history;
  OpenXRVelocityHistory right_velocity_history;
  int offscreen_snapshots = 0;
  float held_ir_x = std::numeric_limits<float>::quiet_NaN();
  float held_ir_y = 0.0f;
  float held_ir_z = 0.0f;

  return [wiimote_index, prefer_left_hand, cached_state, left_velocity_history,
          right_velocity_history, offscreen_snapshots, held_ir_x, held_ir_y,
          held_ir_z](std::string_view group_name, std::string_view control_name,
                     ControlState) mutable -> std::optional<ControlState> {
    if (s_wiimote_sources[wiimote_index].load() != WiimoteSource::OpenXR)
      return std::nullopt;

    const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
    if (!snapshot.runtime_active)
      return std::nullopt;

    if (cached_state.generation != snapshot.generation)
    {
      const auto& left = snapshot.controllers[0];
      const auto& right = snapshot.controllers[1];

      const auto right_valid = right.grip_pose.valid || right.aim_pose.valid;
      const auto left_valid = left.grip_pose.valid || left.aim_pose.valid;

      const s64 sample_time_ns = OpenXRSampleTimeNs(snapshot);
      if (!prefer_left_hand && right_valid)
        cached_state = BuildOpenXRState(right, &right_velocity_history, sample_time_ns);
      else if (left_valid)
        cached_state = BuildOpenXRState(left, &left_velocity_history, sample_time_ns);
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
        offscreen_snapshots = 0;
        held_ir_x = cached_state.ir_x;
        held_ir_y = cached_state.ir_y;
        held_ir_z = cached_state.ir_z;
      }
      else if (++offscreen_snapshots < OPENXR_IR_HIDE_DELAY_SNAPSHOTS)
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

      cached_state.generation = snapshot.generation;
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
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.angular_velocity.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.angular_velocity.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return cached_state.angular_velocity.z;
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

std::array<bool, MAX_BBMOTES> s_openxr_overrides_enabled{};

void UpdateOpenXRInputOverride(unsigned int index, WiimoteSource source)
{
  auto* wiimote =
      static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
  if (!wiimote)
    return;

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

  if (source == WiimoteSource::OpenXR)
  {
    const bool prefer_left_hand =
        Config::Get(Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"},
                                       false});
    wiimote->SetInputOverrideFunction(
        CreateOpenXRInputOverrideFunction(index, prefer_left_hand));
    apply_to_attachments(CreateOpenXRInputOverrideFunction(index, !prefer_left_hand));
    s_openxr_overrides_enabled[index] = true;
  }
  else if (s_openxr_overrides_enabled[index])
  {
    wiimote->ClearInputOverrideFunction();
    apply_to_attachments({});
    s_openxr_overrides_enabled[index] = false;
  }
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

std::optional<OpenXRWiiRemoteState> GetOpenXRWiiRemoteState(unsigned int index)
{
#ifdef ENABLE_VR
  if (index >= MAX_WIIMOTES || s_wiimote_sources[index].load() != WiimoteSource::OpenXR)
    return std::nullopt;

  const bool prefer_left_hand =
      Config::Get(Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"},
                                     false});
  if (prefer_left_hand)
  {
    if (const auto left = GetOpenXRHandState(true))
      return left;
    return GetOpenXRHandState(false);
  }

  if (const auto right = GetOpenXRHandState(false))
    return right;
  return GetOpenXRHandState(true);
#else
  return std::nullopt;
#endif
}

std::optional<OpenXRWiiRemoteState> GetOpenXRHandState(bool left_hand)
{
#ifdef ENABLE_VR
  const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
  if (!snapshot.runtime_active)
    return std::nullopt;

  const auto& controller = snapshot.controllers[left_hand ? 0 : 1];
  if (!controller.grip_pose.valid && !controller.aim_pose.valid)
    return std::nullopt;

  OpenXRVelocityHistory throwaway_velocity_history;
  OpenXRWiimoteState state =
      BuildOpenXRState(controller, &throwaway_velocity_history, OpenXRSampleTimeNs(snapshot));

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
