// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteMotionTracker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/BitUtils.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

namespace WiimoteMotionTracker
{
namespace
{
using WiimoteCommon::AccelCalibrationData;
using WiimoteCommon::AccelData;
using WiimoteCommon::AddressSpace;
using WiimoteCommon::ErrorCode;
using WiimoteCommon::InputReportID;
using WiimoteCommon::IRReportFormat;
using WiimoteCommon::OutputReportID;
using WiimoteEmu::CameraLogic;
using WiimoteEmu::MotionPlus;

// 6 seconds of 200 Hz reports.
constexpr size_t RING_SIZE = 1200;
// Longer gaps (pause, savestate load) are not integrated.
constexpr float MAX_DT = 0.1f;

constexpr u16 EEPROM_ACCEL_CALIBRATION_ADDRESSES[] = {0x16, 0x20};
constexpr u16 MOTION_PLUS_CALIBRATION_ADDRESS = 0x20;
constexpr u16 EXT_ID_ADDRESS = 0xfa;
constexpr u8 MOTION_PLUS_INIT_TRIGGER_ADDRESS = 0xf0;
constexpr u8 MOTION_PLUS_ID_BYTE = 0x05;

// Gravity correction weights per report, as ciface::WiimoteController::Device::UpdateOrientation.
constexpr float ACCEL_WEIGHT_WITH_GYRO = 0.04f;
constexpr float ACCEL_WEIGHT_WITHOUT_GYRO = 0.5f;
constexpr float IR_WEIGHT_WITH_GYRO = 0.035f;
constexpr float IR_WEIGHT_WITHOUT_GYRO = 0.45f;
// The gravity correction fades out as |a| leaves 1 g by this much.
constexpr float ACCEL_TRUST_RANGE_G = 0.5f;

// Consecutive MotionPlus-looking frames before an unknown MotionPlus state is assumed active.
constexpr int ASSUME_MOTION_PLUS_FRAMES = 20;
constexpr int ASSUME_MOTION_PLUS_RANGE = 3000;

constexpr float DEFAULT_IR_DISTANCE = 2.0f;

// WiimoteEmu::ComplementaryFilter, guarded: it normalizes a zero cross product into NaN when the
// prediction is exactly parallel to the measurement (a level remote turning in yaw only, which
// emulated data produces). orientation is the rotation of the world around the device.
Common::Quaternion ComplementaryFilter(const Common::Quaternion& orientation,
                                       const Common::Vec3& measured, float weight,
                                       const Common::Vec3& world_normal)
{
  const Common::Vec3 predicted = orientation * world_normal;
  const Common::Vec3 target = measured.Normalized();
  const Common::Vec3 axis = predicted.Cross(target);
  const float axis_length = axis.Length();
  if (!(axis_length > 1e-6f))
    return orientation;
  const float angle = std::atan2(axis_length, predicted.Dot(target));
  return Common::Quaternion::Rotate(angle * weight, axis / axis_length) * orientation;
}

// The world-around-device rotation with zero yaw that maps world up to the measured gravity.
Common::Quaternion OrientationFromGravity(const Common::Vec3& accel)
{
  const Common::Vec3 up(0, 0, 1);
  const Common::Vec3 target = accel.Normalized();
  const Common::Vec3 axis = up.Cross(target);
  const float axis_length = axis.Length();
  const float angle = std::atan2(axis_length, up.Dot(target));
  if (!(axis_length > 1e-6f))
  {
    return angle < 1 ? Common::Quaternion::Identity() :
                       Common::Quaternion::Rotate(float(MathUtil::PI), Common::Vec3(1, 0, 0));
  }
  return Common::Quaternion::Rotate(angle, axis / axis_length);
}

AccelCalibrationData::Calibration GetDefaultAccelCalibration()
{
  constexpr u16 zero = WiimoteEmu::Wiimote::ACCEL_ZERO_G << 2;
  constexpr u16 one_g = WiimoteEmu::Wiimote::ACCEL_ONE_G << 2;
  return AccelCalibrationData::Calibration({zero, zero, zero}, {one_g, one_g, one_g});
}

// The emulated MotionPlus calibration (MotionPlus::Reset).
MotionPlus::CalibrationBlocks GetDefaultMotionPlusCalibration()
{
  MotionPlus::CalibrationBlock block{};
  block.yaw_zero = MotionPlus::CALIBRATION_ZERO;
  block.roll_zero = MotionPlus::CALIBRATION_ZERO;
  block.pitch_zero = MotionPlus::CALIBRATION_ZERO;
  block.yaw_scale = u16(MotionPlus::CALIBRATION_ZERO - MotionPlus::CALIBRATION_SCALE_OFFSET);
  block.roll_scale = u16(MotionPlus::CALIBRATION_ZERO + MotionPlus::CALIBRATION_SCALE_OFFSET);
  block.pitch_scale = u16(MotionPlus::CALIBRATION_ZERO - MotionPlus::CALIBRATION_SCALE_OFFSET);

  MotionPlus::CalibrationBlocks blocks{block, block};
  blocks.fast.degrees_div_6 = MotionPlus::CALIBRATION_FAST_SCALE_DEGREES / 6;
  blocks.slow.degrees_div_6 = MotionPlus::CALIBRATION_SLOW_SCALE_DEGREES / 6;
  return blocks;
}

bool IsUsableMotionPlusBlock(const MotionPlus::CalibrationBlock& block)
{
  return block.degrees_div_6 != 0 && u16(block.yaw_scale) != u16(block.yaw_zero) &&
         u16(block.roll_scale) != u16(block.roll_zero) &&
         u16(block.pitch_scale) != u16(block.pitch_zero);
}

bool IsI2CSpace(u8 space)
{
  return space == u8(AddressSpace::I2CBus) || space == u8(AddressSpace::I2CBusAlt);
}

bool IsMotionPlusSlave(u8 slave)
{
  return slave == MotionPlus::ACTIVE_DEVICE_ADDR || slave == MotionPlus::INACTIVE_DEVICE_ADDR;
}

bool IsValidPassthroughMode(u8 mode)
{
  return mode == u8(MotionPlus::PassthroughMode::Disabled) ||
         mode == u8(MotionPlus::PassthroughMode::Nunchuk) ||
         mode == u8(MotionPlus::PassthroughMode::Classic);
}

struct PendingWrite
{
  bool valid = false;
  u8 space = 0;
  u8 slave = 0;
  u8 address = 0;
  u8 size = 0;
  std::array<u8, 16> data{};
};

struct PendingRead
{
  bool valid = false;
  u8 space = 0;
  u8 slave = 0;
  u16 address = 0;
  u16 size = 0;
  u16 received = 0;
  // The first bytes of the read, which hold every block the tracker parses.
  std::array<u8, 0x100> data{};
  bool accel_calibration_done = false;
  bool motion_plus_calibration_done = false;
  bool ext_id_done = false;
};

struct SlotState
{
  std::atomic<bool> enabled{false};
  std::atomic<bool> restart{true};
  std::atomic<bool> ir_correction{false};
  std::atomic<bool> reset_requested{false};
  std::atomic<bool> recording{false};
  std::atomic<bool> recording_restart{false};

