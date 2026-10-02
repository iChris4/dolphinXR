// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "InputCommon/ControllerInterface/OpenXR/OpenXR.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Common/VR/OpenXRInputState.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ciface::OpenXR
{
namespace
{
constexpr std::string_view SOURCE_NAME = "OpenXR";

enum class Hand
{
  Left = 0,
  Right = 1,
};

Common::Quaternion ToQuaternion(const std::array<float, 4>& quat)
{
  return {quat[3], quat[0], quat[1], quat[2]};
}

Common::Vec3 ToVec3(const std::array<float, 3>& vec)
{
  return {vec[0], vec[1], vec[2]};
}

class OpenXRDevice final : public Core::Device
{
public:
  OpenXRDevice()
  {
    AddHandInputs(Hand::Left);
    AddHandInputs(Hand::Right);
    AddOutput(new RumbleOutput(this, RumbleTarget::Both));
    AddOutput(new RumbleOutput(this, RumbleTarget::Left));
    AddOutput(new RumbleOutput(this, RumbleTarget::Right));
  }

  std::string GetName() const override { return "OpenXR Controller"; }

  std::string GetSource() const override { return std::string(SOURCE_NAME); }

  bool IsVirtualDevice() const override { return true; }

  int GetSortPriority() const override { return -10; }

  Core::DeviceRemoval UpdateInput() override
  {
    m_snapshot = Common::VR::OpenXRInputState::GetSnapshot();
    UpdateMotionState(Hand::Left);
    UpdateMotionState(Hand::Right);
    return Core::DeviceRemoval::Keep;
  }

  const Common::VR::OpenXRControllerState& GetControllerState(Hand hand) const
  {
    return m_snapshot.controllers[static_cast<size_t>(hand)];
  }

  struct MotionState
  {
    Common::Vec3 acceleration{};
    Common::Vec3 gyroscope{};
    bool acceleration_valid = false;
    bool gyroscope_valid = false;
  };

  const MotionState& GetMotionState(Hand hand) const
  {
    return m_motion_state[static_cast<size_t>(hand)];
  }

private:
  enum class RumbleTarget
  {
    Both,
    Left,
    Right,
  };

  class RumbleOutput final : public Core::Device::Output
  {
  public:
    RumbleOutput(OpenXRDevice* device, RumbleTarget target) : m_device(*device), m_target(target) {}

    std::string GetName() const override
    {
      switch (m_target)
      {
      case RumbleTarget::Both:
        return "Motor";
      case RumbleTarget::Left:
        return "Motor Left";
      case RumbleTarget::Right:
        return "Motor Right";
      }
      return "Motor";
    }

    void SetState(ControlState state) override { m_device.SetRumbleState(state, m_target); }

  private:
    OpenXRDevice& m_device;
    RumbleTarget m_target;
  };

  std::string GetHandPrefix(Hand hand) const { return hand == Hand::Left ? "Left" : "Right"; }

  void SetRumbleState(ControlState state, RumbleTarget target)
  {
    const float amplitude = std::clamp(static_cast<float>(state), 0.0f, 1.0f);
    switch (target)
    {
    case RumbleTarget::Both:
      Common::VR::OpenXRInputState::SetRumble(amplitude);
      break;
    case RumbleTarget::Left:
      Common::VR::OpenXRInputState::SetRumbleForHand(0, amplitude);
      break;
    case RumbleTarget::Right:
      Common::VR::OpenXRInputState::SetRumbleForHand(1, amplitude);
      break;
    }
  }

  void AddHandInputs(Hand hand)
  {
    AddInput(new DigitalInput(this, hand, DigitalControl::Primary));
    AddInput(new DigitalInput(this, hand, DigitalControl::Secondary));
    AddInput(new DigitalInput(this, hand, DigitalControl::Button3));
    AddInput(new DigitalInput(this, hand, DigitalControl::Button4));
    AddInput(new DigitalInput(this, hand, DigitalControl::Menu));
    AddInput(new DigitalInput(this, hand, DigitalControl::Shoulder));
    AddInput(new DigitalInput(this, hand, DigitalControl::Trigger));
    AddInput(new DigitalInput(this, hand, DigitalControl::Squeeze));
    AddInput(new DigitalInput(this, hand, DigitalControl::Thumbstick));
    AddInput(new AnalogInput(this, hand, AnalogControl::Trigger));
    AddInput(new AnalogInput(this, hand, AnalogControl::Squeeze));
    AddInput(new AnalogInput(this, hand, AnalogControl::SqueezeForce));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickX, false));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickX, true));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickY, false));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickY, true));
    AddMotionInputs(hand);
  }

  enum class DigitalControl
  {
    Primary,
    Secondary,
    Button3,
    Button4,
    Menu,
    Shoulder,
    Trigger,
    Squeeze,
    Thumbstick,
  };

  enum class AnalogControl
  {
    Trigger,
    Squeeze,
    SqueezeForce,
  };

  enum class AxisControl
  {
    ThumbstickX,
    ThumbstickY,
  };

  enum class MotionControl
  {
    AccelUp,
    AccelDown,
    AccelLeft,
    AccelRight,
    AccelForward,
    AccelBackward,
    GyroPitchUp,
    GyroPitchDown,
    GyroRollLeft,
    GyroRollRight,
    GyroYawLeft,
    GyroYawRight,
  };

  class DigitalInput final : public Core::Device::Input
  {
  public:
    DigitalInput(const OpenXRDevice* device, Hand hand, DigitalControl control)
        : m_device(*device), m_hand(hand), m_control(control)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_device.GetHandPrefix(m_hand);
      switch (m_control)
      {
      case DigitalControl::Primary:
        return prefix + (m_hand == Hand::Left ? " Button X" : " Button A");
      case DigitalControl::Secondary:
        return prefix + (m_hand == Hand::Left ? " Button Y" : " Button B");
      case DigitalControl::Button3:
        return prefix + " Button 3";
      case DigitalControl::Button4:
        return prefix + " Button 4";
      case DigitalControl::Menu:
        return prefix + " Button Menu";
      case DigitalControl::Shoulder:
        return prefix + " Button Shoulder";
      case DigitalControl::Trigger:
        return prefix + " Button Trigger";
      case DigitalControl::Squeeze:
        return prefix + " Button Squeeze";
      case DigitalControl::Thumbstick:
        return prefix + " Button Thumbstick";
      }

      return "";
    }

    ControlState GetState() const override
    {
      const auto& state = m_device.GetControllerState(m_hand);
      if (!state.connected)
        return 0.0;

      switch (m_control)
      {
      case DigitalControl::Primary:
        return state.primary_button ? 1.0 : 0.0;
      case DigitalControl::Secondary:
        return state.secondary_button ? 1.0 : 0.0;
      case DigitalControl::Button3:
        return state.button_3 ? 1.0 : 0.0;
      case DigitalControl::Button4:
        return state.button_4 ? 1.0 : 0.0;
      case DigitalControl::Menu:
        return state.menu_button ? 1.0 : 0.0;
      case DigitalControl::Shoulder:
        return state.shoulder_button ? 1.0 : 0.0;
      case DigitalControl::Trigger:
        return state.trigger_button ? 1.0 : 0.0;
      case DigitalControl::Squeeze:
        return state.squeeze_button ? 1.0 : 0.0;
      case DigitalControl::Thumbstick:
        return state.thumbstick_button ? 1.0 : 0.0;
      }

      return 0.0;
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const DigitalControl m_control;
  };

  class AnalogInput final : public Core::Device::Input
  {
  public:
    AnalogInput(const OpenXRDevice* device, Hand hand, AnalogControl control)
        : m_device(*device), m_hand(hand), m_control(control)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_device.GetHandPrefix(m_hand);
      switch (m_control)
      {
      case AnalogControl::Trigger:
        return prefix + " Trigger";
      case AnalogControl::Squeeze:
        return prefix + " Squeeze";
      case AnalogControl::SqueezeForce:
        return prefix + " Squeeze Force";
      }

      return "";
    }

    ControlState GetState() const override
    {
      const auto& state = m_device.GetControllerState(m_hand);
      if (!state.connected)
        return 0.0;

      switch (m_control)
      {
      case AnalogControl::Trigger:
        return state.trigger_value;
      case AnalogControl::Squeeze:
        return state.squeeze_value;
      case AnalogControl::SqueezeForce:
        return state.squeeze_force;
      }

      return 0.0;
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const AnalogControl m_control;
  };

  class AxisInput final : public Core::Device::Input
  {
  public:
    AxisInput(const OpenXRDevice* device, Hand hand, AxisControl axis, bool positive)
        : m_device(*device), m_hand(hand), m_axis(axis), m_positive(positive)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_device.GetHandPrefix(m_hand);
      const char axis_char = m_axis == AxisControl::ThumbstickX ? 'X' : 'Y';
      return prefix + " Thumbstick " + axis_char + (m_positive ? '+' : '-');
    }

    ControlState GetState() const override
    {
      const auto& state = m_device.GetControllerState(m_hand);
      if (!state.connected)
        return 0.0;

      const float value =
          m_axis == AxisControl::ThumbstickX ? state.thumbstick_x : state.thumbstick_y;
      return m_positive ? std::max<float>(0.0f, value) : std::max<float>(0.0f, -value);
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const AxisControl m_axis;
    const bool m_positive;
  };

  class MotionInput final : public Core::Device::Input
  {
  public:
    MotionInput(const OpenXRDevice* device, Hand hand, MotionControl control)
        : m_device(*device), m_hand(hand), m_control(control)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_device.GetHandPrefix(m_hand);
      switch (m_control)
      {
      case MotionControl::AccelUp:
        return prefix + " Raw Accel Up";
      case MotionControl::AccelDown:
        return prefix + " Raw Accel Down";
      case MotionControl::AccelLeft:
        return prefix + " Raw Accel Left";
      case MotionControl::AccelRight:
        return prefix + " Raw Accel Right";
      case MotionControl::AccelForward:
        return prefix + " Raw Accel Forward";
      case MotionControl::AccelBackward:
        return prefix + " Raw Accel Backward";
      case MotionControl::GyroPitchUp:
        return prefix + " Raw Gyro Pitch Up";
      case MotionControl::GyroPitchDown:
        return prefix + " Raw Gyro Pitch Down";
      case MotionControl::GyroRollLeft:
        return prefix + " Raw Gyro Roll Left";
      case MotionControl::GyroRollRight:
        return prefix + " Raw Gyro Roll Right";
      case MotionControl::GyroYawLeft:
        return prefix + " Raw Gyro Yaw Left";
      case MotionControl::GyroYawRight:
        return prefix + " Raw Gyro Yaw Right";
      }

      return "";
    }

    bool IsDetectable() const override { return false; }

    ControlState GetState() const override
    {
      const auto& motion = m_device.GetMotionState(m_hand);
      constexpr float accel_scale = float(MathUtil::GRAVITY_ACCELERATION);
      constexpr float gyro_scale = float(MathUtil::PI);

      const auto normalize = [](float value, float scale) -> ControlState {
        return std::clamp<ControlState>(value / scale, 0.0, 1.0);
      };

      switch (m_control)
      {
      case MotionControl::AccelUp:
        return motion.acceleration_valid ? normalize(motion.acceleration.z, accel_scale) : 0.0;
      case MotionControl::AccelDown:
        return motion.acceleration_valid ? normalize(-motion.acceleration.z, accel_scale) : 0.0;
      case MotionControl::AccelLeft:
        return motion.acceleration_valid ? normalize(motion.acceleration.x, accel_scale) : 0.0;
      case MotionControl::AccelRight:
        return motion.acceleration_valid ? normalize(-motion.acceleration.x, accel_scale) : 0.0;
      case MotionControl::AccelForward:
        return motion.acceleration_valid ? normalize(-motion.acceleration.y, accel_scale) : 0.0;
      case MotionControl::AccelBackward:
        return motion.acceleration_valid ? normalize(motion.acceleration.y, accel_scale) : 0.0;
      case MotionControl::GyroPitchUp:
        return motion.gyroscope_valid ? normalize(motion.gyroscope.x, gyro_scale) : 0.0;
      case MotionControl::GyroPitchDown:
        return motion.gyroscope_valid ? normalize(-motion.gyroscope.x, gyro_scale) : 0.0;
      case MotionControl::GyroRollLeft:
        return motion.gyroscope_valid ? normalize(motion.gyroscope.y, gyro_scale) : 0.0;
      case MotionControl::GyroRollRight:
        return motion.gyroscope_valid ? normalize(-motion.gyroscope.y, gyro_scale) : 0.0;
      case MotionControl::GyroYawLeft:
        return motion.gyroscope_valid ? normalize(motion.gyroscope.z, gyro_scale) : 0.0;
      case MotionControl::GyroYawRight:
        return motion.gyroscope_valid ? normalize(-motion.gyroscope.z, gyro_scale) : 0.0;
      }

      return 0.0;
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const MotionControl m_control;
  };

  void AddMotionInputs(Hand hand)
  {
    AddInput(new MotionInput(this, hand, MotionControl::AccelUp));
    AddInput(new MotionInput(this, hand, MotionControl::AccelDown));
    AddInput(new MotionInput(this, hand, MotionControl::AccelLeft));
    AddInput(new MotionInput(this, hand, MotionControl::AccelRight));
    AddInput(new MotionInput(this, hand, MotionControl::AccelForward));
    AddInput(new MotionInput(this, hand, MotionControl::AccelBackward));
    AddInput(new MotionInput(this, hand, MotionControl::GyroPitchUp));
    AddInput(new MotionInput(this, hand, MotionControl::GyroPitchDown));
    AddInput(new MotionInput(this, hand, MotionControl::GyroRollLeft));
    AddInput(new MotionInput(this, hand, MotionControl::GyroRollRight));
    AddInput(new MotionInput(this, hand, MotionControl::GyroYawLeft));
    AddInput(new MotionInput(this, hand, MotionControl::GyroYawRight));
  }

  struct VelocityHistory
  {
    bool has_sample = false;
    Common::Vec3 previous_velocity{};
    std::chrono::steady_clock::time_point previous_time{};
  };

  void UpdateMotionState(Hand hand)
  {
    const size_t hand_index = static_cast<size_t>(hand);
    auto& motion = m_motion_state[hand_index];
    motion = {};

    const auto& controller = GetControllerState(hand);
    const bool has_grip_pose = controller.grip_pose.valid;
    const bool has_aim_pose = controller.aim_pose.valid;
    if (!controller.connected || (!has_grip_pose && !has_aim_pose))
    {
      m_velocity_history[hand_index].has_sample = false;
      return;
    }

    const Common::Quaternion reference_orientation =
        (has_grip_pose ? ToQuaternion(controller.grip_pose.orientation) :
                         ToQuaternion(controller.aim_pose.orientation))
            .Normalized();
    const Common::Matrix33 world_to_local =
        Common::Matrix33::FromQuaternion(reference_orientation).Transposed();

    Common::Vec3 relative_acceleration{};
    auto& velocity_history = m_velocity_history[hand_index];
    if (controller.grip_velocity.linear_valid)
    {
      const Common::Vec3 current_velocity = ToVec3(controller.grip_velocity.linear);
      const auto now = std::chrono::steady_clock::now();
      if (velocity_history.has_sample)
      {
        const float dt = std::chrono::duration_cast<std::chrono::duration<float>>(
                             now - velocity_history.previous_time)
                             .count();
        if (dt > 0.001f)
        {
          const Common::Vec3 world_acceleration =
              (current_velocity - velocity_history.previous_velocity) / dt;
          relative_acceleration = world_to_local * world_acceleration;
        }
      }

      velocity_history.has_sample = true;
      velocity_history.previous_velocity = current_velocity;
      velocity_history.previous_time = now;
    }
    else
    {
      velocity_history.has_sample = false;
    }

    const auto get_matrix = [&world_to_local](int row, int col) {
      return world_to_local.data[row * 3 + col];
    };
    float gx = -get_matrix(0, 1);
    float gz = get_matrix(1, 1);
    float gy = get_matrix(2, 1);

    gx -= relative_acceleration.x / float(MathUtil::GRAVITY_ACCELERATION);
    gz += relative_acceleration.y / float(MathUtil::GRAVITY_ACCELERATION);
    gy += relative_acceleration.z / float(MathUtil::GRAVITY_ACCELERATION);

    motion.acceleration = Common::Vec3(gx, gy, gz) * float(MathUtil::GRAVITY_ACCELERATION);
    motion.acceleration_valid = true;

    if (controller.grip_velocity.angular_valid)
    {
      const Common::Vec3 world_angular_velocity = ToVec3(controller.grip_velocity.angular);
      const Common::Vec3 local_angular_velocity = world_to_local * world_angular_velocity;
      motion.gyroscope = Common::Vec3(local_angular_velocity.x, local_angular_velocity.z,
                                      local_angular_velocity.y);
      motion.gyroscope_valid = true;
    }
  }

  Common::VR::OpenXRInputSnapshot m_snapshot{};
  std::array<MotionState, 2> m_motion_state{};
  std::array<VelocityHistory, 2> m_velocity_history{};
};

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;

  void PopulateDevices() override
  {
    GetControllerInterface().RemoveDevice(
        [](const auto* dev) { return dev->GetSource() == std::string(SOURCE_NAME); });

    GetControllerInterface().AddDevice(std::make_shared<OpenXRDevice>());
  }
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::OpenXR

#endif  // ENABLE_VR
