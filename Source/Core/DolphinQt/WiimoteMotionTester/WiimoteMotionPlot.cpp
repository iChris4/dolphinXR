// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/WiimoteMotionTester/WiimoteMotionPlot.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QPainter>
#include <QPolygonF>

#include "Common/MathUtil.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"

namespace
{
using WiimoteMotionTracker::Sample;

constexpr float WINDOW_SECONDS = 5.0f;
// Samples further apart than this are not joined.
constexpr float MAX_JOIN_SECONDS = 0.1f;

const QColor AXIS_COLORS[3] = {QColor(0xe0, 0x45, 0x45), QColor(0x3c, 0xb3, 0x50),
                               QColor(0x3c, 0x7d, 0xe0)};

constexpr std::array<u8, 3> SLOW_FLAGS = {Sample::SLOW_PITCH, Sample::SLOW_ROLL, Sample::SLOW_YAW};
}  // namespace

WiimoteMotionPlot::WiimoteMotionPlot(Kind kind, QWidget* parent) : QWidget(parent), m_kind(kind)
{
  setMinimumHeight(110);
  if (kind == Kind::Gyroscope)
  {
    setToolTip(tr("MotionPlus samples as decoded from the reports. Thick segments use the fast "
                  "(low precision) range; the dashed lines mark the slow range limit."));
  }
  else
  {
    setToolTip(tr("Accelerometer as decoded from the reports, in g. At rest it reads 1 g up."));
  }
}

void WiimoteMotionPlot::AddSamples(const std::vector<Sample>& samples)
{
  for (const Sample& sample : samples)
  {
    // Decoding restarted.
    if (!m_samples.empty() && sample.time + 1e-4f < m_samples.back().time)
      m_samples.clear();
    m_samples.push_back(sample);
  }
  if (m_samples.empty())
    return;

  const float oldest = m_samples.back().time - WINDOW_SECONDS - 0.5f;
  while (!m_samples.empty() && m_samples.front().time < oldest)
    m_samples.pop_front();
  update();
}

void WiimoteMotionPlot::Clear()
{
  m_samples.clear();
  update();
}

bool WiimoteMotionPlot::HasValue(const Sample& sample) const
{
  return m_kind == Kind::Accelerometer ? (sample.flags & Sample::HAS_ACCEL) != 0 :
                                         (sample.flags & Sample::HAS_GYRO) != 0;
}

float WiimoteMotionPlot::GetValue(const Sample& sample, int axis) const
{
  return m_kind == Kind::Accelerometer ? sample.accel_g.data[axis] : sample.gyro_dps.data[axis];
}

