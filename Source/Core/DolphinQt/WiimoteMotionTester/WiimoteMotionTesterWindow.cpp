// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/WiimoteMotionTester/WiimoteMotionTesterWindow.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/MathUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/WiimoteMotionTester/WiimoteMotionPlot.h"
#include "DolphinQt/WiimoteMotionTester/WiimoteMotionView.h"

namespace
{
using WiimoteCommon::InputReportID;
using WiimoteMotionTracker::CalibrationSource;
using WiimoteMotionTracker::MotionPlusState;
using WiimoteMotionTracker::Snapshot;

constexpr float RAD_TO_DEG = float(360 / MathUtil::TAU);
// No report for this long (wall time) marks the data as stale.
constexpr auto STALE_TIME = std::chrono::milliseconds(500);
// Text readouts refresh every few frames to stay readable.
constexpr int READOUT_TICKS = 4;
constexpr int READOUTS_MIN_WIDTH = 420;
constexpr int READOUTS_WIDTH = 480;

QString Signed(float value, int precision)
{
  return (value < 0 ? QStringLiteral("-") : QStringLiteral("+")) +
         QString::number(std::abs(value), 'f', precision);
}

QString Degrees(float radians)
{
  return Signed(radians * RAD_TO_DEG, 1);
}

float WrapRadians(float angle)
{
  return std::remainder(angle, float(MathUtil::TAU));
}

QString ReportName(InputReportID id)
{
  switch (id)
  {
  case InputReportID::ReportCore:
    return QStringLiteral("Core");
  case InputReportID::ReportCoreAccel:
    return QStringLiteral("Core+Accel");
  case InputReportID::ReportCoreExt8:
    return QStringLiteral("Core+Ext8");
  case InputReportID::ReportCoreAccelIR12:
    return QStringLiteral("Core+Accel+IR12");
  case InputReportID::ReportCoreExt19:
    return QStringLiteral("Core+Ext19");
  case InputReportID::ReportCoreAccelExt16:
    return QStringLiteral("Core+Accel+Ext16");
  case InputReportID::ReportCoreIR10Ext9:
    return QStringLiteral("Core+IR10+Ext9");
  case InputReportID::ReportCoreAccelIR10Ext6:
    return QStringLiteral("Core+Accel+IR10+Ext6");
  case InputReportID::ReportExt21:
    return QStringLiteral("Ext21");
  case InputReportID::ReportInterleave1:
    return QStringLiteral("Interleaved 1");
  case InputReportID::ReportInterleave2:
    return QStringLiteral("Interleaved 2");
  default:
    return QStringLiteral("?");
  }
}

QString HexBytes(const u8* data, size_t size)
{
  QString result;
  for (size_t i = 0; i != size; ++i)
  {
    if (i != 0)
      result += QLatin1Char(' ');
    result += QStringLiteral("%1").arg(uint(data[i]), 2, 16, QLatin1Char('0')).toUpper();
  }
  return result;
}

QLabel* MakeValueLabel(bool wrap = false)
{
  auto* const label = new QLabel;
  label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setWordWrap(wrap);
  return label;
}

QFormLayout* MakeForm(QGroupBox* group)
{
  auto* const layout = new QFormLayout(group);
  layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  layout->setRowWrapPolicy(QFormLayout::DontWrapRows);
  return layout;
}
}  // namespace

WiimoteMotionTesterWindow::WiimoteMotionTesterWindow(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Wii Remote Motion Tester"));
  setWindowFlags(windowFlags() | Qt::Window | Qt::WindowMinMaxButtonsHint);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  CreateWidgets();
  ConnectWidgets();

  auto& settings = Settings::GetQSettings();
  if (!restoreGeometry(
          settings.value(QStringLiteral("wiimotemotiontester/geometry")).toByteArray()))
    resize(1280, 820);

  UpdateSource();
}

WiimoteMotionTesterWindow::~WiimoteMotionTesterWindow()
{
  SetActive(false);
  Settings::GetQSettings().setValue(QStringLiteral("wiimotemotiontester/geometry"), saveGeometry());
}

