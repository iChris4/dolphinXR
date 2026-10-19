// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "VideoCommon/VR/AgentCapture.h"
#include "VideoCommon/VR/VRFrameRegion.h"

using namespace VR::AgentCapture;

namespace
{
DrawRecord MakeDraw(u32 index, bool perspective)
{
  DrawRecord draw;
  draw.index = index;
  draw.perspective = perspective;
  draw.vs = 0x1111;
  draw.ps = 0x2222;
  draw.gs = 0x3333;
  draw.ps_family = 0xF00D;
  draw.textures[1] = 0xABCDEF;
  draw.index_count = 60;
  return draw;
}
}  // namespace

TEST(AgentCapture, DownscaleAveragesBoxes)
{
  // 4x2 image: left half black, right half white -> 2x1: black, white.
  std::vector<u8> rgb(4 * 2 * 3, 0);
  for (u32 y = 0; y < 2; ++y)
  {
    for (u32 x = 2; x < 4; ++x)
    {
      for (u32 c = 0; c < 3; ++c)
        rgb[(y * 4 + x) * 3 + c] = 255;
    }
  }
  const std::vector<u8> out = DownscaleRGB(rgb, 4, 2, 2, 1);
  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[3], 255);
  EXPECT_EQ(DownscaleRGB(rgb, 4, 2, 4, 2), rgb);
}

TEST(AgentCapture, PreviewMatchesAnyCriterion)
{
  PreviewSet preview;
  preview.shader_hashes = {0x2222};
  const std::array<u64, 8> no_textures{};
  EXPECT_TRUE(preview.Matches(0, 1, 0x2222, 3, 0, 0, 0, no_textures));
  EXPECT_FALSE(preview.Matches(0, 1, 2, 3, 0, 0, 0, no_textures));

  preview = {};
  preview.families = {0xF00D};
  EXPECT_TRUE(preview.Matches(0, 1, 2, 3, 0, 0xF00D, 0, no_textures));
  // A zero family (not computed) never matches.
  preview.families = {0};
  EXPECT_FALSE(preview.Matches(0, 1, 2, 3, 0, 0, 0, no_textures));

  preview = {};
  preview.indices = {7};
  EXPECT_TRUE(preview.Matches(7, 1, 2, 3, 0, 0, 0, no_textures));
  EXPECT_FALSE(preview.Matches(8, 1, 2, 3, 0, 0, 0, no_textures));

  preview = {};
  preview.textures = {0xABCDEF};
  std::array<u64, 8> textures{};
  textures[3] = 0xABCDEF;
  EXPECT_TRUE(preview.Matches(0, 1, 2, 3, 0, 0, 0, textures));
  EXPECT_FALSE(preview.Matches(0, 1, 2, 3, 0, 0, 0, no_textures));
  EXPECT_FALSE(preview.Empty());
  EXPECT_TRUE(PreviewSet{}.Empty());
}

TEST(AgentCapture, PreviewModes)
{
  const std::array<u64, 8> no_textures{};
  PreviewSet preview;
  preview.indices = {3};

  preview.mode = PreviewSet::Mode::Pink;
  SetPreview(preview);
  EXPECT_TRUE(HasPreview());
  EXPECT_EQ(MatchPreview(3, 0, 0, 0, 0, 0, 0, no_textures), PreviewAction::Pink);
  EXPECT_EQ(MatchPreview(4, 0, 0, 0, 0, 0, 0, no_textures), PreviewAction::None);

  preview.mode = PreviewSet::Mode::Skip;
  SetPreview(preview);
  EXPECT_EQ(MatchPreview(3, 0, 0, 0, 0, 0, 0, no_textures), PreviewAction::Skip);
  EXPECT_EQ(MatchPreview(4, 0, 0, 0, 0, 0, 0, no_textures), PreviewAction::None);

  preview.mode = PreviewSet::Mode::Isolate;
  SetPreview(preview);
  EXPECT_EQ(MatchPreview(3, 0, 0, 0, 0, 0, 0, no_textures), PreviewAction::None);
  EXPECT_EQ(MatchPreview(4, 0, 0, 0, 0, 0, 0, no_textures), PreviewAction::Skip);

  SetPreview({});
  EXPECT_FALSE(HasPreview());
}

