// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string>

#include "Common/FileUtil.h"
#include "Core/AgentServer/OverrideSession.h"
#include "Core/AgentServer/Tracepoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/HideObjectEngine.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

using namespace AgentServer;

namespace
{
constexpr char PROFILE[] = "[Graphics.VR]\n"
                           "UnitsPerMeter = 30\n"
                           "[ShaderOverride_Enable]\n"
                           "$HUD\n"
                           "[ShaderOverride]\n"
                           "$HUD\n"
                           "Hash=000000001d369b69\n"
                           "Type=PS\n"
                           "match_mode=exact_hash\n"
                           "handling=screen\n"
                           "element_depth=0.25\n"
                           "\n"
                           "$Bloom\n"
                           "Hash=00000000deadbeef\n"
                           "Type=VS\n"
                           "handling=fullscreen\n"
                           "[TextureElementOverride]\n"
                           "$Title\n"
                           "handling=headlocked\n"
                           "texture=0ea0e7ac012220d3\n";

std::string WriteTemp(const std::string& contents)
{
  const std::string dir = File::CreateTempDir();
  const std::string path = dir + "/profile.ini";
  EXPECT_TRUE(File::WriteStringToFile(path, contents));
  return path;
}
}  // namespace

TEST(AgentOverrides, ExtractsEntryText)
{
  EXPECT_EQ(ExtractOverrideEntryText(PROFILE, "ShaderOverride", "HUD"),
            "$HUD\nHash=000000001d369b69\nType=PS\nmatch_mode=exact_hash\nhandling=screen\n"
            "element_depth=0.25");
  EXPECT_EQ(ExtractOverrideEntryText(PROFILE, "ShaderOverride", "Bloom"),
            "$Bloom\nHash=00000000deadbeef\nType=VS\nhandling=fullscreen");
  EXPECT_EQ(ExtractOverrideEntryText(PROFILE, "TextureElementOverride", "Title"),
            "$Title\nhandling=headlocked\ntexture=0ea0e7ac012220d3");
  // The enable list is not an entry.
  EXPECT_EQ(ExtractOverrideEntryText(PROFILE, "ShaderOverride_Enable", "HUD"), "$HUD");
  EXPECT_EQ(ExtractOverrideEntryText(PROFILE, "ShaderOverride", "Missing"), std::nullopt);
  EXPECT_EQ(ExtractOverrideEntryText(PROFILE, "ElementsGroupOverride", "HUD"), std::nullopt);
}

TEST(AgentOverrides, KindNames)
{
  for (const OverrideKind kind : ALL_OVERRIDE_KINDS)
    EXPECT_EQ(ParseOverrideKind(GetOverrideKindName(kind)), kind);
  EXPECT_EQ(ParseOverrideKind("Elements_Group"), OverrideKind::Elements);
  EXPECT_EQ(ParseOverrideKind("hide"), OverrideKind::HideObject);
  EXPECT_EQ(ParseOverrideKind("nope"), std::nullopt);
  EXPECT_EQ(GetOverrideEnableSectionName(OverrideKind::HideObject), "HideObjectCodes_Enabled");
}