void WiimoteMotionPlot::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const QColor text = palette().text().color();
  QColor grid = text;
  grid.setAlpha(40);
  QColor faint = text;
  faint.setAlpha(110);

  painter.fillRect(rect(), palette().base());
  const QFontMetrics metrics(font());
  const QRectF plot = QRectF(rect()).adjusted(52, metrics.height() + 6, -10, -metrics.height() - 4);

  const bool gyro = m_kind == Kind::Gyroscope;
  const float latest = m_samples.empty() ? 0 : m_samples.back().time;
  const float earliest = latest - WINDOW_SECONDS;

  // Vertical range: grows to fit the visible peaks.
  float peak = 0;
  bool has_values = false;
  for (const Sample& sample : m_samples)
  {
    if (sample.time < earliest || !HasValue(sample))
      continue;
    has_values = true;
    for (int axis = 0; axis != 3; ++axis)
      peak = std::max(peak, std::abs(GetValue(sample, axis)));
  }
  const float step = gyro ? 200.0f : 1.0f;
  const float minimum_range = gyro ? 600.0f : 3.0f;
  const float maximum_range = gyro ? 3000.0f : 6.0f;
  const float range =
      std::clamp(std::ceil(peak * 1.1f / step) * step, minimum_range, maximum_range);

  const auto to_x = [&](float time) {
    return plot.left() + (time - earliest) / WINDOW_SECONDS * plot.width();
  };
  const auto to_y = [&](float value) {
    return plot.center().y() - std::clamp(value / range, -1.f, 1.f) * plot.height() / 2;
  };

  // Grid.
  painter.setPen(QPen(grid, 1));
  const float value_step = gyro ? (range > 1200 ? 600.0f : 200.0f) : 1.0f;
  for (float value = -range; value <= range + 0.001f; value += value_step)
  {
    const double y = to_y(value);
    painter.setPen(QPen(value == 0 ? faint : grid, 1));
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    painter.setPen(faint);
    painter.drawText(QRectF(0, y - metrics.height() / 2.0, plot.left() - 6, metrics.height()),
                     Qt::AlignRight | Qt::AlignVCenter, QString::number(value, 'f', 0));
  }
  painter.setPen(QPen(grid, 1));
  for (float second = std::ceil(earliest); second <= latest; second += 1)
  {
    const double x = to_x(second);
    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
  }
  painter.setPen(faint);
  painter.drawText(QRectF(plot.left(), plot.bottom() + 2, plot.width(), metrics.height()),
                   Qt::AlignRight | Qt::AlignTop, tr("%1 s").arg(latest, 0, 'f', 1));

  if (gyro)
  {
    // Beyond this the MotionPlus reports in its fast, coarser range.
    const float slow_limit =
        WiimoteEmu::MotionPlus::SLOW_MAX_RAD_PER_SEC * float(360 / MathUtil::TAU);
    QPen pen(faint, 1);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    for (const float value : {slow_limit, -slow_limit})
    {
      if (slow_limit < range)
        painter.drawLine(QPointF(plot.left(), to_y(value)), QPointF(plot.right(), to_y(value)));
    }
  }

  // Series.
  painter.save();
  painter.setClipRect(plot);
  for (int axis = 0; axis != 3; ++axis)
  {
    QPolygonF line;
    const Sample* previous = nullptr;
    const auto flush = [&] {
      if (line.size() >= 2)
      {
        painter.setPen(QPen(AXIS_COLORS[axis], 1.3));
        painter.drawPolyline(line);
      }
      line.clear();
    };

    for (const Sample& sample : m_samples)
    {
      if (!HasValue(sample))
        continue;
      if (previous && sample.time - previous->time > MAX_JOIN_SECONDS)
        flush();
      const QPointF point(to_x(sample.time), to_y(GetValue(sample, axis)));
      if (gyro && previous && !(sample.flags & SLOW_FLAGS[axis]) &&
          sample.time - previous->time <= MAX_JOIN_SECONDS)
      {
        painter.setPen(QPen(AXIS_COLORS[axis], 3.5));
        painter.drawLine(QPointF(to_x(previous->time), to_y(GetValue(*previous, axis))), point);
      }
      line << point;
      previous = &sample;
    }
    flush();
  }
  painter.restore();

  painter.setPen(QPen(grid, 1));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(plot);

  // Title and legend.
  painter.setPen(text);
  painter.drawText(QPointF(plot.left(), metrics.ascent() + 2),
                   gyro ? tr("MotionPlus gyroscope (°/s)") : tr("Accelerometer (g)"));
  const std::array<QString, 3> names =
      gyro ? std::array<QString, 3>{tr("Pitch (X)"), tr("Roll (Y)"), tr("Yaw (Z)")} :
             std::array<QString, 3>{tr("X (left)"), tr("Y (back)"), tr("Z (up)")};
  double x = plot.right();
  for (int axis = 2; axis >= 0; --axis)
  {
    x -= metrics.horizontalAdvance(names[axis]);
    painter.setPen(text);
    painter.drawText(QPointF(x, metrics.ascent() + 2), names[axis]);
    x -= 16;
    painter.fillRect(QRectF(x, metrics.ascent() / 2.0, 10, 4), AXIS_COLORS[axis]);
    x -= 12;
  }

  if (!has_values)
  {
    painter.setPen(faint);
    painter.drawText(plot, Qt::AlignCenter, gyro ? tr("No MotionPlus samples") : tr("No samples"));
  }
}