void WiimoteMotionTesterWindow::CreateWidgets()
{
  m_timer = new QTimer(this);

  m_slot_combo = new QComboBox;
  for (int i = 0; i != MAX_WIIMOTES; ++i)
    m_slot_combo->addItem(tr("Wii Remote %1").arg(i + 1));
  m_source_label = new QLabel;
  m_reset_button = new QPushButton(tr("Reset Orientation"));
  m_reset_button->setToolTip(
      tr("Sets the orientation back to the accelerometer's pitch and roll. The yaw comes from the "
         "IR dots when the sensor bar is visible; otherwise the remote is assumed to point at the "
         "sensor bar. The VR controller's yaw is aligned to it."));

  m_ir_correction = new QCheckBox(tr("IR Yaw Correction"));
  m_ir_correction->setToolTip(tr(
      "Corrects the gyroscope's yaw drift with the IR dots, like Dolphin's real Wii Remote input "
      "does. Off, the gyroscope alone drives the yaw, so disagreement with the IR pointer stays "
      "visible."));
  m_show_ghost = new QCheckBox(tr("Show VR Controller"));
  m_show_ghost->setChecked(true);
  m_show_ghost->setToolTip(
      tr("Draws the pose of the VR controller that drives an OpenXR Wii Remote, "
         "sampled when each report was sent."));
#ifndef ENABLE_VR
  m_show_ghost->setVisible(false);
#endif
  m_use_ir_distance = new QCheckBox(tr("Distance From IR"));
  m_use_ir_distance->setChecked(true);
  m_use_ir_distance->setToolTip(
      tr("Places the remote at the distance from the sensor bar that the IR dot spacing implies."));
  m_show_ir = new QCheckBox(tr("Show Screen and Pointers"));
  m_show_ir->setChecked(true);

  m_status_label = new QLabel;
  m_status_label->setWordWrap(true);

  auto* const top = new QHBoxLayout;
  top->addWidget(m_slot_combo);
  top->addWidget(m_source_label, 1);
  top->addWidget(m_reset_button);

  auto* const options = new QHBoxLayout;
  options->addWidget(m_ir_correction);
  options->addWidget(m_show_ghost);
  options->addWidget(m_use_ir_distance);
  options->addWidget(m_show_ir);
  options->addStretch(1);

  m_view = new WiimoteMotionView;
  m_accel_plot = new WiimoteMotionPlot(WiimoteMotionPlot::Kind::Accelerometer);
  m_gyro_plot = new WiimoteMotionPlot(WiimoteMotionPlot::Kind::Gyroscope);

  // IR camera, beside the plots.
  m_ir_label = MakeValueLabel(true);
  m_distance_label = MakeValueLabel(true);
  m_ir_view = new WiimoteIRView;
  auto* const ir_group = new QGroupBox(tr("IR Camera"));
  auto* const ir = new QVBoxLayout(ir_group);
  ir->addWidget(m_ir_view, 1);
  ir->addWidget(m_ir_label);
  ir->addWidget(m_distance_label);
  ir_group->setFixedWidth(280);

  auto* const plots = new QWidget;
  auto* const plots_layout = new QVBoxLayout;
  plots_layout->setContentsMargins(0, 0, 0, 0);
  plots_layout->addWidget(m_accel_plot);
  plots_layout->addWidget(m_gyro_plot);
  auto* const bottom_layout = new QHBoxLayout(plots);
  bottom_layout->setContentsMargins(0, 0, 0, 0);
  bottom_layout->addLayout(plots_layout, 1);
  bottom_layout->addWidget(ir_group);

  auto* const left = new QSplitter(Qt::Vertical);
  left->addWidget(m_view);
  left->addWidget(plots);
  left->setStretchFactor(0, 3);
  left->setStretchFactor(1, 2);

  // Reports.
  m_report_label = MakeValueLabel(true);
  m_rate_label = MakeValueLabel(true);
  m_raw_label = MakeValueLabel(true);
  auto* const reports_group = new QGroupBox(tr("Reports"));
  auto* const reports = MakeForm(reports_group);
  reports->addRow(tr("Mode:"), m_report_label);
  reports->addRow(tr("Rate:"), m_rate_label);
  reports->addRow(tr("Last:"), m_raw_label);

  // MotionPlus and calibration.
  m_motion_plus_label = MakeValueLabel(true);
  m_extension_label = MakeValueLabel(true);
  m_calibration_label = MakeValueLabel(true);
  auto* const motion_plus_group = new QGroupBox(tr("MotionPlus and Calibration"));
  auto* const motion_plus = MakeForm(motion_plus_group);
  motion_plus->addRow(tr("MotionPlus:"), m_motion_plus_label);
  motion_plus->addRow(tr("Extension:"), m_extension_label);
  motion_plus->addRow(tr("Calibration:"), m_calibration_label);

  // Motion table.
  auto* const motion_group = new QGroupBox(tr("Motion"));
  auto* const motion = new QGridLayout(motion_group);
  motion->setHorizontalSpacing(12);
  const QString headers[] = {tr("X (pitch)"), tr("Y (roll)"), tr("Z (yaw)")};
  for (int column = 0; column != 3; ++column)
  {
    auto* const header = new QLabel(headers[column]);
    header->setAlignment(Qt::AlignRight);
    motion->addWidget(header, 0, column + 1);
  }
  const QString row_names[ROW_COUNT] = {
      tr("Accelerometer (g)"), tr("Accelerometer raw"), tr("Gyroscope (°/s)"),
      tr("Gyroscope raw"),     tr("Gyroscope range"),   tr("Fused (°)"),
      tr("VR controller (°)"), tr("Difference (°)"),    tr("IR pointing (°)"),
  };
  for (int row = 0; row != ROW_COUNT; ++row)
  {
    motion->addWidget(new QLabel(row_names[row]), row + 1, 0);
    for (int column = 0; column != MOTION_COLUMNS; ++column)
    {
      const bool note = column == MOTION_COLUMNS - 1;
      QLabel* const cell = MakeValueLabel(note);
      cell->setAlignment(note ? Qt::AlignLeft : Qt::AlignRight);
      if (!note)
        cell->setMinimumWidth(
            QFontMetrics(cell->font()).horizontalAdvance(QStringLiteral("-0000.0")));
      m_motion_cells[row][column] = cell;
      motion->addWidget(cell, row + 1, column + 1);
    }
  }
  motion->setColumnStretch(MOTION_COLUMNS, 1);
#ifndef ENABLE_VR
  for (const MotionRow row : {ROW_GHOST, ROW_DIFFERENCE})
  {
    motion->itemAtPosition(row + 1, 0)->widget()->setVisible(false);
    for (QLabel* const cell : m_motion_cells[row])
      cell->setVisible(false);
  }
#endif
  auto* const convention =
      new QLabel(tr("Angles: positive nose down, rolled left, turned left (the MotionPlus axes). "
                    "Body axes: X left, Y back, Z up."));
  convention->setWordWrap(true);
  motion->addWidget(convention, ROW_COUNT + 1, 0, 1, MOTION_COLUMNS + 1);

  auto* const readouts = new QWidget;
  auto* const readouts_layout = new QVBoxLayout(readouts);
  readouts_layout->addWidget(reports_group);
  readouts_layout->addWidget(motion_plus_group);
  readouts_layout->addWidget(motion_group);
  readouts_layout->addStretch(1);

  auto* const scroll = new QScrollArea;
  scroll->setWidget(readouts);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setMinimumWidth(READOUTS_MIN_WIDTH);

  auto* const splitter = new QSplitter(Qt::Horizontal);
  splitter->addWidget(left);
  splitter->addWidget(scroll);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 0);
  splitter->setCollapsible(1, true);
  splitter->setSizes({800, READOUTS_WIDTH});

  auto* const layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addLayout(options);
  layout->addWidget(m_status_label);
  layout->addWidget(splitter, 1);
}

