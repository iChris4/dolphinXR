// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/CameraDepthLimitWidgets.h"

#include <algorithm>

#include <QDoubleSpinBox>
#include <QFormLayout>

QDoubleSpinBox* CameraDepthLimitWidgets::CreateDepthSpin(const QString& tooltip)
{
  auto* spin = new QDoubleSpinBox;
  spin->setRange(0.0, 1000000.0);
  spin->setDecimals(3);
  spin->setSingleStep(1.0);
  spin->setSpecialValueText(tr("Any"));
  spin->setValue(0.0);
  spin->setToolTip(tooltip);
  return spin;
}

CameraDepthLimitWidgets::CameraDepthLimitWidgets()
{
  const QString common =
      tr("Distance from the game camera of the draw's vertices, in game units (Any = no "
         "limit).\nUse it for a HUD drawn in perspective a few units in front of the camera that "
         "shares its textures, shaders or render state with world objects. The agent server's "
         "get_draw and list_draws show a draw's camera depth.");
  m_min_depth_spin = CreateDepthSpin(
      tr("Only match draws whose vertices are all at least this far from the camera.") +
      QStringLiteral("\n") + common);
  m_max_depth_spin = CreateDepthSpin(
      tr("Only match draws whose vertices are all at most this far from the camera.") +
      QStringLiteral("\n") + common);
}

void CameraDepthLimitWidgets::AddRows(QFormLayout* form)
{
  form->addRow(tr("Min Camera Depth:"), m_min_depth_spin);
  form->addRow(tr("Max Camera Depth:"), m_max_depth_spin);
}

void CameraDepthLimitWidgets::SetValues(float min_depth, float max_depth)
{
  m_min_depth_spin->setValue(std::max(min_depth, 0.0f));
  m_max_depth_spin->setValue(std::max(max_depth, 0.0f));
}

float CameraDepthLimitWidgets::GetMinDepth() const
{
  return static_cast<float>(m_min_depth_spin->value());
}

float CameraDepthLimitWidgets::GetMaxDepth() const
{
  return static_cast<float>(m_max_depth_spin->value());
}

QString CameraDepthLimitWidgets::Validate() const
{
  if (GetMinDepth() > 0.0f && GetMaxDepth() > 0.0f && GetMinDepth() > GetMaxDepth())
    return tr("Min Camera Depth must not be greater than Max Camera Depth.");
  return {};
}
