// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteMotionTracker.h"

using WiimoteCommon::AddressSpace;
using WiimoteCommon::ErrorCode;
using WiimoteCommon::InputReportID;
using WiimoteCommon::OutputReportID;
using WiimoteEmu::CameraLogic;
using WiimoteEmu::MotionPlus;
namespace Tracker = WiimoteMotionTracker;

namespace
{
constexpr u32 SLOT = 0;
constexpr u64 TICKS_PER_SECOND = 1'000'000;
// 200 Hz.
constexpr u64 TICKS_PER_REPORT = TICKS_PER_SECOND / 200;
constexpr float DEG = float(MathUtil::TAU / 360);
constexpr float G = float(MathUtil::GRAVITY_ACCELERATION);

constexpr u8 I2C = u8(AddressSpace::I2CBus);
constexpr u8 EEPROM = u8(AddressSpace::EEPROM);

MotionPlus::DataFormat MakeMotionPlusData(const Common::Vec3& angular_velocity)
{
  // As MotionPlus::PrepareInput.
  const MotionPlus::DataFormat::Data gyro = MotionPlus::GetGyroscopeData(angular_velocity);
  MotionPlus::DataFormat data{};
  data.pitch_slow = gyro.is_slow.x;
  data.roll_slow = gyro.is_slow.y;
  data.yaw_slow = gyro.is_slow.z;
  data.pitch1 = u8(gyro.gyro.value.x);
  data.roll1 = u8(gyro.gyro.value.y);
  data.yaw1 = u8(gyro.gyro.value.z);
  data.pitch2 = u8(gyro.gyro.value.x >> 8);
  data.roll2 = u8(gyro.gyro.value.y >> 8);
  data.yaw2 = u8(gyro.gyro.value.z >> 8);
  data.is_mp_data = 1;
  data.zero = 0;
  return data;
}

MotionPlus::CalibrationData MakeMotionPlusCalibration(u8 slow_degrees_div_6)
{
  // As MotionPlus::Reset.
  MotionPlus::CalibrationData calibration{};
  for (MotionPlus::CalibrationBlock* block : {&calibration.fast, &calibration.slow})
  {
    block->yaw_zero = MotionPlus::CALIBRATION_ZERO;
    block->roll_zero = MotionPlus::CALIBRATION_ZERO;
    block->pitch_zero = MotionPlus::CALIBRATION_ZERO;
    block->yaw_scale = u16(MotionPlus::CALIBRATION_ZERO - MotionPlus::CALIBRATION_SCALE_OFFSET);
    block->roll_scale = u16(MotionPlus::CALIBRATION_ZERO + MotionPlus::CALIBRATION_SCALE_OFFSET);
    block->pitch_scale = u16(MotionPlus::CALIBRATION_ZERO - MotionPlus::CALIBRATION_SCALE_OFFSET);
  }
  calibration.fast.degrees_div_6 = MotionPlus::CALIBRATION_FAST_SCALE_DEGREES / 6;
  calibration.slow.degrees_div_6 = slow_degrees_div_6;
  calibration.uid_1 = 0x0b;
  calibration.uid_2 = 0xe9;
  calibration.UpdateChecksum();
  return calibration;
}

class WiimoteMotionTrackerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    Tracker::OnSourceChanged(SLOT, true);
    Tracker::SetEnabled(SLOT, true);
    Tracker::SetIRCorrection(SLOT, false);
  }

  void TearDown() override
  {
    Tracker::SetEnabled(SLOT, false);
    Tracker::OnSourceChanged(SLOT, false);
  }

  void Output(OutputReportID id, std::vector<u8> bytes)
  {
    bytes.insert(bytes.begin(), u8(id));
    Tracker::OnOutputReport(SLOT, bytes.data(), u32(bytes.size()));
  }

  void Input(InputReportID id, std::vector<u8> bytes)
  {
    bytes.insert(bytes.begin(), u8(id));
    Tracker::OnInputReport(SLOT, bytes.data(), u32(bytes.size()), m_ticks, TICKS_PER_SECOND);
  }

  void Ack(OutputReportID id, ErrorCode error)
  {
    Input(InputReportID::Ack, {0, 0, u8(id), u8(error)});
  }

  void WriteRegister(u8 slave, u8 address, u8 value, ErrorCode result = ErrorCode::Success)
  {
    std::vector<u8> bytes = {u8(I2C << 2), u8(slave << 1), 0x00, address, 1};
    bytes.resize(5 + 16);
    bytes[5] = value;
    Output(OutputReportID::WriteData, bytes);
    Ack(OutputReportID::WriteData, result);
  }

  void ActivateMotionPlus(MotionPlus::PassthroughMode mode)
  {
    WriteRegister(MotionPlus::INACTIVE_DEVICE_ADDR, MotionPlus::PASSTHROUGH_MODE_OFFSET, u8(mode));
  }

  void RequestRead(u8 space, u8 slave, u16 address, u16 size)
  {
    Output(OutputReportID::ReadData, {u8(space << 2), u8(slave << 1), u8(address >> 8), u8(address),
                                      u8(size >> 8), u8(size)});
  }

  void ReadReply(u16 address, const u8* data, u8 count, u8 error = 0)
  {
    std::vector<u8> bytes = {0, 0, u8(((count - 1) << 4) | error), u8(address >> 8), u8(address)};
    bytes.resize(5 + 16);
    std::copy_n(data, count, bytes.begin() + 5);
    Input(InputReportID::ReadDataReply, bytes);
  }

  // Sends a data report and advances the clock by one report.
  void Data(InputReportID id, const Common::Vec3& accel_mps2,
            std::optional<MotionPlus::DataFormat> ext = std::nullopt,
            const std::array<WiimoteEmu::IRObject, 4>& ir = HiddenIR())
  {
    WiimoteCommon::DataReportBuilder report(id);
    report.SetAccelData(WiimoteEmu::ConvertAccelData(
        accel_mps2, WiimoteEmu::Wiimote::ACCEL_ZERO_G << 2, WiimoteEmu::Wiimote::ACCEL_ONE_G << 2));
    if (report.HasExt())
    {
      std::fill_n(report.GetExtDataPtr(), report.GetExtDataSize(), u8(0x80));
      if (ext)
        Common::BitCastPtr<MotionPlus::DataFormat>(report.GetExtDataPtr()) = *ext;
    }
    if (report.HasIR())
    {
      u8* const ir_data = report.GetIRDataPtr();
      std::fill_n(ir_data, report.GetIRDataSize(), u8(0xff));
      if (report.GetIRReportFormat() == WiimoteCommon::IRReportFormat::Basic)
      {
        for (size_t i = 0; i != 2; ++i)
        {
          WiimoteEmu::IRBasic block{};
          block.SetObject1(ir[i * 2]);
          block.SetObject2(ir[i * 2 + 1]);
          Common::BitCastPtr<WiimoteEmu::IRBasic>(ir_data + i * sizeof(block)) = block;
        }
      }
    }
    Tracker::OnInputReport(SLOT, report.GetDataPtr(), report.GetDataSize(), m_ticks,
                           TICKS_PER_SECOND);
    m_ticks += TICKS_PER_REPORT;
  }

  static std::array<WiimoteEmu::IRObject, 4> HiddenIR()
  {
    const WiimoteEmu::IRObject hidden(0x3ff, 0x3ff);
    return {hidden, hidden, hidden, hidden};
  }

  u64 m_ticks = 1'000'000;
};
}  // namespace