TEST(AgentOverrides, ManagersParseOneFile)
{
  const std::string path = WriteTemp(PROFILE);

  const auto shaders = ShaderHunter::LoadOverridesFromINIFile(path);
  ASSERT_EQ(shaders.size(), 2u);
  EXPECT_EQ(shaders[0].name, "HUD");
  EXPECT_EQ(shaders[0].hash, 0x1d369b69u);
  EXPECT_EQ(shaders[0].type, ShaderHunter::ShaderType::Pixel);
  EXPECT_EQ(shaders[0].handling, ShaderHunter::HandlingType::Screen);
  EXPECT_TRUE(shaders[0].enabled);
  // Not in the enable list.
  EXPECT_FALSE(shaders[1].enabled);
  EXPECT_EQ(shaders[1].type, ShaderHunter::ShaderType::Vertex);

  const auto textures = TextureElementManager::LoadOverridesFromINIFile(path);
  ASSERT_EQ(textures.size(), 1u);
  EXPECT_EQ(textures[0].handling, ShaderHunter::HandlingType::HeadLocked);
  ASSERT_EQ(textures[0].texture_hashes.size(), 1u);
  EXPECT_EQ(textures[0].texture_hashes[0], 0x0ea0e7ac012220d3u);

  const std::string elements_path = WriteTemp("[ElementsGroupOverride_Enable]\n$HUD\n"
                                              "[ElementsGroupOverride]\n$HUD\n"
                                              "format=element_only_v6\nhandling=screen\n"
                                              "sig_perspective=1\nsig_use_viewport=1\n"
                                              "sig_vp_x=662\nsig_vp_y=582\nsig_vp_w=320\n"
                                              "sig_vp_h=240\n");
  const auto elements = ElementsGroupManager::LoadOverridesFromINIFile(elements_path);
  ASSERT_EQ(elements.size(), 1u);
  EXPECT_EQ(elements[0].handling, ShaderHunter::HandlingType::Screen);
  EXPECT_TRUE(elements[0].runtime_element.use_viewport);
  EXPECT_EQ(elements[0].runtime_element.viewport_x, 662);
  EXPECT_EQ(elements[0].runtime_element.viewport_height, 240);

  const std::string hide_path = WriteTemp("[HideObjectCodes_Enabled]\n$Map\n[HideObjectCodes]\n"
                                          "$Map\nType=128bits\nUpper=0x3C00800500FFF800\n"
                                          "Lower=0x000000000000003C\n");
  const auto codes = HideObjectEngine::LoadCodesFromINIFile(hide_path);
  ASSERT_EQ(codes.size(), 1u);
  ASSERT_EQ(codes[0].entries.size(), 1u);
  EXPECT_EQ(codes[0].entries[0].value_upper, 0x3C00800500FFF800u);
  EXPECT_EQ(codes[0].entries[0].value_lower, 0x3Cu);
}

TEST(AgentTracepoints, RecordsOnlyArmedPoints)
{
  PowerPC::PowerPCState state;
  std::atomic<u64> fields{7};
  Tracepoints::Arm(
      {{0x80001000, Tracepoints::HitKind::Entry}, {0x80001010, Tracepoints::HitKind::Return}}, 3,
      &fields);
  EXPECT_TRUE(Tracepoints::IsActive());

  state.pc = 0x80001000;
  state.gpr[3] = 42;
  Tracepoints::OnCodeHit(state);
  state.pc = 0x80002000;  // not armed
  Tracepoints::OnCodeHit(state);
  state.pc = 0x80001010;
  state.gpr[3] = 1;
  Tracepoints::OnCodeHit(state);
  Tracepoints::OnMemoryHit(state, 0x80400000, 2, true);
  // Beyond max_hits.
  Tracepoints::OnCodeHit(state);
  EXPECT_EQ(Tracepoints::GetHitCount(), 3u);

  const std::vector<Tracepoints::Hit> hits = Tracepoints::Disarm();
  EXPECT_FALSE(Tracepoints::IsActive());
  ASSERT_EQ(hits.size(), 3u);
  EXPECT_EQ(hits[0].kind, Tracepoints::HitKind::Entry);
  EXPECT_EQ(hits[0].gpr[0], 42u);
  EXPECT_EQ(hits[0].field, 7u);
  EXPECT_EQ(hits[1].kind, Tracepoints::HitKind::Return);
  EXPECT_EQ(hits[1].gpr[0], 1u);
  EXPECT_EQ(hits[2].kind, Tracepoints::HitKind::MemoryWrite);
  EXPECT_EQ(hits[2].address, 0x80400000u);
  EXPECT_EQ(hits[2].value, 2u);
}