TEST(AgentCapture, FiltersDraws)
{
  DrawRecord ortho = MakeDraw(5, false);
  DrawRecord persp = MakeDraw(6, true);
  persp.handling = 1;  // screen
  persp.skip = SkipReason::Shader;

  DrawFilter filter;
  EXPECT_TRUE(MatchesFilter(ortho, filter));

  filter.perspective = false;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  EXPECT_FALSE(MatchesFilter(persp, filter));

  filter = {};
  filter.handling = NO_HANDLING;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  EXPECT_FALSE(MatchesFilter(persp, filter));

  filter = {};
  filter.skipped = true;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
  EXPECT_TRUE(MatchesFilter(persp, filter));

  filter = {};
  filter.shader = 0xF00D;  // a family
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  filter.shader = 0x9999;
  EXPECT_FALSE(MatchesFilter(ortho, filter));

  filter = {};
  filter.texture = 0xABCDEF;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  filter.texture = 0x1;
  EXPECT_FALSE(MatchesFilter(ortho, filter));

  filter = {};
  filter.index_min = 6;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
  EXPECT_TRUE(MatchesFilter(persp, filter));
  filter = {};
  filter.min_index_count = 61;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
}

TEST(AgentCapture, HandlingNamesRoundTrip)
{
  for (u8 handling = 0; handling <= 10; ++handling)
    EXPECT_EQ(ParseHandlingName(GetHandlingName(handling)), handling) << int(handling);
  EXPECT_EQ(ParseHandlingName("none"), NO_HANDLING);
  EXPECT_EQ(ParseHandlingName("Screen"), u8{1});
  EXPECT_EQ(ParseHandlingName("bogus"), std::nullopt);
  EXPECT_STREQ(GetHandlingName(NO_HANDLING), "-");
}

TEST(AgentCapture, DescribesRoutes)
{
  DrawRecord draw = MakeDraw(0, true);
  EXPECT_EQ(DescribeRoute(draw), "stereo 3D");
  draw.perspective = false;
  EXPECT_EQ(DescribeRoute(draw), "virtual screen");
  draw.stereo_override = -1.0f;
  EXPECT_EQ(DescribeRoute(draw), "virtual screen");
  draw.stereo_override = 0.0f;
  EXPECT_EQ(DescribeRoute(draw), "fullscreen (auto EFB effect)");
  draw.handling = 2;
  EXPECT_EQ(DescribeRoute(draw), "fullscreen");
  draw.stereo_override = -2.0f;
  EXPECT_EQ(DescribeRoute(draw), "head-locked");
  draw.stereo_override = 2.0f;
  EXPECT_EQ(DescribeRoute(draw), "screen pane");
  draw.skip = SkipReason::AgentPreview;
  EXPECT_EQ(DescribeRoute(draw), "not drawn");
}

// Draws without an override follow the VR settings, as GeometryShaderManager routes them.
TEST(AgentCapture, DescribesDefaultRoutes)
{
  DrawRecord ortho = MakeDraw(0, false);
  ortho.virtual_screen = false;
  EXPECT_EQ(DescribeRoute(ortho), "2D per eye, head-locked (VirtualScreen is off)");
  ortho.virtual_screen = true;
  ortho.detect_render_targets = true;
  ortho.viewport_class = static_cast<u8>(VR::VRViewportClass::RenderToTexture);
  EXPECT_EQ(DescribeRoute(ortho), "native (render to texture)");

  DrawRecord pane = MakeDraw(1, true);
  pane.viewport_class = static_cast<u8>(VR::VRViewportClass::HudElement);
  pane.frame_region_valid = true;
  EXPECT_EQ(DescribeRoute(pane), "virtual screen (small 3D pane)");
  pane.panes_on_screen = false;
  EXPECT_EQ(DescribeRoute(pane), "stereo 3D");
}

TEST(AgentCapture, FormatsRows)
{
  DrawRecord draw = MakeDraw(42, false);
  draw.projection = {0, 640, 0, 480, 0, 1};
  const std::string row = FormatDrawRow(draw);
  EXPECT_NE(row.find("#42"), std::string::npos) << row;
  EXPECT_NE(row.find("ps 00002222"), std::string::npos) << row;
  EXPECT_NE(row.find("0000000000abcdef"), std::string::npos) << row;
  EXPECT_NE(row.find("virtual screen"), std::string::npos) << row;

  const std::string details = FormatDrawDetails(draw);
  EXPECT_NE(details.find("orthographic"), std::string::npos) << details;
  EXPECT_NE(details.find("Texture 1: 0000000000abcdef"), std::string::npos) << details;

  // Ranges without "-0", and the depth/blend column without stray spaces.
  draw.projection = {-0.0f, 608, 72, -0.0f, 0, 1};
  draw.ztest = false;
  draw.zupdate = false;
  draw.blend = true;
  const std::string ortho = FormatDrawRow(draw);
  EXPECT_NE(ortho.find("O x 0..608 y 72..0 |"), std::string::npos) << ortho;
  EXPECT_NE(ortho.find("| blend |"), std::string::npos) << ortho;
  EXPECT_EQ(FormatDrawDetails(draw).find("-0"), std::string::npos);
}