TEST_F(WiimoteMotionTrackerTest, ReportModeIsTracked)
{
  Output(OutputReportID::ReportMode, {0b100, u8(InputReportID::ReportCoreAccelIR10Ext6)});
  Data(InputReportID::ReportCoreAccel, {0, 0, G});

  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.requested_mode, InputReportID::ReportCoreAccelIR10Ext6);
  EXPECT_TRUE(snapshot.continuous);
  EXPECT_EQ(snapshot.report_id, InputReportID::ReportCoreAccel);
  EXPECT_EQ(snapshot.report_count, 1u);
}

TEST_F(WiimoteMotionTrackerTest, MotionPlusActivationFollowsAcknowledgedWrites)
{
  // A Nack (no MotionPlus) changes nothing.
  WriteRegister(MotionPlus::INACTIVE_DEVICE_ADDR, MotionPlus::PASSTHROUGH_MODE_OFFSET, 0x05,
                ErrorCode::Nack);
  EXPECT_EQ(Tracker::GetSnapshot(SLOT).motion_plus, Tracker::MotionPlusState::Inactive);

  ActivateMotionPlus(MotionPlus::PassthroughMode::Nunchuk);
  Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.motion_plus, Tracker::MotionPlusState::Active);
  EXPECT_EQ(snapshot.motion_plus_mode, u8(MotionPlus::PassthroughMode::Nunchuk));

  // A mode change on the active address.
  WriteRegister(MotionPlus::ACTIVE_DEVICE_ADDR, MotionPlus::PASSTHROUGH_MODE_OFFSET, 0x04);
  snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.motion_plus, Tracker::MotionPlusState::Active);
  EXPECT_EQ(snapshot.motion_plus_mode, u8(MotionPlus::PassthroughMode::Disabled));

  // Any write to 0xf0 deactivates it.
  WriteRegister(MotionPlus::ACTIVE_DEVICE_ADDR, 0xf0, 0x55);
  EXPECT_EQ(Tracker::GetSnapshot(SLOT).motion_plus, Tracker::MotionPlusState::Inactive);
}

