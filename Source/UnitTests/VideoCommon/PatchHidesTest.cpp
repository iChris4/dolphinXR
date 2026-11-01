// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "Common/FileUtil.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/PatchFlags.h"
#include "VideoCommon/VR/PatchHides.h"

namespace
{
constexpr u64 HEAD = 0xd7f168afaf5c673aULL;

// The profile routes the head's texture to the virtual screen (any handling other than Skip).
std::vector<TextureElementManager::TextureElementOverride> HeadProfile()
{
  const std::string path = File::CreateTempDir() + "/profile.ini";
  EXPECT_TRUE(File::WriteStringToFile(path, "[TextureElementOverride_Enable]\n$Head\n"
                                            "[TextureElementOverride]\n$Head\n"
                                            "handling=fullscreen\n"
                                            "texture=d7f168afaf5c673a\n"));
  return TextureElementManager::LoadOverridesFromINIFile(path);
}
}  // namespace

// A VR patch's `hide` textures are skipped while its flag is raised (its Camera Anchor holds the
// camera), ahead of the profile's entry for the same texture, which applies again when the flag
// is lowered; the user's files are not changed.
TEST(PatchHides, HideWhileTheFlagIsRaised)
{
  auto& manager = TextureElementManager::GetInstance();
  auto& hunter = ShaderHunter::GetInstance();
  const std::array<u64, 8> bound = {HEAD};
  VR::PatchHides::Set({});
  VR::PatchFlags::Set({});
  hunter.OnFrameEnd();
  manager.ApplyOverrideList("GTEST1", HeadProfile());
  EXPECT_FALSE(manager.ShouldSkipByTexture(bound, nullptr));
  EXPECT_EQ(
      manager.GetHandlingForTextures(bound, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
      ShaderHunter::HandlingType::Fullscreen);

  const std::string flag = VR::PatchHides::FlagFor("first-person");
  EXPECT_EQ(flag, "vr.anchor.first-person");
  VR::PatchHides::Set({{"first-person", {HEAD}, flag}});
  // The video thread's end-of-frame check activates the same list again, with the hide.
  manager.LoadOverridesIfNeeded("GTEST1");
  EXPECT_FALSE(manager.ShouldSkipByTexture(bound, nullptr));

  VR::PatchFlags::Set({flag});
  hunter.OnFrameEnd();
  EXPECT_TRUE(manager.ShouldSkipByTexture(bound, nullptr));
  // Other textures are untouched.
  EXPECT_FALSE(manager.ShouldSkipByTexture({0x1234}, nullptr));

  VR::PatchFlags::Set({});
  hunter.OnFrameEnd();
  EXPECT_FALSE(manager.ShouldSkipByTexture(bound, nullptr));
  EXPECT_EQ(
      manager.GetHandlingForTextures(bound, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
      ShaderHunter::HandlingType::Fullscreen);

  // The patch stopped: its hide goes even with the flag raised.
  VR::PatchHides::Set({});
  manager.LoadOverridesIfNeeded("GTEST1");
  VR::PatchFlags::Set({flag});
  hunter.OnFrameEnd();
  EXPECT_FALSE(manager.ShouldSkipByTexture(bound, nullptr));

  VR::PatchFlags::Set({});
  hunter.OnFrameEnd();
  manager.ApplyOverrideList("", {});
}

// Without a profile entry for the game, a hide alone makes the manager active.
TEST(PatchHides, ActivateTheManagerAlone)
{
  auto& manager = TextureElementManager::GetInstance();
  VR::PatchHides::Set({{"first-person", {HEAD}, "vr.anchor.first-person"}});
  manager.ApplyOverrideList("GTEST1", {});
  EXPECT_TRUE(manager.HasOverrides());
  VR::PatchHides::Set({});
  manager.LoadOverridesIfNeeded("GTEST1");
  EXPECT_FALSE(manager.HasOverrides());
  manager.ApplyOverrideList("", {});
}
