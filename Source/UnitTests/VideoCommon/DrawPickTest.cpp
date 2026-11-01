// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "VideoCommon/ElementsGroupManager.h"
#include "VideoCommon/TextureElementManager.h"
#include "VideoCommon/VR/DrawPick.h"

using namespace VR::DrawPick;
using VR::AgentCapture::CapturedFrame;
using VR::AgentCapture::DrawIdMap;
using VR::AgentCapture::DrawRecord;

namespace
{
// A 4x2 map per eye: the left eye shows draw 3 on its left half and draw 5 on its right half
// (nothing at its top right corner); the right eye shows draw 7 everywhere.
CapturedFrame MakeFrame()
{
  CapturedFrame frame;
  DrawIdMap& map = frame.id_map;
  map.width = 4;
  map.height = 2;
  map.layers = 2;
  map.ids = {3, 3, 5, DrawIdMap::NO_DRAW, 3, 3, 5, 5, 7, 7, 7, 7, 7, 7, 7, 7};
  for (const u32 index : {3u, 5u})
  {
    DrawRecord& draw = frame.draws.emplace_back();
    draw.index = index;
  }
  return frame;
}
}  // namespace

TEST(DrawPick, FindsTheDrawAtAPoint)
{
  const CapturedFrame frame = MakeFrame();
  const DrawRecord* draw = FindDrawAt(frame, 0, 0.1f, 0.9f);
  ASSERT_NE(draw, nullptr);
  EXPECT_EQ(draw->index, 3u);
  draw = FindDrawAt(frame, 0, 0.6f, 0.9f);
  ASSERT_NE(draw, nullptr);
  EXPECT_EQ(draw->index, 5u);
  // Nothing drawn there.
  EXPECT_EQ(FindDrawAt(frame, 0, 0.9f, 0.1f), nullptr);
  // Draw 7 shows in the right eye, but the capture has no record of it.
  EXPECT_EQ(FindDrawAt(frame, 1, 0.5f, 0.5f), nullptr);
  // Outside the image, or without a map.
  EXPECT_EQ(FindDrawAt(frame, 0, 1.5f, 0.5f), nullptr);
  EXPECT_EQ(FindDrawAt(CapturedFrame{}, 0, 0.5f, 0.5f), nullptr);

  // A draw of a later frame of the capture is not the map's.
  CapturedFrame later = MakeFrame();
  for (DrawRecord& record : later.draws)
    record.frame = 1;
  EXPECT_EQ(FindDrawAt(later, 0, 0.1f, 0.9f), nullptr);

  // A mono map answers for both eyes.
  CapturedFrame mono = MakeFrame();
  mono.id_map.layers = 1;
  mono.id_map.ids.resize(8);
  draw = FindDrawAt(mono, 1, 0.1f, 0.9f);
  ASSERT_NE(draw, nullptr);
  EXPECT_EQ(draw->index, 3u);
}

TEST(DrawPick, MapsWindowPointsToEyeImages)
{
  // The frame drawn over a whole 1280x720 window.
  const MathUtil::Rectangle<int> full{0, 0, 1280, 720};
  auto point = MapWindowPoint(full, 1280, 720, WindowLayout::Mono, 320.0f, 180.0f);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->layer, 0u);
  EXPECT_FLOAT_EQ(point->u, 0.25f);
  EXPECT_FLOAT_EQ(point->v, 0.25f);
  EXPECT_FALSE(MapWindowPoint(full, 1280, 720, WindowLayout::Mono, 1300.0f, 10.0f));

  // Side by side: each eye in its half.
  point = MapWindowPoint(full, 1280, 720, WindowLayout::SideBySide, 320.0f, 360.0f);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->layer, 0u);
  EXPECT_FLOAT_EQ(point->u, 0.5f);
  EXPECT_FLOAT_EQ(point->v, 0.5f);
  point = MapWindowPoint(full, 1280, 720, WindowLayout::SideBySide, 960.0f, 360.0f);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->layer, 1u);
  EXPECT_FLOAT_EQ(point->u, 0.5f);

  // Top and bottom.
  point = MapWindowPoint(full, 1280, 720, WindowLayout::TopAndBottom, 640.0f, 540.0f);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->layer, 1u);
  EXPECT_FLOAT_EQ(point->u, 0.5f);
  EXPECT_FLOAT_EQ(point->v, 0.5f);

  // The mirror of one eye, letterboxed in the window.
  const MathUtil::Rectangle<int> boxed{160, 0, 1120, 720};
  point = MapWindowPoint(boxed, 1280, 720, WindowLayout::RightEye, 160.0f, 0.0f);
  ASSERT_TRUE(point);
  EXPECT_EQ(point->layer, 1u);
  EXPECT_FLOAT_EQ(point->u, 0.0f);
  EXPECT_FALSE(MapWindowPoint(boxed, 1280, 720, WindowLayout::LeftEye, 100.0f, 10.0f));
}