TEST_F(WiimoteMotionTrackerTest, ExtensionIdReadDetectsMotionPlus)
{
  const std::array<u8, 6> active_id = {0x00, 0x00, 0xa4, 0x20, 0x04, 0x05};
  RequestRead(I2C, MotionPlus::ACTIVE_DEVICE_ADDR, 0x00fa, 6);
  ReadReply(0x00fa, active_id.data(), 6);
  Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.motion_plus, Tracker::MotionPlusState::Active);
  EXPECT_EQ(snapshot.motion_plus_mode, 0x04);
  EXPECT_TRUE(snapshot.ext_id_valid);

  const std::array<u8, 6> nunchuk_id = {0x00, 0x00, 0xa4, 0x20, 0x00, 0x00};
  RequestRead(I2C, MotionPlus::ACTIVE_DEVICE_ADDR, 0x00fa, 6);
  ReadReply(0x00fa, nunchuk_id.data(), 6);
  snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.motion_plus, Tracker::MotionPlusState::Inactive);
  EXPECT_EQ(snapshot.ext_id, nunchuk_id);
}

TEST_F(WiimoteMotionTrackerTest, AccelCalibrationIsReadFromEeprom)
{
  WiimoteCommon::AccelCalibrationData calibration{};
  calibration.zero_g.Set({500, 510, 520});
  calibration.one_g.Set({600, 612, 624});
  std::array<u8, 16> bytes{};
  std::copy_n(reinterpret_cast<const u8*>(&calibration), sizeof(calibration), bytes.begin());
  std::array<u8, sizeof(calibration)> checksummed;
  std::copy_n(bytes.begin(), checksummed.size(), checksummed.begin());
  WiimoteEmu::UpdateCalibrationDataChecksum(checksummed, 1);
  bytes[sizeof(calibration) - 1] = checksummed.back();

  // A corrupted block keeps the defaults.
  std::array<u8, 16> corrupted = bytes;
  corrupted[0] ^= 1;
  RequestRead(EEPROM, 0, 0x0016, 0x10);
  ReadReply(0x0016, corrupted.data(), 16);
  Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.accel_calibration, Tracker::CalibrationSource::DeviceBadChecksum);
  EXPECT_EQ(snapshot.accel_zero_g, (std::array<u16, 3>{512, 512, 512}));

  RequestRead(EEPROM, 0, 0x0016, 0x10);
  ReadReply(0x0016, bytes.data(), 16);
  snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.accel_calibration, Tracker::CalibrationSource::Device);
  EXPECT_EQ(snapshot.accel_zero_g, (std::array<u16, 3>{500, 510, 520}));
  EXPECT_EQ(snapshot.accel_one_g, (std::array<u16, 3>{600, 612, 624}));
}

TEST_F(WiimoteMotionTrackerTest, MotionPlusCalibrationIsReadAndUsed)
{
  // The slow block claims twice the emulated range, so the same raw data reads twice as fast.
  const MotionPlus::CalibrationData calibration =
      MakeMotionPlusCalibration(MotionPlus::CALIBRATION_SLOW_SCALE_DEGREES / 6 * 2);
  std::array<u8, sizeof(calibration)> bytes;
  std::copy_n(reinterpret_cast<const u8*>(&calibration), bytes.size(), bytes.begin());

  RequestRead(I2C, MotionPlus::INACTIVE_DEVICE_ADDR, 0x0020, 0x20);
  ReadReply(0x0020, bytes.data(), 16);
  ReadReply(0x0030, bytes.data() + 16, 16);
  EXPECT_EQ(Tracker::GetSnapshot(SLOT).motion_plus_calibration, Tracker::CalibrationSource::Device);

  ActivateMotionPlus(MotionPlus::PassthroughMode::Disabled);
  for (int i = 0; i <= 200; ++i)
    Data(InputReportID::ReportCoreAccelExt16, {0, 0, G}, MakeMotionPlusData({0, 0, 1}));

  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_NEAR(snapshot.gyro_rad_s.z, 2.0f, 0.01f);
  EXPECT_NEAR(snapshot.angles.z, 2.0f, 2 * DEG);
}

