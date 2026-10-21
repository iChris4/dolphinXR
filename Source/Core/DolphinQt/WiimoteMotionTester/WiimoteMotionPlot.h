// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <vector>

#include <QString>
#include <QWidget>

#include "Core/HW/WiimoteMotionTracker.h"

class QPaintEvent;

// A scrolling plot of the last seconds of accelerometer or gyroscope samples, X/Y/Z in red, green
// and blue. Time is emulated time, so the plot stops when emulation pauses.
class WiimoteMotionPlot final : public QWidget
{
  Q_OBJECT
public:
  enum class Kind
  {
    Accelerometer,
    Gyroscope,
  };

  explicit WiimoteMotionPlot(Kind kind, QWidget* parent = nullptr);

  void AddSamples(const std::vector<WiimoteMotionTracker::Sample>& samples);
  void Clear();

  QSize sizeHint() const override { return {640, 140}; }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  float GetValue(const WiimoteMotionTracker::Sample& sample, int axis) const;
  bool HasValue(const WiimoteMotionTracker::Sample& sample) const;

  Kind m_kind;
  std::deque<WiimoteMotionTracker::Sample> m_samples;
};
