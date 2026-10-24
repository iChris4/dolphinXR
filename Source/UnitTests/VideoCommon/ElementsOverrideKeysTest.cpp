// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/ShaderHunter.h"

namespace
{
using HandlingType = ShaderHunter::HandlingType;

std::string WriteTemp(const std::string& contents)
{
  const std::string path = File::CreateTempDir() + "/profile.ini";
  EXPECT_TRUE(File::WriteStringToFile(path, contents));
  return path;
}

// The HUD body of a Metroid Prime GC frame, as the profile entry gives it.
constexpr const char* HUD_BODY = "$HUD body\nformat=element_only_v6\nhandling=headlocked\n"
                                 "headlocked_mode=perspective\nhud_scale=30\nhud_width=0.79\n"
                                 "hud_height=0.79\nhud_anchor=1\nhud_anchor_z_min=-28.5\n"
                                 "hud_anchor_z_max=-18\nhud_context=combat\n"
                                 "sig_perspective=1\nsig_use_projection_type=1\n"
                                 "sig_use_load=1\nsig_load_min=2\nsig_load_max=-1\n";
}  // namespace

TEST(ElementsOverrideKeys, ParsesLoadAndHudKeys)
{
  const std::string path =
      WriteTemp(std::string("[ElementsGroupOverride]\n") + HUD_BODY +
                "\n$Radar\nformat=element_only_v6\nhandling=headlocked\nhud_self_center=1\n"
                "hud_context=2\nsig_perspective=1\nsig_use_projection_type=1\n"
                "\n$Plain\nformat=element_only_v6\nhandling=headlocked\nsig_perspective=1\n"
                "sig_use_projection_type=1\n");
  const auto entries = ElementsGroupManager::LoadOverridesFromINIFile(path);
  ASSERT_EQ(entries.size(), 3u);

  const auto& body = entries[0];
  EXPECT_TRUE(body.runtime_element.use_load);
  EXPECT_EQ(body.runtime_element.load_min, 2);
  EXPECT_EQ(body.runtime_element.load_max, -1);
  ASSERT_TRUE(body.headlocked);
  EXPECT_TRUE(body.headlocked->perspective);
  EXPECT_FLOAT_EQ(body.headlocked->scale, 30.0f);
  EXPECT_FLOAT_EQ(body.headlocked->width, 0.79f);
  EXPECT_FLOAT_EQ(body.headlocked->height, 0.79f);
  EXPECT_TRUE(body.headlocked->anchor);
  EXPECT_FLOAT_EQ(body.headlocked->anchor_z_min, -28.5f);
  EXPECT_FLOAT_EQ(body.headlocked->anchor_z_max, -18.0f);
  EXPECT_FALSE(body.headlocked->self_center);
  EXPECT_EQ(body.headlocked->context, 1);

  ASSERT_TRUE(entries[1].headlocked);
  EXPECT_FALSE(entries[1].headlocked->perspective);
  EXPECT_TRUE(entries[1].headlocked->self_center);
  EXPECT_EQ(entries[1].headlocked->context, 2);
  EXPECT_FALSE(std::isfinite(entries[1].headlocked->anchor_z_min));
  EXPECT_FALSE(entries[1].runtime_element.use_load);

  // No HUD key: plain head-locked.
  EXPECT_FALSE(entries[2].headlocked);
}