  // CPU thread: protocol tracking.
  bool has_source = false;
  InputReportID requested_mode = InputReportID::ReportCore;
  bool continuous = false;
  MotionPlusState motion_plus = MotionPlusState::Inactive;
  u8 motion_plus_mode = 0;
  int motion_plus_like_frames = 0;
  bool extension_bit = false;
  bool ext_id_valid = false;
  std::array<u8, 6> ext_id{};
  PendingWrite pending_write;
  PendingRead pending_read;
  AccelCalibrationData::Calibration accel_calibration = GetDefaultAccelCalibration();
  CalibrationSource accel_calibration_source = CalibrationSource::Default;
  MotionPlus::CalibrationBlocks motion_plus_calibration = GetDefaultMotionPlusCalibration();
  CalibrationSource motion_plus_calibration_source = CalibrationSource::Default;

  // CPU thread: decoding, while enabled.
  u64 start_ticks = 0;
  u64 last_ticks = 0;
  u64 last_unique_ticks = 0;
  bool have_gyro_ticks = false;
  u64 last_gyro_ticks = 0;
  float mean_dt = 0;
  float mean_unique_dt = 0;
  u64 report_count = 0;
  std::array<u8, WiimoteCommon::MAX_PAYLOAD> previous_report{};
  u8 previous_report_size = 0;

  AccelData accel_raw{WiimoteCommon::AccelType{512, 512, 616}};
  bool has_gyro = false;
  Common::Vec3 gyro{};
  std::array<u16, 3> gyro_raw{};
  std::array<bool, 3> gyro_slow{};
  bool orientation_initialized = false;
  Common::Quaternion orientation = Common::Quaternion::Identity();
  bool ghost_offset_valid = false;
  float ghost_yaw_offset = 0;
  std::array<IRDot, 4> ir_dots{};
  bool ir_distance_valid = false;
  float ir_distance = DEFAULT_IR_DISTANCE;

  // CPU thread, while recording.
  u64 recording_start_ticks = 0;
  std::chrono::steady_clock::time_point recording_start_time{};
  bool have_recording_xr_time = false;
  s64 recording_start_xr_time = 0;