void WiimoteMotionTesterWindow::ConnectWidgets()
{
  connect(m_timer, &QTimer::timeout, this, &WiimoteMotionTesterWindow::Tick);
  connect(m_slot_combo, &QComboBox::currentIndexChanged, this, &WiimoteMotionTesterWindow::SetSlot);
  connect(m_reset_button, &QPushButton::clicked, this,
          [this] { WiimoteMotionTracker::RequestOrientationReset(u32(m_slot)); });
  connect(m_ir_correction, &QCheckBox::toggled, this,
          [this](bool checked) { WiimoteMotionTracker::SetIRCorrection(u32(m_slot), checked); });
  connect(m_show_ghost, &QCheckBox::toggled, m_view, &WiimoteMotionView::SetShowGhost);
  connect(m_use_ir_distance, &QCheckBox::toggled, m_view, &WiimoteMotionView::SetUseIRDistance);
  connect(m_show_ir, &QCheckBox::toggled, m_view, &WiimoteMotionView::SetShowIR);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State) {
    ClearHistory();
    UpdateSource();
  });
  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &WiimoteMotionTesterWindow::UpdateSource);
}

void WiimoteMotionTesterWindow::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  SetActive(true);
}

void WiimoteMotionTesterWindow::hideEvent(QHideEvent* event)
{
  SetActive(false);
  Settings::GetQSettings().setValue(QStringLiteral("wiimotemotiontester/geometry"), saveGeometry());
  QDialog::hideEvent(event);
}

