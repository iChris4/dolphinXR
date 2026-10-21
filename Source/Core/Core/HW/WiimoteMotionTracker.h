// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"

// Passive observer of the reports exchanged between the game and each Wii Remote slot, for the
// Wii Remote Motion Tester. It decodes the accelerometer, MotionPlus and IR data from the bytes
// the game receives and fuses them into an orientation. Emulated, OpenXR and real Wii Remotes go
// through the same decoder, so differences between them are differences the game sees.
//
// Frames follow Dolphin's Wii Remote conventions: body +X left, +Y backward (toward the player),
// +Z up. The IR camera looks along -Y. Angles use the right-hand rule about those axes:
// pitch +X (nose down), roll +Y (roll left), yaw +Z (yaw left).
//
// The On* functions are called from the CPU thread (IOS::HLE::WiimoteDevice). The other
// functions may be called from any thread.
namespace WiimoteMotionTracker
{
enum class MotionPlusState : u8
{
  // State lost (savestate load). Decoding falls back to a heuristic.
  Unknown,
  Inactive,
  Active,
  // Unknown state, but the extension bytes look like MotionPlus data.
  AssumedActive,
};

enum class CalibrationSource : u8
{
  // Nominal values (Dolphin's emulated Wii Remote) until the game reads the device's.
  Default,
  Device,
  DeviceBadChecksum,
};

struct IRDot
{
  bool visible = false;
  u16 x = 0;
  u16 y = 0;
  u8 size = 0;
};

struct Sample
{
  enum Flags : u8
  {
    HAS_ACCEL = 1 << 0,
    // This report carried a MotionPlus sample. Otherwise gyro_dps holds the previous one.
    HAS_GYRO = 1 << 1,
    SLOW_PITCH = 1 << 2,
    SLOW_ROLL = 1 << 3,
    SLOW_YAW = 1 << 4,
    // Same bytes as the previous report (real Wii Remotes repeat reports to keep 200 Hz).
    DUPLICATE = 1 << 5,
  };

  // Emulated seconds since the tester was enabled.
  float time = 0;
  Common::Vec3 accel_g{};
  // Pitch, roll, yaw in degrees per second.
  Common::Vec3 gyro_dps{};
  u8 flags = 0;
};

struct Snapshot
{
  bool has_source = false;
  // Number of data reports decoded since the tester was enabled for this slot.
  u64 report_count = 0;
  // Emulated seconds of the last data report since the tester was enabled.
  float time = 0;

  WiimoteCommon::InputReportID report_id{};
  WiimoteCommon::InputReportID requested_mode{};
  bool continuous = false;
  std::array<u8, WiimoteCommon::MAX_PAYLOAD> last_report{};
  u8 last_report_size = 0;
  float report_rate_hz = 0;
  float unique_report_rate_hz = 0;

  MotionPlusState motion_plus = MotionPlusState::Inactive;
  // Passthrough mode written at 0xfe: 0x04 (none), 0x05 (Nunchuk), 0x07 (Classic).
  u8 motion_plus_mode = 0;
  bool extension_bit = false;
  bool ext_id_valid = false;
  std::array<u8, 6> ext_id{};

  bool has_accel = false;
  std::array<u16, 3> accel_raw{};
  Common::Vec3 accel_g{};

  bool has_gyro = false;
  // Pitch, roll, yaw (14-bit raw values).
  std::array<u16, 3> gyro_raw{};
  std::array<bool, 3> gyro_slow{};
  Common::Vec3 gyro_rad_s{};
  // Emulated seconds since the last MotionPlus sample.
  float gyro_age = 0;

  // Rotation of the world around the device: v_device = orientation * v_world.
  Common::Quaternion orientation = Common::Quaternion::Identity();
  // Pitch (nose down +), roll (left +), yaw (left +), radians.
  Common::Vec3 angles{};

  // The VR controller driving an OpenXR Wii Remote, in the same frame and convention, with its
  // yaw aligned to the fused orientation at the last reset.
  bool ghost_valid = false;
  Common::Quaternion ghost_orientation = Common::Quaternion::Identity();
  Common::Vec3 ghost_angles{};
  bool ghost_left_hand = false;
  bool ghost_distance_valid = false;
  float ghost_distance_m = 0;

  WiimoteCommon::IRReportFormat ir_format = WiimoteCommon::IRReportFormat::None;
  std::array<IRDot, 4> ir_dots{};
  int ir_visible = 0;
  // Mean of the visible dots, -1..+1 (pixel space: +x to the right, +y down in the image).
  Common::Vec2 ir_center{};
  // Direction from the remote to the sensor bar, body frame.
  Common::Vec3 ir_direction{0, -1, 0};
  // Pointing angles relative to the sensor bar implied by the IR dots, radians.
  float ir_yaw = 0;
  float ir_pitch = 0;
  bool ir_roll_valid = false;
  float ir_roll = 0;
  // Distance to the sensor bar from the dot spacing (held while the dots are lost).
  bool ir_distance_valid = false;
  float ir_distance_m = 2;

  CalibrationSource accel_calibration = CalibrationSource::Default;
  CalibrationSource motion_plus_calibration = CalibrationSource::Default;
  std::array<u16, 3> accel_zero_g{};
  std::array<u16, 3> accel_one_g{};

  u64 sample_seq = 0;
};

// The emulated Wii Remote's HID source changed. Resets everything, calibration included.
void OnSourceChanged(u32 slot, bool has_source);
// The remote disconnected. Resets the motion state and pending reads, keeps the calibration.
void OnDisconnect(u32 slot);
// A savestate was loaded: the MotionPlus state is no longer known.
void OnStateLoaded(u32 slot);
// An output report from the game. payload[0] is the OutputReportID.
void OnOutputReport(u32 slot, const u8* payload, u32 size);
// An input report to the game. report[0] is the InputReportID.
void OnInputReport(u32 slot, const u8* report, u32 size, u64 ticks, u64 ticks_per_second);

// Data reports are decoded only while enabled. Protocol tracking (reporting mode, MotionPlus
// activation, calibration reads) always runs, so a tester opened mid-game knows them.
void SetEnabled(u32 slot, bool enabled);
bool IsEnabled(u32 slot);
// Correct yaw and pitch from the IR dots, as Dolphin's real Wii Remote input device does.
void SetIRCorrection(u32 slot, bool enabled);
// Puts the fused orientation back to gravity-only pitch and roll, with the yaw the IR dots imply
// (zero, pointing at the sensor bar, when they are not visible), and aligns the VR ghost's yaw to
// it. Applied at the next data report.
void RequestOrientationReset(u32 slot);

Snapshot GetSnapshot(u32 slot);
// Appends the samples newer than since_seq (at most the ring size) and returns the newest seq.
u64 CopyNewSamples(u32 slot, u64 since_seq, std::vector<Sample>* out);

// While recording, each data report becomes a CSV row with the columns of GetRecordingColumns():
// what the game received, the tester's decoding of it and, for the OpenXR source, the VR
// controller data and the input override's output behind it. Reports are decoded while
// recording even if the tester is disabled. Times start at the first recorded report.
void SetRecording(u32 slot, bool recording);
bool IsRecording(u32 slot);
// Moves the rows recorded since the last call to the end of out. Returns how many there were.
u64 TakeRecordedRows(u32 slot, std::string* out);
// The CSV header line, newline included.
std::string GetRecordingColumns();

// Radians: pitch (nose down +), roll (left +), yaw (left +) of a world-around-device rotation.
Common::Vec3 GetAngles(const Common::Quaternion& world_around_device);
}  // namespace WiimoteMotionTracker
