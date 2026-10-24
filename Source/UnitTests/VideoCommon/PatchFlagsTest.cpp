// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/VR/PatchFlags.h"

TEST(PatchFlags, GenerationChangesOnlyWithTheFlags)
{
  VR::PatchFlags::Set({});
  const u64 empty = VR::PatchFlags::GetGeneration();

  VR::PatchFlags::Set({"mp_visor_scan", "mp_paused", "mp_paused"});
  const u64 raised = VR::PatchFlags::GetGeneration();
  EXPECT_NE(raised, empty);
  EXPECT_EQ(VR::PatchFlags::Snapshot(), (std::vector<std::string>{"mp_paused", "mp_visor_scan"}));

  // The same flags in another order: nothing changed.
  VR::PatchFlags::Set({"mp_visor_scan", "mp_paused"});
  EXPECT_EQ(VR::PatchFlags::GetGeneration(), raised);

  VR::PatchFlags::Set({});
  EXPECT_NE(VR::PatchFlags::GetGeneration(), raised);
  EXPECT_TRUE(VR::PatchFlags::Snapshot().empty());
}

// An Elements entry conditioned on a flag a patch raises: the flag takes effect at the next end
// of frame, and survives a re-apply of the override lists (which clears the draw-raised flags).
TEST(PatchFlags, ConditionOverrideEntries)
{
  ElementsGroupManager::ElementGroupOverride entry;
  entry.name = "Scan box";
  entry.handling = ShaderHunter::HandlingType::Fullscreen;
  entry.runtime_element.valid = true;
  entry.runtime_element.use_projection_type = true;
  entry.condition_flag = "mp_visor_scan";

  auto& elements = ElementsGroupManager::GetInstance();
  auto& hunter = ShaderHunter::GetInstance();
  elements.ApplyOverrideList("FLAGS1", {entry});

  ElementsGroupManager::DrawRecord draw;
  draw.signature.valid = true;

  VR::PatchFlags::Set({});
  hunter.OnFrameEnd();
  EXPECT_EQ(elements.ResolveOverrides(draw).handling, ShaderHunter::HandlingType::Skip);

  VR::PatchFlags::Set({"mp_visor_scan"});
  EXPECT_EQ(elements.ResolveOverrides(draw).handling, ShaderHunter::HandlingType::Skip);
  hunter.OnFrameEnd();
  EXPECT_TRUE(hunter.IsFlagActive("mp_visor_scan"));
  EXPECT_EQ(elements.ResolveOverrides(draw).handling, ShaderHunter::HandlingType::Fullscreen);

  // Draw flags are only active the frame after they were seen; patch flags stay.
  hunter.OnFrameEnd();
  hunter.ApplyOverrideList("FLAGS1", {});
  EXPECT_EQ(elements.ResolveOverrides(draw).handling, ShaderHunter::HandlingType::Fullscreen);

  VR::PatchFlags::Set({});
  hunter.OnFrameEnd();
  EXPECT_FALSE(hunter.IsFlagActive("mp_visor_scan"));
  EXPECT_EQ(elements.ResolveOverrides(draw).handling, ShaderHunter::HandlingType::Skip);

  elements.ApplyOverrideList("", {});
  hunter.ApplyOverrideList("", {});
}
