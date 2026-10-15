// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include <imgui.h>

#include "Core/System.h"

#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VR/VRMenuTabs.h"

namespace VR
{
namespace
{
// The performance statistics of the on-screen display (frame time graph, VPS, FPS, speed),
// which is not visible inside the headset.
class StatsTab final : public VRMenuTab
{
public:
  const char* GetName() const override { return "Stats"; }

  void Draw(VRMenuContext&) override
  {
    Core::System::GetInstance().GetPerfMetrics().DrawImGuiStatsPanel(
        ImGui::GetFontSize() * 14.0f, 3.0f);
  }
};
}  // namespace

std::unique_ptr<VRMenuTab> CreateStatsTab()
{
  return std::make_unique<StatsTab>();
}
}  // namespace VR