TEST(DrawPick, PicksForTheHunterThatIsOn)
{
  EXPECT_EQ(ResolveAutoTarget(true, true, true), Target::Shader);
  EXPECT_EQ(ResolveAutoTarget(false, true, true), Target::Elements);
  EXPECT_EQ(ResolveAutoTarget(false, false, true), Target::Texture);
  EXPECT_EQ(ResolveAutoTarget(false, false, false), Target::Shader);

  DrawRecord draw;
  draw.vs = 1;
  draw.ps = 2;
  draw.gs = 3;
  EXPECT_EQ(GetShaderHash(draw, ShaderHunter::ShaderType::Pixel), 2u);
  EXPECT_EQ(GetShaderHash(draw, ShaderHunter::ShaderType::Vertex), 1u);
  EXPECT_EQ(GetShaderHash(draw, ShaderHunter::ShaderType::Geometry), 3u);
  EXPECT_EQ(GetFirstTexture(draw), 0u);
  draw.textures[2] = 0xAB;
  draw.textures[5] = 0xCD;
  EXPECT_EQ(GetFirstTexture(draw), 0xABu);
}

TEST(DrawPick, ArmsOneClick)
{
  DisarmClick();
  EXPECT_FALSE(IsClickArmed());
  ArmClick(Target::HideCode);
  EXPECT_TRUE(IsClickArmed());
  EXPECT_EQ(TakeArmedClick(), Target::HideCode);
  EXPECT_FALSE(IsClickArmed());
  EXPECT_EQ(TakeArmedClick(), std::nullopt);

  ArmVRPick(Target::Elements);
  EXPECT_TRUE(IsVRPickArmed());
  EXPECT_EQ(TakeArmedVRPick(), Target::Elements);
  EXPECT_FALSE(IsVRPickArmed());
}

// What a pick selects in Elements hunting: the seed whose group holds the draw's signature.
TEST(DrawPick, FindsTheSeedOfASignature)
{
  ElementsGroupManager& elements = ElementsGroupManager::GetInstance();
  elements.SetPopupOpen(true);
  elements.SetHuntEnabled(true);
  ShaderHunter::RuntimeElementSignature hud;
  hud.valid = true;
  hud.ortho_right_x100 = 64000;
  hud.viewport_width = 640;
  ShaderHunter::RuntimeElementSignature world = hud;
  world.perspective = true;
  world.perspective_hfov_x100 = 8243;
  for (const auto& signature : {world, hud, world})
  {
    ElementsGroupManager::DrawRecord draw;
    draw.signature = signature;
    elements.RegisterDraw(draw);
  }
  elements.OnFrameEnd();
  EXPECT_EQ(elements.FindSeedCandidate(world), 0);
  EXPECT_EQ(elements.FindSeedCandidate(hud), 1);
  ShaderHunter::RuntimeElementSignature other = hud;
  other.viewport_width = 320;
  EXPECT_EQ(elements.FindSeedCandidate(other), -1);
  EXPECT_EQ(elements.FindSeedCandidate({}), -1);
  elements.SetHuntEnabled(false);
  elements.SetPopupOpen(false);
  elements.OnFrameEnd();
}

TEST(DrawPick, SelectsATextureByHash)
{
  TextureElementManager& textures = TextureElementManager::GetInstance();
  textures.SetHunterActive(true);
  std::array<u64, 8> bound{};
  bound[0] = 0x1111;
  bound[1] = 0x2222;
  textures.CaptureDrawTextures(bound, {});
  textures.OnFrameEnd();
  EXPECT_TRUE(textures.SelectTexture(0x2222));
  EXPECT_EQ(textures.GetSelectedTextureHash(), 0x2222u);
  EXPECT_EQ(textures.GetHuntingSelection().hash, 0x2222u);
  // A texture the last frame did not draw is reported (the selection shows no position then).
  EXPECT_FALSE(textures.SelectTexture(0x3333));
  EXPECT_EQ(textures.GetHuntingSelection().position, -1);
  textures.SetHunterActive(false);
}

// A picked hide code's preview paints pink what the code would hide: its lines, matched against
// the start of a batch's vertex data like the code itself.
TEST(DrawPick, HideCodePreviewMatchesItsVertexData)
{
  HideObjectEngine::Engine& engine = HideObjectEngine::Engine::GetInstance();
  HideObjectEngine::HideObjectEntry entry;
  entry.type = HideObjectEngine::HideObjectType::Bits16;
  entry.value_lower = 0xABCD;
  engine.SetPinkPreview({entry});
  EXPECT_TRUE(engine.HasPinkPreview());
  const u8 match[4] = {0xAB, 0xCD, 0x00, 0x11};
  const u8 other[4] = {0xAB, 0xCE, 0x00, 0x11};
  EXPECT_TRUE(engine.MatchesPinkPreview(match));
  EXPECT_FALSE(engine.MatchesPinkPreview(other));
  engine.SetPinkPreview({});
  EXPECT_FALSE(engine.HasPinkPreview());
}