TEST_F(WiimoteMotionTrackerTest, BadMotionPlusChecksumIsFlagged)
{
  MotionPlus::CalibrationData calibration =
      MakeMotionPlusCalibration(MotionPlus::CALIBRATION_SLOW_SCALE_DEGREES / 6);
  calibration.uid_1 ^= 0xff;
  std::array<u8, sizeof(calibration)> bytes;
  std::copy_n(reinterpret_cast<const u8*>(&calibration), bytes.size(), bytes.begin());

  RequestRead(I2C, MotionPlus::INACTIVE_DEVICE_ADDR, 0x0020, 0x20);
  ReadReply(0x0020, bytes.data(), 16);
  ReadReply(0x0030, bytes.data() + 16, 16);
  EXPECT_EQ(Tracker::GetSnapshot(SLOT).motion_plus_calibration,
            Tracker::CalibrationSource::DeviceBadChecksum);
}

TEST_F(WiimoteMotionTrackerTest, GravityGivesPitchAndRoll)
{
  // Nose up 30 degrees: gravity's reaction leans toward the IR end (-Y).
  for (int i = 0; i != 400; ++i)
    Data(InputReportID::ReportCoreAccel,
         Common::Vec3(0, -std::sin(30 * DEG), std::cos(30 * DEG)) * G);
  Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_NEAR(snapshot.angles.x, -30 * DEG, 1 * DEG);
  EXPECT_NEAR(snapshot.angles.y, 0, 1 * DEG);
  EXPECT_NEAR(snapshot.accel_g.Length(), 1.0f, 0.03f);

  // Rolled right 20 degrees: gravity's reaction leans toward the left side (+X).
  Tracker::RequestOrientationReset(SLOT);
  for (int i = 0; i != 400; ++i)
    Data(InputReportID::ReportCoreAccel,
         Common::Vec3(std::sin(20 * DEG), 0, std::cos(20 * DEG)) * G);
  snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_NEAR(snapshot.angles.x, 0, 1 * DEG);
  EXPECT_NEAR(snapshot.angles.y, -20 * DEG, 1 * DEG);
}

TEST_F(WiimoteMotionTrackerTest, ConstantYawRateIntegrates)
{
  ActivateMotionPlus(MotionPlus::PassthroughMode::Disabled);

  // One second of 1 rad/s yaw to the left. The first report sets the initial orientation.
  for (int i = 0; i <= 200; ++i)
    Data(InputReportID::ReportCoreAccelExt16, {0, 0, G}, MakeMotionPlusData({0, 0, 1}));

  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_TRUE(snapshot.has_gyro);
  EXPECT_NEAR(snapshot.gyro_rad_s.z, 1.0f, 0.005f);
  EXPECT_NEAR(snapshot.angles.z, 1.0f, 1 * DEG);
  EXPECT_NEAR(snapshot.angles.x, 0, 1 * DEG);
  EXPECT_NEAR(snapshot.report_rate_hz, 200.0f, 1.0f);
}

TEST_F(WiimoteMotionTrackerTest, PitchRateIntegratesNoseDown)
{
  ActivateMotionPlus(MotionPlus::PassthroughMode::Disabled);

  // Nose down 0.5 rad over one second, with matching gravity at the end so the accelerometer
  // correction agrees with the gyro.
  for (int i = 0; i <= 200; ++i)
  {
    const float pitch = 0.5f * float(i) / 200;
    const Common::Vec3 accel = Common::Vec3(0, std::sin(pitch), std::cos(pitch)) * G;
    Data(InputReportID::ReportCoreAccelExt16, accel, MakeMotionPlusData({0.5f, 0, 0}));
  }
  EXPECT_NEAR(Tracker::GetSnapshot(SLOT).angles.x, 0.5f, 1 * DEG);
}

