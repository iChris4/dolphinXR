// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointF>
#include <QWidget>

#include "Core/HW/WiimoteMotionTracker.h"

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

// A 3D scene with a Wii Remote shaped block oriented from the decoded reports, the VR controller
// as a wireframe ghost, and a sensor bar under which a screen shows where each one points.
class WiimoteMotionView final : public QWidget
{
  Q_OBJECT
public:
  explicit WiimoteMotionView(QWidget* parent = nullptr);

  void SetSnapshot(const WiimoteMotionTracker::Snapshot& snapshot, bool has_data);
  void SetShowGhost(bool show);
  void SetShowIR(bool show);
  void SetUseIRDistance(bool use);
  void ResetView();

  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  WiimoteMotionTracker::Snapshot m_snapshot;
  bool m_has_data = false;
  bool m_show_ghost = true;
  bool m_show_ir = true;
  bool m_use_ir_distance = true;

  float m_azimuth;
  float m_elevation;
  float m_radius;
  QPointF m_drag_start;
  float m_drag_azimuth = 0;
  float m_drag_elevation = 0;
};

// The IR camera image: dots in the 1024x768 camera space, drawn as seen from behind the remote.
class WiimoteIRView final : public QWidget
{
  Q_OBJECT
public:
  explicit WiimoteIRView(QWidget* parent = nullptr);

  void SetSnapshot(const WiimoteMotionTracker::Snapshot& snapshot, bool has_data);

  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return width * 3 / 4; }
  QSize sizeHint() const override { return {240, 180}; }
  QSize minimumSizeHint() const override { return {160, 120}; }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  WiimoteMotionTracker::Snapshot m_snapshot;
  bool m_has_data = false;
};