void WiimoteMotionTesterWindow::SetActive(bool active)
{
  if (active == m_active)
    return;
  m_active = active;

  if (active)
  {
    ClearHistory();
    WiimoteMotionTracker::SetIRCorrection(u32(m_slot), m_ir_correction->isChecked());
    WiimoteMotionTracker::SetEnabled(u32(m_slot), true);
    const double refresh_rate = screen() ? screen()->refreshRate() : 60;
    m_timer->start(int(1000 / std::clamp(refresh_rate, 30.0, 240.0)));
    UpdateSource();
  }
  else
  {
    m_timer->stop();
    WiimoteMotionTracker::SetEnabled(u32(m_slot), false);
  }
}

void WiimoteMotionTesterWindow::SetSlot(int slot)
{
  if (slot == m_slot || slot < 0 || slot >= MAX_WIIMOTES)
    return;

  const bool active = m_active;
  SetActive(false);
  m_slot = slot;
  SetActive(active);
  UpdateSource();
}

void WiimoteMotionTesterWindow::ClearHistory()
{
  m_seq = 0;
  m_last_report_count = 0;
  m_last_report_time = {};
  m_accel_plot->Clear();
  m_gyro_plot->Clear();
}

void WiimoteMotionTesterWindow::UpdateSource()
{
  const WiimoteSource source = Config::Get(Config::GetInfoForWiimoteSource(m_slot));
  QString name;
  switch (source)
  {
  case WiimoteSource::Emulated:
    name = tr("Emulated Wii Remote");
    break;
  case WiimoteSource::OpenXR:
    name = tr("OpenXR Wii Remote (VR controller)");
    break;
  case WiimoteSource::Real:
    name = tr("Real Wii Remote");
    break;
  default:
    name = tr("None");
    break;
  }
  m_source = source;
  m_source_label->setText(tr("Source: %1").arg(name));
  m_show_ghost->setEnabled(source == WiimoteSource::OpenXR);
}

void WiimoteMotionTesterWindow::Tick()
{
  const u32 slot = u32(m_slot);
  const Snapshot snapshot = WiimoteMotionTracker::GetSnapshot(slot);

  m_new_samples.clear();
  const u64 seq = WiimoteMotionTracker::CopyNewSamples(slot, m_seq, &m_new_samples);
  if (seq < m_seq)
  {
    // The tracker was re-enabled.
    ClearHistory();
    WiimoteMotionTracker::CopyNewSamples(slot, 0, &m_new_samples);
  }
  m_seq = seq;
  m_accel_plot->AddSamples(m_new_samples);
  m_gyro_plot->AddSamples(m_new_samples);

  const auto now = std::chrono::steady_clock::now();
  if (snapshot.report_count != m_last_report_count)
  {
    m_last_report_count = snapshot.report_count;
    m_last_report_time = now;
  }
  const bool has_data = snapshot.report_count > 0 && now - m_last_report_time < STALE_TIME;

  m_view->SetSnapshot(snapshot, has_data);
  m_ir_view->SetSnapshot(snapshot, has_data);

  if (m_tick++ % READOUT_TICKS == 0)
    UpdateReadouts(snapshot, has_data);
}

void WiimoteMotionTesterWindow::SetMotionRow(MotionRow row, const QString& x, const QString& y,
                                             const QString& z, const QString& note)
{
  m_motion_cells[row][0]->setText(x);
  m_motion_cells[row][1]->setText(y);
  m_motion_cells[row][2]->setText(z);
  m_motion_cells[row][3]->setText(note);
}

