// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <vector>

#include <QDialog>
#include <QString>

#include "Common/CommonTypes.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteMotionTracker.h"

class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;
class WiimoteIRView;
class WiimoteMotionPlot;
class WiimoteMotionView;

// Shows a Wii Remote slot's motion as the game receives it: the accelerometer, MotionPlus and IR
// data decoded from its reports, fused into an orientation drawn as a 3D Wii Remote. For the
// OpenXR source, the VR controller driving it is drawn over it for comparison.
class WiimoteMotionTesterWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit WiimoteMotionTesterWindow(QWidget* parent);
  ~WiimoteMotionTesterWindow() override;

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void SetActive(bool active);
  void SetSlot(int slot);
  void Tick();
  void UpdateSource();
  void UpdateReadouts(const WiimoteMotionTracker::Snapshot& snapshot, bool has_data);
  void ClearHistory();

  int m_slot = 0;
  WiimoteSource m_source = WiimoteSource::None;
  bool m_active = false;
  u64 m_seq = 0;
  u64 m_last_report_count = 0;
  std::chrono::steady_clock::time_point m_last_report_time{};
  int m_tick = 0;
  std::vector<WiimoteMotionTracker::Sample> m_new_samples;

  QTimer* m_timer;
  QComboBox* m_slot_combo;
  QLabel* m_source_label;
  QLabel* m_status_label;
  QPushButton* m_reset_button;
  QCheckBox* m_ir_correction;
  QCheckBox* m_show_ghost;
  QCheckBox* m_use_ir_distance;
  QCheckBox* m_show_ir;

  WiimoteMotionView* m_view;
  WiimoteIRView* m_ir_view;
  WiimoteMotionPlot* m_accel_plot;
  WiimoteMotionPlot* m_gyro_plot;

  QLabel* m_report_label;
  QLabel* m_rate_label;
  QLabel* m_raw_label;
  QLabel* m_motion_plus_label;
  QLabel* m_extension_label;
  QLabel* m_calibration_label;
  QLabel* m_ir_label;
  QLabel* m_distance_label;

  // The motion table: rows of X (pitch), Y (roll), Z (yaw) and a note column.
  enum MotionRow
  {
    ROW_ACCEL,
    ROW_ACCEL_RAW,
    ROW_GYRO,
    ROW_GYRO_RAW,
    ROW_GYRO_RANGE,
    ROW_FUSED,
    ROW_GHOST,
    ROW_DIFFERENCE,
    ROW_IR,
    ROW_COUNT,
  };
  static constexpr int MOTION_COLUMNS = 4;
  std::array<std::array<QLabel*, MOTION_COLUMNS>, ROW_COUNT> m_motion_cells{};
  void SetMotionRow(MotionRow row, const QString& x, const QString& y, const QString& z,
                    const QString& note = {});
};
