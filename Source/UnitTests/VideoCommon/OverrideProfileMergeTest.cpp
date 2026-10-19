// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Common/FileUtil.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

namespace
{
std::string WriteTemp(const std::string& contents)
{
  const std::string path = File::CreateTempDir() + "/profile.ini";
  EXPECT_TRUE(File::WriteStringToFile(path, contents));
  return path;
}

template <typename Override>
const Override* Find(const std::vector<Override>& overrides, const std::string& name)
{
  for (const Override& entry : overrides)
  {
    if (entry.name == name)
      return &entry;
  }
  return nullptr;
}

std::string ElementEntry(const std::string& name)
{
  return "$" + name +
         "\nformat=element_only_v6\nhandling=screen\nsig_perspective=0\nsig_use_viewport=1\n"
         "sig_vp_x=646\nsig_vp_y=570\nsig_vp_w=304\nsig_vp_h=228\n";
}
}  // namespace

// The user's file saves every override with an enable list. One saved before an override shipped
// in Sys does not list it: the new override keeps its Sys state. The overrides the user's file
// does define follow its list.
TEST(OverrideProfileMerge, ElementsUserFileKeepsNewSysOverrides)
{
  const std::string sys =
      WriteTemp("[ElementsGroupOverride_Enable]\n$HUD\n$Minimap\n"
                "[ElementsGroupOverride]\n" +
                ElementEntry("HUD") + "\n" + ElementEntry("Minimap") + "\n" + ElementEntry("Off"));
  const std::string user =
      WriteTemp("[ElementsGroupOverride_Enable]\n$Off\n[ElementsGroupOverride]\n" +
                ElementEntry("HUD") + "\n" + ElementEntry("Off"));

  const auto merged = ElementsGroupManager::LoadOverridesFromINIFiles({sys}, {user});
  ASSERT_EQ(merged.size(), 3u);
  // Turned off by the user.
  ASSERT_TRUE(Find(merged, "HUD"));
  EXPECT_FALSE(Find(merged, "HUD")->enabled);
  // Shipped after the user's file was saved.
  ASSERT_TRUE(Find(merged, "Minimap"));
  EXPECT_TRUE(Find(merged, "Minimap")->enabled);
  // Turned on by the user.
  ASSERT_TRUE(Find(merged, "Off"));
  EXPECT_TRUE(Find(merged, "Off")->enabled);
  EXPECT_FALSE(Find(merged, "Minimap")->user_defined);
}

TEST(OverrideProfileMerge, EnableListTurnsOnEarlierFilesOverrides)
{
  // A region file may only list an override that a shorter prefix file (RMC.ini) defines.
  const std::string prefix = WriteTemp("[ShaderOverride_Enable]\n[ShaderOverride]\n"
                                       "$Bloom\nHash=00000000deadbeef\nType=PS\n"
                                       "handling=fullscreen\n");
  const std::string region = WriteTemp("[ShaderOverride_Enable]\n$Bloom\n");
  const auto shaders = ShaderHunter::LoadOverridesFromINIFiles({prefix, region}, {});
  ASSERT_EQ(shaders.size(), 1u);
  EXPECT_TRUE(shaders[0].enabled);
}

TEST(OverrideProfileMerge, ShaderTextureAndHideObjectFollowTheSameRule)
{
  const std::string shader_sys =
      WriteTemp("[ShaderOverride_Enable]\n$New\n[ShaderOverride]\n"
                "$New\nHash=000000001d369b69\nType=PS\nhandling=screen\n");
  const std::string shader_user = WriteTemp("[ShaderOverride_Enable]\n[ShaderOverride]\n");
  const auto shaders = ShaderHunter::LoadOverridesFromINIFiles({shader_sys}, {shader_user});
  ASSERT_EQ(shaders.size(), 1u);
  EXPECT_TRUE(shaders[0].enabled);

  const std::string texture_sys = WriteTemp("[TextureElementOverride_Enable]\n$New\n"
                                            "[TextureElementOverride]\n"
                                            "$New\nhandling=screen\ntexture=0ea0e7ac012220d3\n");
  const std::string texture_user =
      WriteTemp("[TextureElementOverride_Enable]\n[TextureElementOverride]\n");
  const auto textures =
      TextureElementManager::LoadOverridesFromINIFiles({texture_sys}, {texture_user});
  ASSERT_EQ(textures.size(), 1u);
  EXPECT_TRUE(textures[0].enabled);

  const std::string hide_sys = WriteTemp("[HideObjectCodes_Enabled]\n$Map\n[HideObjectCodes]\n"
                                         "$Map\nType=128bits\nUpper=0x3C00800500FFF800\n"
                                         "Lower=0x000000000000003C\n");
  const std::string hide_user = WriteTemp("[HideObjectCodes_Enabled]\n[HideObjectCodes]\n");
  const auto codes = HideObjectEngine::LoadFromINIFiles({hide_sys}, {hide_user});
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_TRUE(codes[0].active);
}
