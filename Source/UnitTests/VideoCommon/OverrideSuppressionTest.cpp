// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Common/FileUtil.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/OverrideSuppression.h"

namespace
{
std::vector<TextureElementManager::TextureElementOverride> CannonProfile()
{
  const std::string path = File::CreateTempDir() + "/profile.ini";
  EXPECT_TRUE(File::WriteStringToFile(path, "[TextureElementOverride_Enable]\n$Cannon\n"
                                            "[TextureElementOverride]\n$Cannon\n"
                                            "handling=controller_anchor\nanchor_hand=right\n"
                                            "texture=1789add6ac081b19\n"));
  return TextureElementManager::LoadOverridesFromINIFile(path);
}
}  // namespace

// A VR game patch that moves the arm cannon itself turns the profile's visual anchor of it off
// while it runs, without changing the user's files, and the anchor comes back when it stops.
TEST(OverrideSuppression, TurnsOverridesOffWhileSet)
{
  using VR::OverrideSuppression::Kind;
  auto& manager = TextureElementManager::GetInstance();
  VR::OverrideSuppression::Set({});
  manager.ApplyOverrideList("GTEST1", CannonProfile());
  EXPECT_TRUE(manager.HasControllerAnchorOverrides());

  VR::OverrideSuppression::Set({{Kind::Texture, "Cannon"}});
  EXPECT_TRUE(VR::OverrideSuppression::IsSuppressed(Kind::Texture, "Cannon"));
  EXPECT_FALSE(VR::OverrideSuppression::IsSuppressed(Kind::Elements, "Cannon"));
  // The video thread's end-of-frame check activates the same list again, without the anchor.
  manager.LoadOverridesIfNeeded("GTEST1");
  EXPECT_FALSE(manager.HasControllerAnchorOverrides());

  VR::OverrideSuppression::Set({});
  manager.LoadOverridesIfNeeded("GTEST1");
  EXPECT_TRUE(manager.HasControllerAnchorOverrides());

  manager.ApplyOverrideList("", {});
}
