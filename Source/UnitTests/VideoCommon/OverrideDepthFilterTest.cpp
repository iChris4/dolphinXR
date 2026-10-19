// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "VideoCommon/DrawDepth.h"
#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/ShaderHunter.h"
#include "VideoCommon/TextureElementManager.h"

namespace
{
using HandlingType = ShaderHunter::HandlingType;

std::string WriteTemp(const std::string& contents)
{
  const std::string path = File::CreateTempDir() + "/profile.ini";
  EXPECT_TRUE(File::WriteStringToFile(path, contents));
  return path;
}

// A quad at a given camera depth, with identity position matrices.
class Quad
{
public:
  explicit Quad(float depth)
  {
    m_decl.stride = 12;
    m_decl.position.enable = true;
    m_decl.position.type = ComponentFormat::Float;
    m_decl.position.components = 3;
    m_matrices[10] = 1.0f;  // matrix 0, z row (0, 0, 1, 0)
    for (int i = 0; i < 4; ++i)
    {
      const float position[3] = {static_cast<float>(i & 1), static_cast<float>(i >> 1), -depth};
      std::memcpy(&m_vertices[i * 12], position, sizeof(position));
    }
    m_source = DrawDepthSource(m_vertices.data(), 4, 12, &m_decl, m_matrices.data(), 0);
  }
  const DrawDepthSource* Get() const { return &m_source; }

private:
  PortableVertexDeclaration m_decl;
  std::array<float, 268> m_matrices{};
  std::array<u8, 48> m_vertices{};
  DrawDepthSource m_source;
};

constexpr u64 ICON_TEXTURE = 0x2df5d24bb14cbfc9;
}  // namespace

TEST(OverrideDepthFilter, ParsesTheKeys)
{
  const std::string elements = WriteTemp("[ElementsGroupOverride]\n$Near\nformat=element_only_v6\n"
                                         "handling=screen\nmin_depth=2.5\nmax_depth=10\n"
                                         "sig_perspective=1\nsig_use_projection_type=1\n"
                                         "\n$Unset\nformat=element_only_v6\nhandling=screen\n"
                                         "max_depth=-3\nsig_perspective=1\n");
  const auto element_entries = ElementsGroupManager::LoadOverridesFromINIFile(elements);
  ASSERT_EQ(element_entries.size(), 2u);
  EXPECT_FLOAT_EQ(element_entries[0].min_depth, 2.5f);
  EXPECT_FLOAT_EQ(element_entries[0].max_depth, 10.0f);
  // A non-positive value is no limit.
  EXPECT_FALSE(element_entries[1].max_depth > 0.0f);

  const std::string textures =
      WriteTemp("[TextureElementOverride]\n$Item\nhandling=screen\nmax_depth=8\n"
                "texture=2df5d24bb14cbfc9\n");
  const auto texture_entries = TextureElementManager::LoadOverridesFromINIFile(textures);
  ASSERT_EQ(texture_entries.size(), 1u);
  EXPECT_FLOAT_EQ(texture_entries[0].max_depth, 8.0f);
  EXPECT_FLOAT_EQ(texture_entries[0].min_depth, 0.0f);

  const std::string shaders = WriteTemp("[ShaderOverride]\n$Far\nHash=0000000000001234\nType=PS\n"
                                        "handling=screen\nmin_depth=40\n");
  const auto shader_entries = ShaderHunter::LoadOverridesFromINIFile(shaders);
  ASSERT_EQ(shader_entries.size(), 1u);
  EXPECT_FLOAT_EQ(shader_entries[0].min_depth, 40.0f);
  EXPECT_FLOAT_EQ(shader_entries[0].max_depth, 0.0f);
}

TEST(OverrideDepthFilter, SavesTheKeys)
{
  const std::string previous = File::GetUserPath(D_GAMESETTINGSVR_IDX);
  File::SetUserPath(D_GAMESETTINGSVR_IDX, File::CreateTempDir() + "/");

  TextureElementManager::TextureElementOverride texture;
  texture.name = "Item";
  texture.handling = HandlingType::Screen;
  texture.texture_hashes = {ICON_TEXTURE};
  texture.max_depth = 7.5f;
  TextureElementManager::SaveOverridesToINI("DEPTH1", {texture});
  const auto textures = TextureElementManager::LoadOverridesFromINI("DEPTH1");

  ElementsGroupManager::ElementGroupOverride element;
  element.name = "Near";
  element.handling = HandlingType::Screen;
  element.runtime_element.valid = true;
  element.min_depth = 1.25f;
  ElementsGroupManager::SaveOverridesToINI("DEPTH1", {element});
  const auto elements = ElementsGroupManager::LoadOverridesFromINI("DEPTH1");

  ShaderHunter::ShaderOverride shader;
  shader.name = "HUD";
  shader.hash = 0x1234;
  shader.type = ShaderHunter::ShaderType::Pixel;
  shader.handling = HandlingType::Screen;
  shader.max_depth = 12.0f;
  ShaderHunter::SaveOverridesToINI("DEPTH1", {shader});
  const auto shaders = ShaderHunter::LoadOverridesFromINI("DEPTH1");

  File::SetUserPath(D_GAMESETTINGSVR_IDX, previous);

  ASSERT_EQ(textures.size(), 1u);
  EXPECT_FLOAT_EQ(textures[0].max_depth, 7.5f);
  ASSERT_EQ(elements.size(), 1u);
  EXPECT_FLOAT_EQ(elements[0].min_depth, 1.25f);
  EXPECT_FLOAT_EQ(elements[0].max_depth, 0.0f);
  ASSERT_EQ(shaders.size(), 1u);
  EXPECT_FLOAT_EQ(shaders[0].max_depth, 12.0f);
}