  // Published to the UI.
  std::mutex mutex;
  Snapshot snapshot;
  std::vector<Sample> ring;
  u64 seq = 0;
  std::string recorded_rows;
  u64 recorded_row_count = 0;
};

std::array<SlotState, MAX_WIIMOTES> s_slots;

SlotState* GetSlot(u32 slot)
{
  return slot < s_slots.size() ? &s_slots[slot] : nullptr;
}

std::array<u16, 3> ToArray(const WiimoteCommon::AccelType& value)
{
  return {value.x, value.y, value.z};
}

// Copies the protocol state into the snapshot. Hold the slot mutex.
void PublishProtocolLocked(SlotState& s)
{
  Snapshot& snapshot = s.snapshot;
  snapshot.has_source = s.has_source;
  snapshot.requested_mode = s.requested_mode;
  snapshot.continuous = s.continuous;
  snapshot.motion_plus = s.motion_plus;
  snapshot.motion_plus_mode = s.motion_plus_mode;
  snapshot.extension_bit = s.extension_bit;
  snapshot.ext_id_valid = s.ext_id_valid;
  snapshot.ext_id = s.ext_id;
  snapshot.accel_calibration = s.accel_calibration_source;
  snapshot.motion_plus_calibration = s.motion_plus_calibration_source;
  snapshot.accel_zero_g = ToArray(s.accel_calibration.zero);
  snapshot.accel_one_g = ToArray(s.accel_calibration.max);
}

void PublishProtocol(SlotState& s)
{
  std::lock_guard lock(s.mutex);
  PublishProtocolLocked(s);
}

void ResetMotion(SlotState& s)
{
  s.restart = true;
  s.pending_read = {};
  s.pending_write = {};
}

void SetMotionPlusState(SlotState& s, MotionPlusState state, u8 mode)
{
  s.motion_plus = state;
  s.motion_plus_mode = state == MotionPlusState::Inactive ? 0 : mode;
  s.motion_plus_like_frames = 0;
}

void CommitWrite(SlotState& s, const PendingWrite& write)
{
  if (!IsI2CSpace(write.space) || !IsMotionPlusSlave(write.slave))
    return;

  const auto covers = [&write](u8 address) {
    return address >= write.address && address < write.address + write.size;
  };
  const bool motion_plus_active =
      s.motion_plus == MotionPlusState::Active || s.motion_plus == MotionPlusState::AssumedActive;

  if (covers(MotionPlus::PASSTHROUGH_MODE_OFFSET))
  {
    const u8 mode = write.data[MotionPlus::PASSTHROUGH_MODE_OFFSET - write.address];
    // An inactive MotionPlus answers on 0x53 and an active one on 0x52 (MotionPlus::BusWrite).
    // The write was acknowledged, so the MotionPlus is in the state its address implies.
    if (write.slave == MotionPlus::INACTIVE_DEVICE_ADDR || motion_plus_active)
    {
      if (IsValidPassthroughMode(mode))
        SetMotionPlusState(s, MotionPlusState::Active, mode);
      else
        SetMotionPlusState(s, MotionPlusState::Inactive, 0);
    }
  }

  // Any write to 0xf0 deactivates an active MotionPlus. On an inactive one it initializes the
  // passthrough extension instead, so the MotionPlus stays inactive either way.
  if (write.slave == MotionPlus::ACTIVE_DEVICE_ADDR && covers(MOTION_PLUS_INIT_TRIGGER_ADDRESS))
    SetMotionPlusState(s, MotionPlusState::Inactive, 0);
}

void HandleOutputReport(SlotState& s, const u8* payload, u32 size)
{
  if (size < 1)
    return;

  const u8* const report = payload + 1;
  const u32 report_size = size - 1;

  switch (OutputReportID(payload[0]))
  {
  case OutputReportID::ReportMode:
  {
    if (report_size < sizeof(WiimoteCommon::OutputReportMode))
      return;
    const WiimoteCommon::OutputReportMode mode =
        Common::BitCastPtr<WiimoteCommon::OutputReportMode>(report);
    s.requested_mode = mode.mode;
    s.continuous = mode.continuous;
    break;
  }
  case OutputReportID::WriteData:
  {
    // The header plus at least one data byte.
    constexpr u32 HEADER_SIZE = sizeof(WiimoteCommon::OutputReportWriteData) - 16;
    if (report_size < HEADER_SIZE + 1)
      return;
    WiimoteCommon::OutputReportWriteData write{};
    std::copy_n(report, std::min<u32>(report_size, sizeof(write)), reinterpret_cast<u8*>(&write));

    PendingWrite& pending = s.pending_write;
    pending = {};
    if (!IsI2CSpace(write.space) || !IsMotionPlusSlave(write.slave_address))
      return;
    pending.valid = true;
    pending.space = write.space;
    pending.slave = write.slave_address;
    // The i2c bus only uses the low byte of the address.
    pending.address = write.address[1];
    pending.size = std::min<u8>(write.size, u8(std::min<u32>(16, report_size - HEADER_SIZE)));
    std::copy_n(write.data, pending.size, pending.data.data());
    break;
  }
  case OutputReportID::ReadData:
  {
    if (report_size < sizeof(WiimoteCommon::OutputReportReadData))
      return;
    const WiimoteCommon::OutputReportReadData read =
        Common::BitCastPtr<WiimoteCommon::OutputReportReadData>(report);

    PendingRead& pending = s.pending_read;
    pending = {};
    pending.valid = true;
    pending.space = read.space;
    pending.slave = read.slave_address;
    pending.address = Common::swap16(read.address);
    pending.size = Common::swap16(read.size);
    break;
  }
  default:
    break;
  }
}

void ParseAccelCalibration(SlotState& s, const u8* data)
{
  std::array<u8, sizeof(AccelCalibrationData)> bytes;
  std::copy_n(data, bytes.size(), bytes.begin());
  const AccelCalibrationData calibration = Common::BitCastPtr<AccelCalibrationData>(bytes.data());

  WiimoteEmu::UpdateCalibrationDataChecksum(bytes, 1);
  if (calibration.checksum != bytes.back())
  {
    // Keep the previous calibration.
    if (s.accel_calibration_source != CalibrationSource::Device)
      s.accel_calibration_source = CalibrationSource::DeviceBadChecksum;
    return;
  }

  const auto result = calibration.GetCalibration();
  if (result.zero.x == result.max.x || result.zero.y == result.max.y ||
      result.zero.z == result.max.z)
  {
    s.accel_calibration_source = CalibrationSource::DeviceBadChecksum;
    return;
  }
  s.accel_calibration = result;
  s.accel_calibration_source = CalibrationSource::Device;
}

void ParseMotionPlusCalibration(SlotState& s, const u8* data)
{
  MotionPlus::CalibrationData calibration = Common::BitCastPtr<MotionPlus::CalibrationData>(data);
  const auto read_checksum = std::pair(u16(calibration.crc32_lsb), u16(calibration.crc32_msb));
  calibration.UpdateChecksum();
  const bool checksum_ok =
      read_checksum == std::pair(u16(calibration.crc32_lsb), u16(calibration.crc32_msb));

  // Like ciface::WiimoteController, use the data even with a bad checksum, but never values that
  // would divide by zero.
  if (!IsUsableMotionPlusBlock(calibration.fast) || !IsUsableMotionPlusBlock(calibration.slow))
  {
    s.motion_plus_calibration_source = CalibrationSource::DeviceBadChecksum;
    return;
  }
  s.motion_plus_calibration.fast = calibration.fast;
  s.motion_plus_calibration.slow = calibration.slow;
  s.motion_plus_calibration_source =
      checksum_ok ? CalibrationSource::Device : CalibrationSource::DeviceBadChecksum;
}

void ParseExtensionID(SlotState& s, u8 slave, const u8* data)
{
  std::copy_n(data, s.ext_id.size(), s.ext_id.begin());
  s.ext_id_valid = true;

  if (data[5] == MOTION_PLUS_ID_BYTE && slave == MotionPlus::ACTIVE_DEVICE_ADDR)
  {
    SetMotionPlusState(s, MotionPlusState::Active, data[4]);
  }
  else if (slave == MotionPlus::INACTIVE_DEVICE_ADDR || data[5] != MOTION_PLUS_ID_BYTE)
  {
    // A MotionPlus answering on 0x53, or a normal extension on 0x52.
    SetMotionPlusState(s, MotionPlusState::Inactive, 0);
  }
}

// Parses the blocks of interest once the read has delivered them.
void ProcessReadProgress(SlotState& s)
{
  PendingRead& read = s.pending_read;
  const auto available = [&read](u16 address, size_t size) {
    if (address < read.address)
      return false;
    const size_t offset = size_t(address - read.address);
    return offset + size <= read.received && offset + size <= read.data.size();
  };
  const auto at = [&read](u16 address) { return read.data.data() + (address - read.address); };

  if (read.space == u8(AddressSpace::EEPROM))
  {
    if (read.accel_calibration_done)
      return;
    for (const u16 address : EEPROM_ACCEL_CALIBRATION_ADDRESSES)
    {
      if (!available(address, sizeof(AccelCalibrationData)))
        continue;
      ParseAccelCalibration(s, at(address));
      read.accel_calibration_done = true;
      if (s.accel_calibration_source == CalibrationSource::Device)
        break;
    }
    return;
  }

  if (!IsI2CSpace(read.space) || !IsMotionPlusSlave(read.slave))
    return;

  // 0x52 answers for the MotionPlus only while it is active. Otherwise it is the passthrough
  // extension, whose own calibration also sits at 0x20.
  const bool is_motion_plus = read.slave == MotionPlus::INACTIVE_DEVICE_ADDR ||
                              s.motion_plus == MotionPlusState::Active ||
                              s.motion_plus == MotionPlusState::AssumedActive;
  if (is_motion_plus && !read.motion_plus_calibration_done &&
      available(MOTION_PLUS_CALIBRATION_ADDRESS, sizeof(MotionPlus::CalibrationData)))
  {
    ParseMotionPlusCalibration(s, at(MOTION_PLUS_CALIBRATION_ADDRESS));
    read.motion_plus_calibration_done = true;
  }

  if (!read.ext_id_done && available(EXT_ID_ADDRESS, 6))
  {
    ParseExtensionID(s, read.slave, at(EXT_ID_ADDRESS));
    read.ext_id_done = true;
  }
}

void HandleReadDataReply(SlotState& s, const u8* report, u32 report_size)
{
  if (report_size < sizeof(WiimoteCommon::InputReportReadDataReply))
    return;
  const WiimoteCommon::InputReportReadDataReply reply =
      Common::BitCastPtr<WiimoteCommon::InputReportReadDataReply>(report);

  PendingRead& read = s.pending_read;
  if (!read.valid)
    return;

  // The reply only carries the low 16 bits of the address, so it must continue the request.
  const u16 address = Common::swap16(reply.address);
  if (reply.error != 0 || address != u16(read.address + read.received))
  {
    read = {};
    return;
  }

  const u16 count = reply.size_minus_one + 1;
  const u16 offset = read.received;
  if (offset < read.data.size())
  {
    std::copy_n(reply.data, std::min<size_t>(count, read.data.size() - offset),
                read.data.data() + offset);
  }
  read.received += count;

  ProcessReadProgress(s);

  if (read.received >= read.size)
    read = {};
}

void HandleProtocolInputReport(SlotState& s, const u8* report, u32 size)
{
  const u8* const payload = report + 1;
  const u32 payload_size = size - 1;

  switch (InputReportID(report[0]))
  {
  case InputReportID::Status:
  {
    if (payload_size < sizeof(WiimoteCommon::InputReportStatus))
      return;
    const WiimoteCommon::InputReportStatus status =
        Common::BitCastPtr<WiimoteCommon::InputReportStatus>(payload);
    s.extension_bit = status.extension;
    break;
  }
  case InputReportID::ReadDataReply:
    HandleReadDataReply(s, payload, payload_size);
    break;
  case InputReportID::Ack:
  {
    if (payload_size < sizeof(WiimoteCommon::InputReportAck))
      return;
    const WiimoteCommon::InputReportAck ack =
        Common::BitCastPtr<WiimoteCommon::InputReportAck>(payload);
    if (ack.rpt_id == OutputReportID::WriteData)
    {
      const PendingWrite write = s.pending_write;
      s.pending_write = {};
      if (write.valid && ack.error_code == ErrorCode::Success)
        CommitWrite(s, write);
    }
    else if (ack.rpt_id == OutputReportID::ReadData && ack.error_code != ErrorCode::Success)
    {
      s.pending_read = {};
    }
    break;
  }
  default:
    break;
  }
}

IRDot MakeDot(WiimoteEmu::IRObject position, u8 size)
{
  IRDot dot;
  // Hidden dots are 0xff-filled.
  dot.visible = position.y < CameraLogic::CAMERA_RES_Y && position.x < CameraLogic::CAMERA_RES_X;
  dot.x = position.x;
  dot.y = position.y;
  dot.size = size;
  return dot;
}

void DecodeIRDots(SlotState& s, const WiimoteCommon::DataReportManipulator& manipulator)
{
  const u8* const ir = manipulator.GetIRDataPtr();
  switch (manipulator.GetIRReportFormat())
  {
  case IRReportFormat::Basic:
    for (size_t i = 0; i != 2; ++i)
    {
      const WiimoteEmu::IRBasic block =
          Common::BitCastPtr<WiimoteEmu::IRBasic>(ir + i * sizeof(WiimoteEmu::IRBasic));
      // Basic reports carry no size. Assume a typical one, as WiimoteController does.
      s.ir_dots[i * 2] = MakeDot(block.GetObject1(), 2);
      s.ir_dots[i * 2 + 1] = MakeDot(block.GetObject2(), 2);
    }
    break;
  case IRReportFormat::Extended:
    for (size_t i = 0; i != 4; ++i)
    {
      const WiimoteEmu::IRExtended object =
          Common::BitCastPtr<WiimoteEmu::IRExtended>(ir + i * sizeof(WiimoteEmu::IRExtended));
      s.ir_dots[i] = MakeDot(object.GetPosition(), object.size);
    }
    break;
  case IRReportFormat::Full1:
  case IRReportFormat::Full2:
  {
    // Interleaved reports carry two objects each: 0-1, then 2-3.
    const size_t first = manipulator.GetIRReportFormat() == IRReportFormat::Full1 ? 0 : 2;
    for (size_t i = 0; i != 2; ++i)
    {
      const WiimoteEmu::IRFull object =
          Common::BitCastPtr<WiimoteEmu::IRFull>(ir + i * sizeof(WiimoteEmu::IRFull));
      s.ir_dots[first + i] = MakeDot(object.GetPosition(), object.size);
    }
    break;
  }
  default:
    break;
  }
}

struct IRResult
{
  int visible = 0;
  Common::Vec2 center{};
  Common::Vec3 direction{0, -1, 0};
  float yaw = 0;
  float pitch = 0;
  bool roll_valid = false;
  float roll = 0;
};

IRResult AnalyzeIRDots(SlotState& s)
{
  IRResult result;
  Common::Vec2 sum{};
  std::array<Common::Vec2, 4> points{};
  for (const IRDot& dot : s.ir_dots)
  {
    if (!dot.visible)
      continue;
    points[result.visible] = Common::Vec2(dot.x, dot.y);
    sum += points[result.visible];
    ++result.visible;
  }
  if (result.visible == 0)
    return result;

  const Common::Vec2 mean = sum / float(result.visible);
  const Common::Vec2 camera_max(CameraLogic::CAMERA_RES_X - 1, CameraLogic::CAMERA_RES_Y - 1);
  result.center = mean / camera_max * 2.f - Common::Vec2(1, 1);

  // Dolphin's camera model (CameraLogic::GetCameraPoints): body +X (left) maps to small pixel x
  // and body +Z (up) to large pixel y.
  const float tan_x = std::tan(CameraLogic::CAMERA_FOV_X / 2);
  const float tan_y = std::tan(CameraLogic::CAMERA_FOV_Y / 2);
  result.yaw = std::atan(result.center.x * tan_x);
  result.pitch = std::atan(result.center.y * tan_y);
  result.direction =
      Common::Vec3(-result.center.x * tan_x, -1, result.center.y * tan_y).Normalized();

  if (result.visible == 2)
  {
    auto left = points[0];
    auto right = points[1];
    if (right.x < left.x)
      std::swap(left, right);
    // Pixel y grows upward in the player's view, so a remote rolled left sees the bar turned
    // clockwise.
    result.roll = std::atan2(-(right.y - left.y), right.x - left.x);
    result.roll_valid = true;
  }

  if (result.visible >= 2)
  {
    // Same estimate as ciface::WiimoteController::Device::IRState::ProcessData.
    Common::Vec2 variance{};
    for (int i = 0; i != result.visible; ++i)
    {
      const Common::Vec2 delta = points[i] - mean;
      variance += delta * delta;
    }
    variance /= float(result.visible);
    const Common::Vec2 separation =
        Common::Vec2(std::sqrt(variance.x), std::sqrt(variance.y)) /
        Common::Vec2(CameraLogic::CAMERA_RES_X,
                     CameraLogic::CAMERA_RES_Y * CameraLogic::CAMERA_AR) *
        2;
    const float length = separation.Length();
    if (length > 0)
    {
      s.ir_distance = CameraLogic::SENSOR_BAR_LED_SEPARATION / length / 2 / tan_x;
      s.ir_distance_valid = true;
    }
  }

  return result;
}

void UpdateMotionPlusHeuristic(SlotState& s, const MotionPlus::DataFormat& data)
{
  // An unknown MotionPlus state (savestate load) is resolved by the game's next activation or
  // extension ID read. Until then, frames that look like MotionPlus data imply an active one.
  // Nunchuk data sets these bits too unless Z is held, so this stays labelled as an assumption.
  if (!data.is_mp_data || data.zero)
  {
    if (data.is_mp_data)
      s.motion_plus_like_frames = 0;
    return;
  }

  const auto gyro = data.GetData().gyro.value;
  const auto plausible = [](u16 value) {
    return std::abs(int(value) - MotionPlus::ZERO_VALUE) < ASSUME_MOTION_PLUS_RANGE;
  };
  if (!plausible(gyro.x) || !plausible(gyro.y) || !plausible(gyro.z))
  {
    s.motion_plus_like_frames = 0;
    return;
  }

  if (++s.motion_plus_like_frames >= ASSUME_MOTION_PLUS_FRAMES)
  {
    s.motion_plus = MotionPlusState::AssumedActive;
    s.motion_plus_like_frames = 0;
  }
}

// The recording's columns. Angles are in degrees. The fused orientation is the rotation of the
// world around the device. The xr_ columns are the runtime's raw data in the OpenXR reference
// space (X right, Y up, Z back; quaternions x, y, z, w), the in_ columns what the input override
// fed the emulated remote (acceleration in m/s² along the body axes, gyro about the MotionPlus
// axes, IR cursor -1..+1).
constexpr std::string_view RECORDING_COLUMNS =
    "wall_s,emu_s,report,dup,mplus,"
    "accel_x_g,accel_y_g,accel_z_g,"
    "gyro_new,gyro_pitch_dps,gyro_roll_dps,gyro_yaw_dps,"
    "gyro_pitch_raw,gyro_roll_raw,gyro_yaw_raw,gyro_slow,"
    "fused_w,fused_x,fused_y,fused_z,fused_pitch_deg,fused_roll_deg,fused_yaw_deg,"
    "ir_dots,ir_cx,ir_cy,ir_yaw_deg,ir_pitch_deg,ir_dist_m,"
    "ghost_pitch_deg,ghost_roll_deg,ghost_yaw_deg,"
    // RECORDING_XR_COLUMNS of them.
    "xr_gen,xr_time_s,xr_hand,xr_withheld,"
    "xr_aim_qx,xr_aim_qy,xr_aim_qz,xr_aim_qw,"
    "xr_ptr_qx,xr_ptr_qy,xr_ptr_qz,xr_ptr_qw,"
    "xr_grip_qx,xr_grip_qy,xr_grip_qz,xr_grip_qw,xr_grip_x,xr_grip_y,xr_grip_z,"
    "xr_angvel_x,xr_angvel_y,xr_angvel_z,xr_linvel_x,xr_linvel_y,xr_linvel_z,"
    "xr_hit_u,xr_hit_v,xr_hit_dist_m,"
    // RECORDING_INPUT_COLUMNS of them.
    "in_gen,in_accel_x,in_accel_y,in_accel_z,in_gyro_pitch_dps,in_gyro_roll_dps,in_gyro_yaw_dps,"
    "in_ir_x,in_ir_y,in_ir_z";
constexpr int RECORDING_XR_COLUMNS = 28;
constexpr int RECORDING_INPUT_COLUMNS = 10;

class CsvRow
{
public:
  explicit CsvRow(std::string* out) : m_out(out) {}