TEST_F(WiimoteMotionTrackerTest, PassthroughAlternationIntegratesOverTheGap)
{
  ActivateMotionPlus(MotionPlus::PassthroughMode::Nunchuk);

  MotionPlus::DataFormat extension_frame{};
  extension_frame.is_mp_data = 0;
  for (int i = 0; i <= 200; ++i)
  {
    Data(InputReportID::ReportCoreAccelExt16, {0, 0, G},
         i % 2 == 0 ? MakeMotionPlusData({0, 0, 1}) : extension_frame);
  }

  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_NEAR(snapshot.angles.z, 1.0f, 1 * DEG);

  std::vector<Tracker::Sample> samples;
  Tracker::CopyNewSamples(SLOT, 0, &samples);
  ASSERT_EQ(samples.size(), 201u);
  EXPECT_TRUE(samples[0].flags & Tracker::Sample::HAS_GYRO);
  EXPECT_FALSE(samples[1].flags & Tracker::Sample::HAS_GYRO);
  EXPECT_NEAR(samples[1].gyro_dps.z, 57.3f, 0.5f);
}

TEST_F(WiimoteMotionTrackerTest, InterleavedReportsMergeTheAccelerometer)
{
  const Common::Vec3 accel = Common::Vec3(0.3f, -0.2f, 0.9f) * G;
  Data(InputReportID::ReportInterleave1, accel);
  Data(InputReportID::ReportInterleave2, accel);

  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_NEAR(snapshot.accel_g.x, 0.3f, 0.04f);
  EXPECT_NEAR(snapshot.accel_g.y, -0.2f, 0.04f);
  EXPECT_NEAR(snapshot.accel_g.z, 0.9f, 0.04f);
}

TEST_F(WiimoteMotionTrackerTest, IRDotsGiveDistanceAndAngles)
{
  const WiimoteEmu::IRObject hidden(0x3ff, 0x3ff);
  Data(InputReportID::ReportCoreAccelIR10Ext6, {0, 0, G}, std::nullopt,
       {WiimoteEmu::IRObject(462, 384), WiimoteEmu::IRObject(562, 384), hidden, hidden});

  Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.ir_visible, 2);
  ASSERT_TRUE(snapshot.ir_distance_valid);
  // Dots 100 pixels apart: standard deviation 50.
  const float expected = CameraLogic::SENSOR_BAR_LED_SEPARATION / (50.0f / 1024 * 2) / 2 /
                         std::tan(CameraLogic::CAMERA_FOV_X / 2);
  EXPECT_NEAR(snapshot.ir_distance_m, expected, 0.01f);
  EXPECT_NEAR(snapshot.ir_yaw, 0, 0.2f * DEG);
  EXPECT_NEAR(snapshot.ir_roll, 0, 0.1f * DEG);

  // The bar seen to the right of the image: the remote points to its left (yaw left +).
  // The right dot higher in the image (larger y): the remote is rolled right.
  Data(InputReportID::ReportCoreAccelIR10Ext6, {0, 0, G}, std::nullopt,
       {WiimoteEmu::IRObject(762, 384), WiimoteEmu::IRObject(862, 434), hidden, hidden});
  snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_GT(snapshot.ir_yaw, 5 * DEG);
  EXPECT_LT(snapshot.ir_roll, -20 * DEG);
  // The bar is to the remote's right (-X) and above (+Z).
  EXPECT_LT(snapshot.ir_direction.x, 0);
  EXPECT_GT(snapshot.ir_direction.z, 0);

  // Hidden dots keep the last distance.
  Data(InputReportID::ReportCoreAccelIR10Ext6, {0, 0, G});
  snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.ir_visible, 0);
  EXPECT_TRUE(snapshot.ir_distance_valid);
}

TEST_F(WiimoteMotionTrackerTest, ResetTakesTheYawFromTheIRDots)
{
  // The bar in the right half of the image: the remote points to its left.
  const WiimoteEmu::IRObject hidden(0x3ff, 0x3ff);
  Data(InputReportID::ReportCoreAccelIR10Ext6, {0, 0, G}, std::nullopt,
       {WiimoteEmu::IRObject(762, 384), WiimoteEmu::IRObject(862, 384), hidden, hidden});

  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_GT(snapshot.ir_yaw, 5 * DEG);
  EXPECT_NEAR(snapshot.angles.z, snapshot.ir_yaw, 0.1f * DEG);
  EXPECT_NEAR(snapshot.angles.x, 0, 0.1f * DEG);
}