TEST(OverrideDepthFilter, ElementsMatchOnlyNearDraws)
{
  ElementsGroupManager::ElementGroupOverride entry;
  entry.name = "Popup";
  entry.handling = HandlingType::Screen;
  entry.runtime_element.valid = true;
  entry.runtime_element.perspective = true;
  entry.runtime_element.use_projection_type = true;
  entry.max_depth = 8.0f;
  ElementsGroupManager::ElementGroupOverride skip = entry;
  skip.name = "Hide far";
  skip.handling = HandlingType::Skip;
  skip.max_depth = 0.0f;
  skip.min_depth = 100.0f;

  auto& manager = ElementsGroupManager::GetInstance();
  manager.ApplyOverrideList("DEPTH2", {entry, skip});

  ElementsGroupManager::DrawRecord draw;
  draw.signature.valid = true;
  draw.signature.perspective = true;

  const Quad near(2.0f), far(30.0f), very_far(500.0f);
  draw.depth = near.Get();
  EXPECT_EQ(manager.ResolveOverrides(draw).handling, HandlingType::Screen);
  draw.depth = far.Get();
  const auto far_result = manager.ResolveOverrides(draw);
  EXPECT_EQ(far_result.handling, HandlingType::Skip);
  EXPECT_FALSE(far_result.skip);
  draw.depth = very_far.Get();
  EXPECT_TRUE(manager.ResolveOverrides(draw).skip);
  // A draw whose depth is unknown does not match an entry with a depth limit.
  draw.depth = nullptr;
  EXPECT_EQ(manager.ResolveOverrides(draw).handling, HandlingType::Skip);

  // Without a limit the entry matches whatever the depth.
  entry.max_depth = 0.0f;
  manager.ApplyOverrideList("DEPTH2", {entry});
  EXPECT_EQ(manager.ResolveOverrides(draw).handling, HandlingType::Screen);

  manager.ApplyOverrideList("", {});
}

TEST(OverrideDepthFilter, TextureFallsThroughToTheNextEntry)
{
  TextureElementManager::TextureElementOverride popup;
  popup.name = "Popup";
  popup.handling = HandlingType::Screen;
  popup.texture_hashes = {ICON_TEXTURE};
  popup.max_depth = 8.0f;
  TextureElementManager::TextureElementOverride other = popup;
  other.name = "Other";
  other.handling = HandlingType::HeadLocked;
  other.max_depth = 0.0f;

  auto& manager = TextureElementManager::GetInstance();
  manager.ApplyOverrideList("DEPTH3", {popup, other});

  const std::array<u64, 8> bound = {ICON_TEXTURE};
  const Quad near(2.0f), far(30.0f);
  const auto handling = [&](const DrawDepthSource* depth) {
    return manager.GetHandlingForTextures(bound, depth, nullptr, nullptr, nullptr, nullptr,
                                          nullptr);
  };
  EXPECT_EQ(handling(near.Get()), HandlingType::Screen);
  EXPECT_EQ(handling(far.Get()), HandlingType::HeadLocked);
  EXPECT_EQ(handling(nullptr), HandlingType::HeadLocked);

  TextureElementManager::TextureElementOverride skip = popup;
  skip.handling = HandlingType::Skip;
  manager.ApplyOverrideList("DEPTH3", {skip});
  EXPECT_TRUE(manager.ShouldSkipByTexture(bound, near.Get()));
  EXPECT_FALSE(manager.ShouldSkipByTexture(bound, far.Get()));

  manager.ApplyOverrideList("", {});
}

TEST(OverrideDepthFilter, ShaderMatchesOnlyNearDraws)
{
  ShaderHunter::ShaderOverride popup;
  popup.name = "Popup";
  popup.hash = 0x1234;
  popup.type = ShaderHunter::ShaderType::Pixel;
  popup.handling = HandlingType::Screen;
  popup.max_depth = 8.0f;
  ShaderHunter::ShaderOverride far_skip = popup;
  far_skip.name = "Far";
  far_skip.hash = 0x5678;
  far_skip.handling = HandlingType::Skip;
  far_skip.max_depth = 0.0f;
  far_skip.min_depth = 100.0f;

  auto& hunter = ShaderHunter::GetInstance();
  hunter.ApplyOverrideList("DEPTH4", {popup, far_skip});

  const Quad near(2.0f), far(30.0f), very_far(500.0f);
  hunter.SetCurrentDrawDepth(near.Get());
  EXPECT_EQ(hunter.GetOverrideHandling(0, 0x1234, 0), HandlingType::Screen);
  hunter.SetCurrentDrawDepth(far.Get());
  EXPECT_EQ(hunter.GetOverrideHandling(0, 0x1234, 0), HandlingType::Skip);
  hunter.SetCurrentDrawDepth(nullptr);
  EXPECT_EQ(hunter.GetOverrideHandling(0, 0x1234, 0), HandlingType::Skip);

  hunter.SetCurrentDrawDepth(very_far.Get());
  EXPECT_TRUE(hunter.ShouldSkipByOverride(0, 0x5678, 0));
  hunter.SetCurrentDrawDepth(far.Get());
  EXPECT_FALSE(hunter.ShouldSkipByOverride(0, 0x5678, 0));

  hunter.SetCurrentDrawDepth(nullptr);
  hunter.ApplyOverrideList("", {});
}