  void Empty(int count = 1)
  {
    for (int i = 0; i != count; ++i)
      Next();
  }
  void Text(std::string_view text)
  {
    Next();
    m_out->append(text);
  }
  void Integer(s64 value)
  {
    Next();
    fmt::format_to(std::back_inserter(*m_out), "{}", value);
  }
  // Non-finite values (a hidden pointer) are left empty.
  void Number(double value, int precision)
  {
    Next();
    if (std::isfinite(value))
      fmt::format_to(std::back_inserter(*m_out), "{:.{}f}", value, precision);
  }
  void Numbers(const float* values, size_t count, int precision, float scale = 1)
  {
    for (size_t i = 0; i != count; ++i)
      Number(values[i] * scale, precision);
  }
  void Vec3(const Common::Vec3& value, int precision, float scale = 1)
  {
    Numbers(value.data.data(), value.data.size(), precision, scale);
  }
  void End() { m_out->push_back('\n'); }

private:
  void Next()
  {
    if (m_count++ != 0)
      m_out->push_back(',');
  }

  std::string* m_out;
  int m_count = 0;
};

struct RecordedReport
{
  // Seconds since the first recorded report.
  double wall_time = 0;
  double emulated_time = 0;
  bool duplicate = false;
  bool new_gyro = false;
#ifdef ENABLE_VR
  const Wiimote::OpenXRWiiRemotePose* pose = nullptr;
  const Wiimote::OpenXRWiiRemoteInput* input = nullptr;
  // Seconds of the runtime's sample time since the first recorded one.
  double xr_time = 0;
#endif
};

std::string_view GetMotionPlusStateName(MotionPlusState state)
{
  switch (state)
  {
  case MotionPlusState::Active:
    return "active";
  case MotionPlusState::AssumedActive:
    return "assumed";
  case MotionPlusState::Unknown:
    return "unknown";
  default:
    return "inactive";
  }
}

void AppendRecordedRow(const Snapshot& snapshot, const RecordedReport& report, std::string* out)
{
  constexpr float DEGREES = float(360 / MathUtil::TAU);
  CsvRow row(out);

  row.Number(report.wall_time, 6);
  row.Number(report.emulated_time, 6);
  std::string bytes;
  for (size_t i = 0; i != snapshot.last_report_size; ++i)
    fmt::format_to(std::back_inserter(bytes), "{:02X}", snapshot.last_report[i]);
  row.Text(bytes);
  row.Integer(report.duplicate);
  row.Text(GetMotionPlusStateName(snapshot.motion_plus));

  if (snapshot.has_accel)
    row.Vec3(snapshot.accel_g, 4);
  else
    row.Empty(3);

  row.Integer(report.new_gyro);
  if (snapshot.has_gyro)
  {
    row.Vec3(snapshot.gyro_rad_s, 2, DEGREES);
    for (const u16 raw : snapshot.gyro_raw)
      row.Integer(raw);
    std::string ranges;
    for (const bool slow : snapshot.gyro_slow)
      ranges.push_back(slow ? 'S' : 'F');
    row.Text(ranges);
  }
  else
  {
    row.Empty(7);
  }

  const auto& orientation = snapshot.orientation.data;
  row.Number(orientation.w, 6);
  row.Number(orientation.x, 6);
  row.Number(orientation.y, 6);
  row.Number(orientation.z, 6);
  row.Vec3(snapshot.angles, 3, DEGREES);

  row.Integer(snapshot.ir_visible);
  if (snapshot.ir_visible > 0)
  {
    row.Number(snapshot.ir_center.x, 4);
    row.Number(snapshot.ir_center.y, 4);
    row.Number(snapshot.ir_yaw * DEGREES, 3);
    row.Number(snapshot.ir_pitch * DEGREES, 3);
  }
  else
  {
    row.Empty(4);
  }
  // The distance is held while the dots are lost; record it only when measured.
  if (snapshot.ir_distance_valid && snapshot.ir_visible >= 2)
    row.Number(snapshot.ir_distance_m, 3);
  else
    row.Empty();

  if (snapshot.ghost_valid)
    row.Vec3(snapshot.ghost_angles, 3, DEGREES);
  else
    row.Empty(3);

#ifdef ENABLE_VR
  if (const Wiimote::OpenXRWiiRemotePose* const pose = report.pose)
  {
    const Common::VR::OpenXRControllerState& controller = pose->controller;
    row.Integer(s64(pose->generation));
    row.Number(report.xr_time, 6);
    row.Text(pose->left_hand ? "L" : "R");
    row.Integer(pose->input_withheld);
    for (const Common::VR::OpenXRPoseState* const state :
         {&controller.aim_pose, &controller.pointer_pose, &controller.grip_pose})
    {
      if (state->valid)
        row.Numbers(state->orientation.data(), 4, 6);
      else
        row.Empty(4);
    }
    if (controller.grip_pose.valid)
      row.Numbers(controller.grip_pose.position.data(), 3, 4);
    else
      row.Empty(3);
    if (controller.grip_velocity.angular_valid)
      row.Numbers(controller.grip_velocity.angular.data(), 3, 4);
    else
      row.Empty(3);
    if (controller.grip_velocity.linear_valid)
      row.Numbers(controller.grip_velocity.linear.data(), 3, 4);
    else
      row.Empty(3);
    const Common::VR::OpenXRScreenHit& hit = controller.screen_hit;
    if (hit.valid)
    {
      row.Number(hit.u, 4);
      row.Number(hit.v, 4);
      row.Number(hit.distance_m, 3);
    }
    else
    {
      row.Empty(3);
    }
  }
  else
  {
    row.Empty(RECORDING_XR_COLUMNS);
  }

  if (const Wiimote::OpenXRWiiRemoteInput* const input = report.input)
  {
    row.Integer(s64(input->generation));
    row.Vec3(input->acceleration, 3);
    row.Vec3(input->angular_velocity, 2, DEGREES);
    row.Number(input->ir_x, 4);
    row.Number(input->ir_y, 4);
    row.Number(input->ir_z, 3);
  }
  else
  {
    row.Empty(RECORDING_INPUT_COLUMNS);
  }
#else
  row.Empty(RECORDING_XR_COLUMNS + RECORDING_INPUT_COLUMNS);
#endif

  row.End();
}

void StartDecoding(SlotState& s, u64 ticks)
{
  s.start_ticks = ticks;
  s.last_ticks = ticks;
  s.last_unique_ticks = ticks;
  s.have_gyro_ticks = false;
  s.mean_dt = 0;
  s.mean_unique_dt = 0;
  s.report_count = 0;
  s.previous_report_size = 0;
  s.has_gyro = false;
  s.gyro = {};
  s.orientation_initialized = false;
  s.orientation = Common::Quaternion::Identity();
  s.ghost_offset_valid = false;
  s.ir_dots = {};
  s.ir_distance_valid = false;
  s.ir_distance = DEFAULT_IR_DISTANCE;

  // The sample sequence stays monotonic; the UI sees the restart as time going back to zero.
  std::lock_guard lock(s.mutex);
  s.snapshot = {};
  PublishProtocolLocked(s);
  s.snapshot.sample_seq = s.seq;
}

void DecodeDataReport(SlotState& s, u32 slot, const u8* report, u32 size, u64 ticks,
                      u64 ticks_per_second)
{
  const auto id = InputReportID(report[0]);
  if (!WiimoteCommon::DataReportBuilder::IsValidMode(id) || ticks_per_second == 0)
    return;

  std::array<u8, WiimoteCommon::MAX_PAYLOAD> payload{};
  const u32 payload_size = std::min<u32>(size - 1, u32(payload.size()));
  std::copy_n(report + 1, payload_size, payload.data());
  const auto manipulator = WiimoteCommon::MakeDataReportManipulator(id, payload.data());
  if (payload_size < manipulator->GetDataSize())
    return;

  if (s.restart.exchange(false) || ticks < s.last_ticks)
    StartDecoding(s, ticks);

  const double seconds_per_tick = 1.0 / double(ticks_per_second);
  const float dt = std::clamp(float(double(ticks - s.last_ticks) * seconds_per_tick), 0.f, MAX_DT);
  s.last_ticks = ticks;

  // Report rate and duplicates.
  const u8 report_size = u8(std::min<u32>(size, WiimoteCommon::MAX_PAYLOAD));
  const bool duplicate = s.report_count != 0 && report_size == s.previous_report_size &&
                         std::equal(report, report + report_size, s.previous_report.begin());
  std::copy_n(report, report_size, s.previous_report.begin());
  s.previous_report_size = report_size;
  if (s.report_count != 0)
  {
    s.mean_dt = s.mean_dt == 0 ? dt : s.mean_dt + (dt - s.mean_dt) * 0.05f;
    if (!duplicate)
    {
      const float unique_dt =
          std::clamp(float(double(ticks - s.last_unique_ticks) * seconds_per_tick), 0.f, MAX_DT);
      s.mean_unique_dt = s.mean_unique_dt == 0 ?
                             unique_dt :
                             s.mean_unique_dt + (unique_dt - s.mean_unique_dt) * 0.05f;
    }
  }
  if (!duplicate)
    s.last_unique_ticks = ticks;
  ++s.report_count;

  // Accelerometer. Interleaved reports update half of the value each.
  const bool has_accel = manipulator->HasAccel();
  if (has_accel)
    manipulator->GetAccelData(&s.accel_raw);
  const Common::Vec3 accel_g = s.accel_raw.GetNormalizedValue(s.accel_calibration);
  const bool usable_accel = has_accel && accel_g.LengthSquared() > 0;

  // MotionPlus.
  bool new_gyro = false;
  if (manipulator->HasExt() && manipulator->GetExtDataSize() >= sizeof(MotionPlus::DataFormat))
  {
    const MotionPlus::DataFormat data =
        Common::BitCastPtr<MotionPlus::DataFormat>(manipulator->GetExtDataPtr());
    if (s.motion_plus == MotionPlusState::Unknown)
      UpdateMotionPlusHeuristic(s, data);

    const bool motion_plus_active =
        s.motion_plus == MotionPlusState::Active || s.motion_plus == MotionPlusState::AssumedActive;
    // Passthrough modes alternate MotionPlus and extension frames.
    if (motion_plus_active && data.is_mp_data)
    {
      const MotionPlus::DataFormat::Data gyro = data.GetData();
      s.gyro = gyro.GetAngularVelocity(s.motion_plus_calibration);
      s.gyro_raw = {gyro.gyro.value.x, gyro.gyro.value.y, gyro.gyro.value.z};
      s.gyro_slow = {gyro.is_slow.x, gyro.is_slow.y, gyro.is_slow.z};
      s.has_gyro = true;
      new_gyro = true;
    }
  }
  const bool motion_plus_active =
      s.motion_plus == MotionPlusState::Active || s.motion_plus == MotionPlusState::AssumedActive;
  if (!motion_plus_active)
    s.has_gyro = false;

  // IR.
  if (manipulator->HasIR())
    DecodeIRDots(s, *manipulator);
  else
    s.ir_dots = {};
  const IRResult ir = AnalyzeIRDots(s);

  // Fusion, as ciface::WiimoteController::Device::UpdateOrientation. The orientation is the
  // rotation of the world around the device.
  if (s.reset_requested.exchange(false))
    s.orientation_initialized = false;

  bool aligned_now = false;
  if (!s.orientation_initialized)
  {
    // Gravity-only pitch and roll. The yaw points at the sensor bar, or where the IR dots say
    // the remote points relative to it.
    s.orientation = usable_accel ? OrientationFromGravity(accel_g) : Common::Quaternion::Identity();
    if (ir.visible > 0)
    {
      s.orientation = (Common::Quaternion::RotateZ(ir.yaw) * s.orientation.Conjugate())
                          .Normalized()
                          .Conjugate();
    }
    s.orientation_initialized = true;
    aligned_now = true;
  }
  else
  {
    if (new_gyro)
    {
      const float gyro_dt =
          s.have_gyro_ticks ?
              std::clamp(float(double(ticks - s.last_gyro_ticks) * seconds_per_tick), 0.f, MAX_DT) :
              dt;
      s.orientation = WiimoteEmu::GetRotationFromGyroscope(s.gyro * -gyro_dt) * s.orientation;
    }
    if (usable_accel)
    {
      // The accelerometer only measures gravity while the remote is not accelerating: a swing's
      // centripetal push of several g would roll the orientation by tens of degrees.
      const float accel_trust =
          std::clamp(1 - std::abs(accel_g.Length() - 1) / ACCEL_TRUST_RANGE_G, 0.f, 1.f);
      s.orientation = ComplementaryFilter(
          s.orientation, accel_g,
          (motion_plus_active ? ACCEL_WEIGHT_WITH_GYRO : ACCEL_WEIGHT_WITHOUT_GYRO) * accel_trust,
          {0, 0, 1});
    }
    if (s.ir_correction.load(std::memory_order_relaxed) && ir.visible > 0)
    {
      const Common::Vec3 ir_rotation = Common::Vec3(ir.center.y * CameraLogic::CAMERA_FOV_Y, 0,
                                                    ir.center.x * CameraLogic::CAMERA_FOV_X) /
                                       2;
      const Common::Vec3 ir_normal(0, 1, 0);
      const Common::Vec3 ir_vector = WiimoteEmu::GetRotationFromGyroscope(-ir_rotation) * ir_normal;
      s.orientation = ComplementaryFilter(
          s.orientation, ir_vector,
          motion_plus_active ? IR_WEIGHT_WITH_GYRO : IR_WEIGHT_WITHOUT_GYRO, ir_normal);
    }
    s.orientation = s.orientation.Normalized();
  }
  if (new_gyro)
  {
    s.last_gyro_ticks = ticks;
    s.have_gyro_ticks = true;
  }
  const Common::Vec3 angles = GetAngles(s.orientation);

  // The VR controller, sampled at the same instant.
  bool ghost_valid = false;
  Common::Quaternion ghost = Common::Quaternion::Identity();
  bool ghost_left_hand = false;
  bool ghost_distance_valid = false;
  float ghost_distance = 0;
#ifdef ENABLE_VR
  const std::optional<Wiimote::OpenXRWiiRemotePose> pose = Wiimote::GetOpenXRWiiRemotePose(slot);
  if (pose)
  {
    const Common::Quaternion device_to_world = pose->orientation.Normalized();
    if (aligned_now || !s.ghost_offset_valid)
    {
      s.ghost_yaw_offset = GetAngles(device_to_world.Conjugate()).z - angles.z;
      s.ghost_offset_valid = true;
    }
    ghost = (Common::Quaternion::RotateZ(-s.ghost_yaw_offset) * device_to_world)
                .Normalized()
                .Conjugate();
    ghost_valid = true;
    ghost_left_hand = pose->left_hand;
    ghost_distance_valid = pose->screen_distance_valid;
    ghost_distance = pose->screen_distance_m;
  }
  else
  {
    s.ghost_offset_valid = false;
  }
#else
  (void)slot;
  (void)aligned_now;
#endif

  const bool recording = s.recording.load(std::memory_order_relaxed);
  RecordedReport recorded;
#ifdef ENABLE_VR
  std::optional<Wiimote::OpenXRWiiRemoteInput> input;
#endif
  if (recording)
  {
    const auto now = std::chrono::steady_clock::now();
    if (s.recording_restart.exchange(false))
    {
      s.recording_start_ticks = ticks;
      s.recording_start_time = now;
      s.have_recording_xr_time = false;
    }
    recorded.wall_time = std::chrono::duration<double>(now - s.recording_start_time).count();
    // Negative after loading an older savestate.
    recorded.emulated_time = double(s64(ticks - s.recording_start_ticks)) * seconds_per_tick;
    recorded.duplicate = duplicate;
    recorded.new_gyro = new_gyro;
#ifdef ENABLE_VR
    if (pose)
    {
      if (!s.have_recording_xr_time && pose->sample_time_ns != 0)
      {
        s.recording_start_xr_time = pose->sample_time_ns;
        s.have_recording_xr_time = true;
      }
      recorded.xr_time = s.have_recording_xr_time && pose->sample_time_ns != 0 ?
                             double(pose->sample_time_ns - s.recording_start_xr_time) * 1e-9 :
                             std::numeric_limits<double>::quiet_NaN();
      recorded.pose = &*pose;
      input = Wiimote::GetOpenXRWiiRemoteInput(slot);
      if (input)
        recorded.input = &*input;
    }
#endif
  }

  Sample sample;
  sample.time = float(double(ticks - s.start_ticks) * seconds_per_tick);
  sample.accel_g = has_accel ? accel_g : Common::Vec3{};
  sample.gyro_dps = s.has_gyro ? s.gyro * float(360 / MathUtil::TAU) : Common::Vec3{};
  sample.flags = (has_accel ? Sample::HAS_ACCEL : 0) | (new_gyro ? Sample::HAS_GYRO : 0) |
                 (duplicate ? Sample::DUPLICATE : 0);
  if (s.has_gyro)
  {
    sample.flags |= (s.gyro_slow[0] ? Sample::SLOW_PITCH : 0) |
                    (s.gyro_slow[1] ? Sample::SLOW_ROLL : 0) |
                    (s.gyro_slow[2] ? Sample::SLOW_YAW : 0);
  }

  std::lock_guard lock(s.mutex);
  Snapshot& snapshot = s.snapshot;
  PublishProtocolLocked(s);
  snapshot.report_count = s.report_count;
  snapshot.time = sample.time;
  snapshot.report_id = id;
  snapshot.last_report = {};
  std::copy_n(report, report_size, snapshot.last_report.begin());
  snapshot.last_report_size = report_size;
  snapshot.report_rate_hz = s.mean_dt > 0 ? 1 / s.mean_dt : 0;
  snapshot.unique_report_rate_hz = s.mean_unique_dt > 0 ? 1 / s.mean_unique_dt : 0;

  snapshot.has_accel = has_accel;
  snapshot.accel_raw = ToArray(s.accel_raw.value);
  snapshot.accel_g = accel_g;

  snapshot.has_gyro = s.has_gyro;
  snapshot.gyro_raw = s.gyro_raw;
  snapshot.gyro_slow = s.gyro_slow;
  snapshot.gyro_rad_s = s.has_gyro ? s.gyro : Common::Vec3{};
  snapshot.gyro_age =
      s.have_gyro_ticks ? float(double(ticks - s.last_gyro_ticks) * seconds_per_tick) : 0;

  snapshot.orientation = s.orientation;
  snapshot.angles = angles;

  snapshot.ghost_valid = ghost_valid;
  snapshot.ghost_orientation = ghost;
  snapshot.ghost_angles = ghost_valid ? GetAngles(ghost) : Common::Vec3{};
  snapshot.ghost_left_hand = ghost_left_hand;
  snapshot.ghost_distance_valid = ghost_distance_valid;
  snapshot.ghost_distance_m = ghost_distance;

  snapshot.ir_format = manipulator->GetIRReportFormat();
  snapshot.ir_dots = s.ir_dots;
  snapshot.ir_visible = ir.visible;
  snapshot.ir_center = ir.center;
  snapshot.ir_direction = ir.direction;
  snapshot.ir_yaw = ir.yaw;
  snapshot.ir_pitch = ir.pitch;
  snapshot.ir_roll_valid = ir.roll_valid;
  snapshot.ir_roll = ir.roll;
  snapshot.ir_distance_valid = s.ir_distance_valid;
  snapshot.ir_distance_m = s.ir_distance;

  if (!s.ring.empty())
  {
    s.ring[s.seq % s.ring.size()] = sample;
    ++s.seq;
  }
  snapshot.sample_seq = s.seq;

  if (recording)
  {
    AppendRecordedRow(snapshot, recorded, &s.recorded_rows);
    ++s.recorded_row_count;
  }
}
}  // namespace

Common::Vec3 GetAngles(const Common::Quaternion& world_around_device)
{
  // GetRoll and GetYaw are positive to the right.
  return {WiimoteEmu::GetPitch(world_around_device), -WiimoteEmu::GetRoll(world_around_device),
          -WiimoteEmu::GetYaw(world_around_device)};
}

void OnSourceChanged(u32 slot, bool has_source)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return;