TEST_F(WiimoteMotionTrackerTest, DuplicateReportsAreFlagged)
{
  Data(InputReportID::ReportCoreAccel, {0, 0, G});
  Data(InputReportID::ReportCoreAccel, {0, 0, G});
  Data(InputReportID::ReportCoreAccel, {0, 0.5f * G, G});

  std::vector<Tracker::Sample> samples;
  const u64 seq = Tracker::CopyNewSamples(SLOT, 0, &samples);
  ASSERT_EQ(samples.size(), 3u);
  EXPECT_EQ(seq, 3u);
  EXPECT_FALSE(samples[0].flags & Tracker::Sample::DUPLICATE);
  EXPECT_TRUE(samples[1].flags & Tracker::Sample::DUPLICATE);
  EXPECT_FALSE(samples[2].flags & Tracker::Sample::DUPLICATE);
  EXPECT_NEAR(samples[2].time, 0.01f, 1e-4f);

  samples.clear();
  EXPECT_EQ(Tracker::CopyNewSamples(SLOT, seq, &samples), seq);
  EXPECT_TRUE(samples.empty());
}

TEST_F(WiimoteMotionTrackerTest, DisabledSkipsDecodingButTracksProtocol)
{
  Tracker::SetEnabled(SLOT, false);
  Data(InputReportID::ReportCoreAccel, {0, 0, G});
  ActivateMotionPlus(MotionPlus::PassthroughMode::Classic);

  Tracker::SetEnabled(SLOT, true);
  EXPECT_EQ(Tracker::GetSnapshot(SLOT).report_count, 0u);

  Data(InputReportID::ReportCoreAccelExt16, {0, 0, G}, MakeMotionPlusData({0, 0, 0}));
  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.report_count, 1u);
  EXPECT_EQ(snapshot.motion_plus, Tracker::MotionPlusState::Active);
  EXPECT_EQ(snapshot.motion_plus_mode, u8(MotionPlus::PassthroughMode::Classic));
}

TEST_F(WiimoteMotionTrackerTest, SavestateLoadAssumesMotionPlusFromItsData)
{
  ActivateMotionPlus(MotionPlus::PassthroughMode::Disabled);
  Tracker::OnStateLoaded(SLOT);
  EXPECT_EQ(Tracker::GetSnapshot(SLOT).motion_plus, Tracker::MotionPlusState::Unknown);

  for (int i = 0; i != 30; ++i)
    Data(InputReportID::ReportCoreAccelExt16, {0, 0, G}, MakeMotionPlusData({0, 0, 0.2f}));
  const Tracker::Snapshot snapshot = Tracker::GetSnapshot(SLOT);
  EXPECT_EQ(snapshot.motion_plus, Tracker::MotionPlusState::AssumedActive);
  EXPECT_TRUE(snapshot.has_gyro);
}

TEST(WiimoteMotionTrackerAngles, SignsFollowTheRightHandRule)
{
  // Rotations of the device into the world; the tracker stores their inverse.
  const auto angles = [](const Common::Quaternion& device_to_world) {
    return Tracker::GetAngles(device_to_world.Conjugate());
  };
  EXPECT_NEAR(angles(Common::Quaternion::RotateX(0.3f)).x, 0.3f, 1e-4f);
  EXPECT_NEAR(angles(Common::Quaternion::RotateY(0.3f)).y, 0.3f, 1e-4f);
  EXPECT_NEAR(angles(Common::Quaternion::RotateZ(0.3f)).z, 0.3f, 1e-4f);
}

#ifdef ENABLE_VR
TEST(WiimoteMotionTrackerAngles, OpenXRControllerPoseMapsToTheWiiRemoteFrame)
{
  // OpenXR: X right, Y up, Z back (toward the player).
  const auto angles = [](const Common::Quaternion& openxr_rotation) {
    return Tracker::GetAngles(Wiimote::OpenXRToWiimoteRotation(openxr_rotation).Conjugate());
  };
  // Aiming 30 degrees up is a nose-up pitch.
  EXPECT_NEAR(angles(Common::Quaternion::RotateX(30 * DEG)).x, -30 * DEG, 1e-3f);
  // Turning 20 degrees left.
  EXPECT_NEAR(angles(Common::Quaternion::RotateY(20 * DEG)).z, 20 * DEG, 1e-3f);
  // Rolling 15 degrees left (counterclockwise seen from behind).
  EXPECT_NEAR(angles(Common::Quaternion::RotateZ(15 * DEG)).y, 15 * DEG, 1e-3f);

  // Gravity agrees with what the OpenXR input override reports: world up in aim space, mapped with
  // v -> (-x, z, y) (BuildOpenXRState).
  const Common::Quaternion pose = Common::Quaternion::RotateY(0.4f) *
                                  Common::Quaternion::RotateX(0.3f) *
                                  Common::Quaternion::RotateZ(0.2f);
  const Common::Vec3 up_in_aim = pose.Conjugate() * Common::Vec3(0, 1, 0);
  const Common::Vec3 expected(-up_in_aim.x, up_in_aim.z, up_in_aim.y);
  const Common::Vec3 actual =
      Wiimote::OpenXRToWiimoteRotation(pose).Conjugate() * Common::Vec3(0, 0, 1);
  EXPECT_NEAR(actual.x, expected.x, 1e-5f);
  EXPECT_NEAR(actual.y, expected.y, 1e-5f);
  EXPECT_NEAR(actual.z, expected.z, 1e-5f);
}

