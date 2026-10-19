// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QCoreApplication>
#include <QString>

class QDoubleSpinBox;
class QFormLayout;

// The "Min Camera Depth" / "Max Camera Depth" rows of the Shader, Elements and Texture override
// dialogs (the min_depth / max_depth keys; 0 = no limit).
class CameraDepthLimitWidgets final
{
  Q_DECLARE_TR_FUNCTIONS(CameraDepthLimitWidgets)

public:
  // Creates the spin boxes; AddRows hands them to the dialog's layout.
  CameraDepthLimitWidgets();

  void AddRows(QFormLayout* form);

  void SetValues(float min_depth, float max_depth);
  float GetMinDepth() const;
  float GetMaxDepth() const;

  // Why the values cannot be saved (both set and min > max), empty when they can.
  QString Validate() const;

private:
  static QDoubleSpinBox* CreateDepthSpin(const QString& tooltip);

  QDoubleSpinBox* m_min_depth_spin = nullptr;
  QDoubleSpinBox* m_max_depth_spin = nullptr;
};