  ResetMotion(*s);
  s->has_source = has_source;
  s->requested_mode = InputReportID::ReportCore;
  s->continuous = false;
  SetMotionPlusState(*s, MotionPlusState::Inactive, 0);
  s->extension_bit = false;
  s->ext_id_valid = false;
  s->ext_id = {};
  s->accel_calibration = GetDefaultAccelCalibration();
  s->accel_calibration_source = CalibrationSource::Default;
  s->motion_plus_calibration = GetDefaultMotionPlusCalibration();
  s->motion_plus_calibration_source = CalibrationSource::Default;

  std::lock_guard lock(s->mutex);
  s->snapshot = {};
  PublishProtocolLocked(*s);
}

void OnDisconnect(u32 slot)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return;

  ResetMotion(*s);
  // The game sets the reporting mode and activates the MotionPlus again after reconnecting.
  SetMotionPlusState(*s, MotionPlusState::Inactive, 0);
  s->ext_id_valid = false;
  PublishProtocol(*s);
}

void OnStateLoaded(u32 slot)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return;

  ResetMotion(*s);
  SetMotionPlusState(*s, MotionPlusState::Unknown, 0);
  PublishProtocol(*s);
}

void OnOutputReport(u32 slot, const u8* payload, u32 size)
{
  SlotState* const s = GetSlot(slot);
  if (!s || !payload)
    return;

  HandleOutputReport(*s, payload, size);
}