TEST(ElementsOverrideKeys, SavesLoadAndHudKeys)
{
  const std::string previous = File::GetUserPath(D_GAMESETTINGSVR_IDX);
  File::SetUserPath(D_GAMESETTINGSVR_IDX, File::CreateTempDir() + "/");

  const auto parsed = ElementsGroupManager::LoadOverridesFromINIFile(
      WriteTemp(std::string("[ElementsGroupOverride]\n") + HUD_BODY));
  ASSERT_EQ(parsed.size(), 1u);
  ElementsGroupManager::ElementGroupOverride body = parsed[0];
  body.runtime_element.any_alpha_update = true;
  ElementsGroupManager::ElementGroupOverride plain = parsed[0];
  plain.name = "Plain";
  plain.headlocked.reset();
  plain.runtime_element.use_load = false;
  ElementsGroupManager::SaveOverridesToINI("KEYS1", {body, plain});
  std::string text;
  File::ReadFileToString(File::GetUserPath(D_GAMESETTINGSVR_IDX) + "KEYS1.ini", text);
  const auto saved = ElementsGroupManager::LoadOverridesFromINI("KEYS1");

  File::SetUserPath(D_GAMESETTINGSVR_IDX, previous);

  ASSERT_EQ(saved.size(), 2u);
  EXPECT_TRUE(saved[0].runtime_element.use_load);
  EXPECT_EQ(saved[0].runtime_element.load_min, 2);
  EXPECT_EQ(saved[0].runtime_element.load_max, -1);
  ASSERT_TRUE(saved[0].headlocked);
  EXPECT_TRUE(saved[0].headlocked->perspective);
  EXPECT_FLOAT_EQ(saved[0].headlocked->scale, 30.0f);
  EXPECT_FLOAT_EQ(saved[0].headlocked->anchor_z_min, -28.5f);
  EXPECT_EQ(saved[0].headlocked->context, 1);
  EXPECT_TRUE(saved[0].runtime_element.any_alpha_update);
  // An entry without the keys is saved without them, as before they existed.
  EXPECT_FALSE(saved[1].headlocked);
  EXPECT_FALSE(saved[1].runtime_element.use_load);
  EXPECT_FALSE(saved[1].runtime_element.any_alpha_update);
  EXPECT_EQ(text.find("sig_use_load"), text.rfind("sig_use_load"));
  EXPECT_EQ(text.find("headlocked_mode"), text.rfind("headlocked_mode"));
  EXPECT_EQ(text.find("sig_any_alpha_write"), text.rfind("sig_any_alpha_write"));
}

// The entry that gives the draw its handling supplies the HUD parameters, and the projection load
// tells apart two draws that share everything else.
TEST(ElementsOverrideKeys, ResolvesHudParametersOfTheMatchingEntry)
{
  const auto entries = ElementsGroupManager::LoadOverridesFromINIFile(
      WriteTemp(std::string("[ElementsGroupOverride]\n") + HUD_BODY +
                "\n$Helmet\nformat=element_only_v6\nhandling=headlocked\n"
                "headlocked_mode=perspective\nhud_scale=100\nhud_width=1.7\nhud_height=1.7\n"
                "sig_perspective=1\nsig_use_projection_type=1\n"
                "sig_use_load=1\nsig_load_min=0\nsig_load_max=1\n"));
  ASSERT_EQ(entries.size(), 2u);
  auto& manager = ElementsGroupManager::GetInstance();
  manager.ApplyOverrideList("KEYS2", entries);

  ElementsGroupManager::DrawRecord draw;
  draw.signature.valid = true;
  draw.signature.perspective = true;

  draw.signature.projection_load = 1;
  auto result = manager.ResolveOverrides(draw);
  EXPECT_EQ(result.handling, HandlingType::HeadLocked);
  ASSERT_TRUE(result.headlocked);
  EXPECT_FLOAT_EQ(result.headlocked->scale, 100.0f);
  EXPECT_FALSE(result.headlocked->anchor);

  draw.signature.projection_load = 5;
  result = manager.ResolveOverrides(draw);
  ASSERT_TRUE(result.headlocked);
  EXPECT_FLOAT_EQ(result.headlocked->scale, 30.0f);
  EXPECT_TRUE(result.headlocked->anchor);

  // An orthographic draw matches neither.
  draw.signature.perspective = false;
  EXPECT_EQ(manager.ResolveOverrides(draw).handling, HandlingType::Skip);

  manager.ApplyOverrideList("", {});
}