namespace
{
float AngleBetween(const Common::Quaternion& a, const Common::Quaternion& b)
{
  return 2 * std::acos(std::min(1.f, std::abs((a * b.Conjugate()).data.w)));
}

// What a MotionPlus game integrates: each sample over the time since the previous one.
Common::Quaternion Integrate(const Common::Quaternion& orientation, const Common::Vec3& rate,
                             double seconds)
{
  return (orientation * WiimoteEmu::GetRotationFromGyroscope(rate * float(seconds))).Normalized();
}

// The controller turning about a fixed body axis, sampled like OpenXR snapshots on PC.
struct TurningController
{
  Common::Quaternion start = Common::Quaternion::RotateX(0.3f) * Common::Quaternion::RotateZ(0.5f);
  Common::Vec3 rate{4, -7, 5};

  Common::Quaternion PoseAt(double t) const { return Integrate(start, rate, t); }
};

// A 540 deg/s turn for one second, then still. Reports come every 5.33 ms of emulated time, but
// the emulation runs in bursts of three reports, so the 30 Hz snapshots reach it irregularly.
// The game receives the rate of every report, or of every other one (MotionPlus passthrough
// modes, starting at report first_sample). Returns the worst error while turning.
float SimulateTurn(int sample_every, int first_sample, float* final_error)
{
  constexpr double REPORT_SECONDS = 1.0 / 187.5;
  constexpr double SNAPSHOT_SECONDS = 1.0 / 30;
  const TurningController controller;
  Wiimote::OpenXRGyroSteering steering;
  Common::Quaternion game = controller.start;
  Common::Quaternion snapshot = controller.start;
  Common::Vec3 snapshot_rate{};
  double snapshot_time = 0;
  double last_sample = 0;
  float worst_while_turning = 0;
  steering.Step(snapshot, snapshot_rate, 0);
  for (int i = 1; i * REPORT_SECONDS < 2.0; ++i)
  {
    const double t = i * REPORT_SECONDS;
    const double real_time = (i / 3) * 3 * REPORT_SECONDS;
    while (snapshot_time + SNAPSHOT_SECONDS <= real_time)
    {
      snapshot_time += SNAPSHOT_SECONDS;
      const Common::Quaternion pose = controller.PoseAt(std::min(snapshot_time, 1.0));
      snapshot_rate =
          Wiimote::GetOpenXRBodyAngularVelocity(snapshot, pose, float(SNAPSHOT_SECONDS));
      snapshot = pose;
    }
    const Common::Vec3 rate = steering.Step(snapshot, snapshot_rate, REPORT_SECONDS);
    if (i % sample_every == first_sample)
    {
      game = Integrate(game, rate, t - last_sample);
      last_sample = t;
    }
    if (t < 1.0)
      worst_while_turning = std::max(worst_while_turning, AngleBetween(game, snapshot));
  }
  *final_error = AngleBetween(game, controller.PoseAt(1.0));
  return worst_while_turning;
}
}  // namespace

TEST(WiimoteOpenXRGyro, BodyAngularVelocityUsesTheMotionPlusAxes)
{
  const Common::Quaternion base = Common::Quaternion::RotateZ(1.0f);
  const Common::Vec3 nose_down =
      Wiimote::GetOpenXRBodyAngularVelocity(base, base * Common::Quaternion::RotateX(0.1f), 0.1f);
  EXPECT_NEAR(nose_down.x, 1, 1e-4f);
  EXPECT_NEAR(nose_down.y, 0, 1e-4f);
  EXPECT_NEAR(nose_down.z, 0, 1e-4f);

  // The same rotation with the opposite quaternion sign takes the short way.
  const Common::Quaternion roll = base * Common::Quaternion::RotateY(-0.2f);
  const Common::Quaternion negated(-roll.data.w, -roll.data.x, -roll.data.y, -roll.data.z);
  const Common::Vec3 roll_rate = Wiimote::GetOpenXRBodyAngularVelocity(base, negated, 0.1f);
  EXPECT_NEAR(roll_rate.x, 0, 1e-4f);
  EXPECT_NEAR(roll_rate.y, -2, 1e-4f);
  EXPECT_NEAR(roll_rate.z, 0, 1e-4f);
}