void OnInputReport(u32 slot, const u8* report, u32 size, u64 ticks, u64 ticks_per_second)
{
  SlotState* const s = GetSlot(slot);
  if (!s || !report || size < 1)
    return;

  if (report[0] < u8(InputReportID::ReportCore))
  {
    // Status, read replies and acks: rare, always tracked.
    HandleProtocolInputReport(*s, report, size);
    if (s->enabled.load(std::memory_order_relaxed))
      PublishProtocol(*s);
    return;
  }

  if (!s->enabled.load(std::memory_order_relaxed) && !s->recording.load(std::memory_order_relaxed))
    return;

  DecodeDataReport(*s, slot, report, size, ticks, ticks_per_second);
}

void SetEnabled(u32 slot, bool enabled)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return;

  {
    std::lock_guard lock(s->mutex);
    if (enabled)
    {
      s->ring.assign(RING_SIZE, Sample{});
      s->seq = 0;
      s->snapshot.sample_seq = 0;
    }
    else
    {
      s->ring.clear();
      s->ring.shrink_to_fit();
    }
  }
  s->restart = true;
  s->enabled = enabled;
}

bool IsEnabled(u32 slot)
{
  const SlotState* const s = GetSlot(slot);
  return s && s->enabled.load(std::memory_order_relaxed);
}