void WiimoteMotionTesterWindow::UpdateReadouts(const Snapshot& s, bool has_data)
{
  // Status line.
  auto& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  QString status;
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
  {
    status = tr("Bluetooth passthrough is enabled: its reports bypass Dolphin's Wii Remote code, "
                "so the tester receives nothing.");
  }
  else if (state == Core::State::Uninitialized || state == Core::State::Starting)
  {
    status = tr("Start a Wii game to see this Wii Remote's reports.");
  }
  else if (!system.IsWii())
  {
    status = tr("GameCube games have no Wii Remotes.");
  }
  else if (!has_data)
  {
    status = state == Core::State::Paused ?
                 tr("Emulation is paused.") :
                 tr("No reports from this Wii Remote. Press a button to connect it.");
  }
  else
  {
    status = tr("Point the remote at the sensor bar or the screen, then press Reset Orientation.");
  }
  if (NetPlay::IsNetPlayRunning())
    status += QLatin1Char(' ') + tr("NetPlay: another player may drive this slot.");
  m_status_label->setText(status);

  const QString none = QStringLiteral("-");

  // MotionPlus and calibration.
  QString motion_plus;
  switch (s.motion_plus)
  {
  case MotionPlusState::Active:
    motion_plus = tr("Active");
    break;
  case MotionPlusState::AssumedActive:
    motion_plus = tr("Assumed active (state lost by a savestate load)");
    break;
  case MotionPlusState::Unknown:
    motion_plus = tr("Unknown (state lost by a savestate load)");
    break;
  default:
    motion_plus = tr("Inactive (the game has not activated it)");
    break;
  }
  if (s.motion_plus == MotionPlusState::Active)
  {
    switch (s.motion_plus_mode)
    {
    case 0x04:
      motion_plus += tr(", no passthrough");
      break;
    case 0x05:
      motion_plus += tr(", Nunchuk passthrough");
      break;
    case 0x07:
      motion_plus += tr(", Classic Controller passthrough");
      break;
    default:
      break;
    }
  }
  m_motion_plus_label->setText(motion_plus);

  QString extension = s.ext_id_valid ? tr("ID %1").arg(HexBytes(s.ext_id.data(), s.ext_id.size())) :
                                       tr("ID not read yet");
  extension += tr(", status bit %1").arg(s.extension_bit ? 1 : 0);
  m_extension_label->setText(extension);

  const auto calibration_name = [this](CalibrationSource source, bool used_when_bad) {
    switch (source)
    {
    case CalibrationSource::Device:
      return tr("read from the remote");
    case CalibrationSource::DeviceBadChecksum:
      return used_when_bad ? tr("read from the remote, bad checksum") :
                             tr("bad checksum on the remote, nominal values used");
    default:
      return tr("nominal (the game has not read it)");
    }
  };
  m_calibration_label->setText(tr("Accelerometer %1: 0 g %2 %3 %4, 1 g %5 %6 %7.\nMotionPlus %8.")
                                   .arg(calibration_name(s.accel_calibration, false))
                                   .arg(s.accel_zero_g[0])
                                   .arg(s.accel_zero_g[1])
                                   .arg(s.accel_zero_g[2])
                                   .arg(s.accel_one_g[0])
                                   .arg(s.accel_one_g[1])
                                   .arg(s.accel_one_g[2])
                                   .arg(calibration_name(s.motion_plus_calibration, true)));

  if (s.report_count == 0)
  {
    for (QLabel* label : {m_report_label, m_rate_label, m_raw_label, m_ir_label, m_distance_label})
      label->setText(none);
    for (int row = 0; row != ROW_COUNT; ++row)
      SetMotionRow(MotionRow(row), none, none, none);
    return;
  }

  // Reports.
  m_report_label->setText(tr("0x%1 %2%3")
                              .arg(uint(s.report_id), 2, 16, QLatin1Char('0'))
                              .arg(ReportName(s.report_id))
                              .arg(s.continuous ? tr(", continuous") : QString()));
  QString rate = tr("%1 Hz").arg(s.report_rate_hz, 0, 'f', 1);
  // Real Wii Remotes can repeat their last report to keep 200 Hz. Emulated ones send identical
  // reports whenever the input does not change, so the new-data rate means nothing for them.
  if (m_source == WiimoteSource::Real && s.unique_report_rate_hz > 0 &&
      std::abs(s.unique_report_rate_hz - s.report_rate_hz) > s.report_rate_hz * 0.05f)
  {
    rate += tr(", %1 Hz of new data").arg(s.unique_report_rate_hz, 0, 'f', 1);
  }
  m_rate_label->setText(rate);
  m_raw_label->setText(HexBytes(s.last_report.data(), s.last_report_size));

  // Motion table.
  if (s.has_accel)
  {
    SetMotionRow(ROW_ACCEL, Signed(s.accel_g.x, 3), Signed(s.accel_g.y, 3), Signed(s.accel_g.z, 3),
                 tr("|a| %1").arg(s.accel_g.Length(), 0, 'f', 3));
    SetMotionRow(ROW_ACCEL_RAW, QString::number(s.accel_raw[0]), QString::number(s.accel_raw[1]),
                 QString::number(s.accel_raw[2]), tr("10-bit"));
  }
  else
  {
    SetMotionRow(ROW_ACCEL, none, none, none, tr("not in this mode"));
    SetMotionRow(ROW_ACCEL_RAW, none, none, none);
  }

  if (s.has_gyro)
  {
    const Common::Vec3 dps = s.gyro_rad_s * RAD_TO_DEG;
    SetMotionRow(ROW_GYRO, Signed(dps.x, 1), Signed(dps.y, 1), Signed(dps.z, 1),
                 s.gyro_age > 0.05f ? tr("%1 ms old").arg(int(s.gyro_age * 1000)) : QString());
    SetMotionRow(ROW_GYRO_RAW, QString::number(s.gyro_raw[0]), QString::number(s.gyro_raw[1]),
                 QString::number(s.gyro_raw[2]), tr("14-bit"));
    const auto range = [this](bool slow) { return slow ? tr("slow") : tr("FAST"); };
    SetMotionRow(ROW_GYRO_RANGE, range(s.gyro_slow[0]), range(s.gyro_slow[1]),
                 range(s.gyro_slow[2]));
  }
  else
  {
    SetMotionRow(ROW_GYRO, none, none, none, tr("no MotionPlus data"));
    SetMotionRow(ROW_GYRO_RAW, none, none, none);
    SetMotionRow(ROW_GYRO_RANGE, none, none, none);
  }

  SetMotionRow(ROW_FUSED, Degrees(s.angles.x), Degrees(s.angles.y), Degrees(s.angles.z));

  if (s.ghost_valid)
  {
    SetMotionRow(ROW_GHOST, Degrees(s.ghost_angles.x), Degrees(s.ghost_angles.y),
                 Degrees(s.ghost_angles.z), s.ghost_left_hand ? tr("left hand") : tr("right hand"));
    const Common::Quaternion difference = s.orientation * s.ghost_orientation.Conjugate();
    const float total = 2 * std::acos(std::min(1.f, std::abs(difference.data.w)));
    SetMotionRow(ROW_DIFFERENCE, Degrees(WrapRadians(s.angles.x - s.ghost_angles.x)),
                 Degrees(WrapRadians(s.angles.y - s.ghost_angles.y)),
                 Degrees(WrapRadians(s.angles.z - s.ghost_angles.z)),
                 tr("total %1").arg(total * RAD_TO_DEG, 0, 'f', 1));
  }
  else
  {
    SetMotionRow(ROW_GHOST, none, none, none, tr("OpenXR source only"));
    SetMotionRow(ROW_DIFFERENCE, none, none, none);
  }

  if (s.ir_visible > 0)
  {
    SetMotionRow(ROW_IR, Degrees(s.ir_pitch), s.ir_roll_valid ? Degrees(s.ir_roll) : none,
                 Degrees(s.ir_yaw));
  }
  else
  {
    SetMotionRow(ROW_IR, none, none, none,
                 s.ir_format == WiimoteCommon::IRReportFormat::None ? tr("not in this mode") :
                                                                      tr("no dots"));
  }

  // IR camera.
  if (s.ir_format == WiimoteCommon::IRReportFormat::None)
    m_ir_label->setText(tr("Dots: not in this reporting mode"));
  else if (s.ir_visible == 0)
    m_ir_label->setText(tr("Dots: none visible"));
  else
    m_ir_label->setText(tr("Dots: %1 visible, center %2 %3")
                            .arg(s.ir_visible)
                            .arg(Signed(s.ir_center.x, 2))
                            .arg(Signed(s.ir_center.y, 2)));

  QString distance = s.ir_distance_valid ? tr("Distance: %1 m").arg(s.ir_distance_m, 0, 'f', 2) :
                                           tr("Distance: unknown (needs two dots)");
  if (s.ghost_distance_valid)
    distance += tr(", VR screen %1 m").arg(s.ghost_distance_m, 0, 'f', 2);
  m_distance_label->setText(distance);
}