TEST(WiimoteOpenXRGyro, SteeringKeepsTheGameOnTheControllerPose)
{
  for (const auto& [sample_every, first_sample] :
       {std::pair(1, 0), std::pair(2, 0), std::pair(2, 1)})
  {
    float final_error = 0;
    const float worst_while_turning = SimulateTurn(sample_every, first_sample, &final_error);
    // Behind by about one snapshot while turning, and nothing left once the controller stops.
    EXPECT_LT(worst_while_turning, 25 * DEG) << sample_every << ' ' << first_sample;
    EXPECT_LT(final_error, 0.5f * DEG) << sample_every << ' ' << first_sample;
  }
}

TEST(WiimoteOpenXRGyro, PoseHistoryFindsTheSampleOneWindowBack)
{
  constexpr s64 MS = 1'000'000;
  Wiimote::OpenXRPoseSample sample;
  Wiimote::OpenXRPoseHistory history;
  EXPECT_EQ(history.FindOlderThan(100 * MS, 16 * MS), nullptr);

  // 250 Hz polling: the sample one window back.
  for (s64 t = 0; t <= 100 * MS; t += 4 * MS)
  {
    sample.time_ns = t;
    history.Push(sample);
  }
  ASSERT_NE(history.FindOlderThan(100 * MS, 16 * MS), nullptr);
  EXPECT_EQ(history.FindOlderThan(100 * MS, 16 * MS)->time_ns, 84 * MS);
  EXPECT_EQ(history.FindOlderThan(102 * MS, 16 * MS)->time_ns, 84 * MS);
  // Per-frame polling: the previous sample, older than the window.
  EXPECT_EQ(history.FindOlderThan(133 * MS, 16 * MS)->time_ns, 100 * MS);

  // A young history gives its oldest sample once that is half a window old.
  Wiimote::OpenXRPoseHistory young;
  sample.time_ns = 0;
  young.Push(sample);
  EXPECT_EQ(young.FindOlderThan(5 * MS, 16 * MS), nullptr);
  ASSERT_NE(young.FindOlderThan(8 * MS, 16 * MS), nullptr);
  EXPECT_EQ(young.FindOlderThan(8 * MS, 16 * MS)->time_ns, 0);

  // The ring keeps the last CAPACITY samples.
  Wiimote::OpenXRPoseHistory ring;
  for (s64 t = 0; t != 200; ++t)
  {
    sample.time_ns = t * MS;
    ring.Push(sample);
  }
  ASSERT_NE(ring.FindOlderThan(199 * MS, 100 * MS), nullptr);
  EXPECT_EQ(ring.FindOlderThan(199 * MS, 100 * MS)->time_ns,
            (200 - s64(Wiimote::OpenXRPoseHistory::CAPACITY)) * MS);
  ring.Clear();
  EXPECT_EQ(ring.FindOlderThan(199 * MS, 1 * MS), nullptr);
}

TEST(WiimoteOpenXRGyro, SteeringCatchesUpWithAPoseJumpAtALimitedSpeed)
{
  Wiimote::OpenXRGyroSteering steering;
  const Common::Quaternion before = Common::Quaternion::Identity();
  const Common::Quaternion after = Common::Quaternion::RotateZ(90 * DEG);
  steering.Step(before, {}, 0);

  Common::Quaternion game = before;
  float fastest = 0;
  for (int i = 0; i != 400; ++i)
  {
    const Common::Vec3 rate = steering.Step(after, {}, 0.005);
    fastest = std::max(fastest, rate.Length());
    game = Integrate(game, rate, 0.005);
  }
  EXPECT_LE(fastest, Wiimote::OpenXRGyroSteering::MAX_STEERING_SPEED + 1e-4f);
  EXPECT_LT(AngleBetween(game, after), 0.5f * DEG);

  // A pause restarts the integral at the pose instead of spinning toward it.
  const Common::Vec3 after_pause = steering.Step(before, {}, 1.0);
  EXPECT_EQ(after_pause.Length(), 0);
  EXPECT_LT(AngleBetween(steering.integrated, before), 1e-4f);
}
#endif