void SetIRCorrection(u32 slot, bool enabled)
{
  if (SlotState* const s = GetSlot(slot))
    s->ir_correction = enabled;
}

void RequestOrientationReset(u32 slot)
{
  if (SlotState* const s = GetSlot(slot))
    s->reset_requested = true;
}

Snapshot GetSnapshot(u32 slot)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return {};

  std::lock_guard lock(s->mutex);
  return s->snapshot;
}

void SetRecording(u32 slot, bool recording)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return;

  if (recording)
  {
    std::lock_guard lock(s->mutex);
    s->recorded_rows.clear();
    s->recorded_row_count = 0;
  }
  s->recording_restart = true;
  s->recording = recording;
}

bool IsRecording(u32 slot)
{
  const SlotState* const s = GetSlot(slot);
  return s && s->recording.load(std::memory_order_relaxed);
}

u64 TakeRecordedRows(u32 slot, std::string* out)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return 0;

  std::lock_guard lock(s->mutex);
  out->append(s->recorded_rows);
  s->recorded_rows.clear();
  return std::exchange(s->recorded_row_count, 0);
}

std::string GetRecordingColumns()
{
  return std::string(RECORDING_COLUMNS) + '\n';
}

u64 CopyNewSamples(u32 slot, u64 since_seq, std::vector<Sample>* out)
{
  SlotState* const s = GetSlot(slot);
  if (!s)
    return since_seq;

  std::lock_guard lock(s->mutex);
  if (s->ring.empty())
    return s->seq;

  const u64 oldest = s->seq > s->ring.size() ? s->seq - s->ring.size() : 0;
  for (u64 i = std::max(since_seq, oldest); i < s->seq; ++i)
    out->push_back(s->ring[i % s->ring.size()]);
  return s->seq;
}
}  // namespace WiimoteMotionTracker
