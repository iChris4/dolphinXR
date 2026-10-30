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

  // Frames of a multi-frame capture.
  ortho.frame = 3;
  filter = {};
  filter.frame = 3;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  filter.frame = 0;
  EXPECT_FALSE(MatchesFilter(ortho, filter));

  // A point of the screen: only draws with a known box holding it.
  filter = {};
  filter.screen_x = 0.5f;
  filter.screen_y = 0.5f;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
  ortho.screen_left = 0.4f;
  ortho.screen_top = 0.45f;
  ortho.screen_right = 0.6f;
  ortho.screen_bottom = 0.55f;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  filter.screen_x = 0.7f;
  EXPECT_FALSE(MatchesFilter(ortho, filter));

  // Visibility needs a draw-ID map.
  filter = {};
  filter.visible = true;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
  ortho.visible_pixels = 0;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
  filter.visible = false;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
  ortho.visible_pixels = 12;
  EXPECT_FALSE(MatchesFilter(ortho, filter));
  filter.visible = true;
  EXPECT_TRUE(MatchesFilter(ortho, filter));
}

TEST(AgentCapture, ProjectsOrthographicPointsToTheFrame)
{
  // A 640x480 GX screen with (0,0) at the top left: ndc x = x / 320 - 1, ndc y = 1 - y / 240.
  const std::array<float, 6> projection = {2.0f / 640.0f, -1.0f, -2.0f / 480.0f, 1.0f, 0.0f, 1.0f};
  VR::VRFrameRegion frame;
  frame.left = 0;
  frame.top = 0;
  frame.width = 640;
  frame.height = 480;
  frame.valid = true;
  const std::vector<std::array<float, 3>> points = {{160, 120, 0}, {480, 360, 0}};
  // The full-screen viewport: centre 342+320,342+240, half extents 320 x -240.
  const ScreenBox box = ProjectViewPointsToFrame(points, projection, false, 662.0f, 582.0f, 320.0f,
                                                 -240.0f, 342, 342, frame);
  ASSERT_TRUE(box.valid);
  EXPECT_FALSE(box.partial);
  EXPECT_FLOAT_EQ(box.left, 0.25f);
  EXPECT_FLOAT_EQ(box.top, 0.25f);
  EXPECT_FLOAT_EQ(box.right, 0.75f);
  EXPECT_FLOAT_EQ(box.bottom, 0.75f);

  // A frame of 448 lines starting at line 16 (Metroid Prime Trilogy): y moves with it.
  frame.top = 16;
  frame.height = 448;
  const ScreenBox boxed = ProjectViewPointsToFrame(points, projection, false, 662.0f, 582.0f,
                                                   320.0f, -240.0f, 342, 342, frame);
  ASSERT_TRUE(boxed.valid);
  EXPECT_FLOAT_EQ(boxed.top, (120.0f - 16.0f) / 448.0f);
  EXPECT_FLOAT_EQ(boxed.left, 0.25f);

  // Without a frame region, the whole EFB (640x528) is the frame.
  frame.valid = false;
  const ScreenBox efb = ProjectViewPointsToFrame(points, projection, false, 662.0f, 582.0f, 320.0f,
                                                 -240.0f, 342, 342, frame);
  ASSERT_TRUE(efb.valid);
  EXPECT_FLOAT_EQ(efb.top, 120.0f / 528.0f);

  EXPECT_FALSE(ProjectViewPointsToFrame({}, projection, false, 662.0f, 582.0f, 320.0f, -240.0f, 342,
                                        342, frame)
                   .valid);
}

TEST(AgentCapture, ProjectsPerspectivePointsToTheFrame)
{
  // A 90 degree field of view: ndc x = x / -z, ndc y = y / -z.
  const std::array<float, 6> projection = {1.0f, 0.0f, 1.0f, 0.0f, -1.0f, -2.0f};
  VR::VRFrameRegion frame;
  frame.width = 640;
  frame.height = 480;
  frame.valid = true;
  const std::vector<std::array<float, 3>> points = {{5, -5, -10}, {0, 0, -10}, {0, 0, 5}};
  const ScreenBox box = ProjectViewPointsToFrame(points, projection, true, 662.0f, 582.0f, 320.0f,
                                                 -240.0f, 342, 342, frame);
  ASSERT_TRUE(box.valid);
  // The point behind the camera is left out.
  EXPECT_TRUE(box.partial);
  EXPECT_FLOAT_EQ(box.left, 0.5f);
  EXPECT_FLOAT_EQ(box.top, 0.5f);
  EXPECT_FLOAT_EQ(box.right, 0.75f);
  EXPECT_FLOAT_EQ(box.bottom, 0.75f);

  const ScreenBox behind = ProjectViewPointsToFrame(
      {{0, 0, 5}, {0, 0, 0}}, projection, true, 662.0f, 582.0f, 320.0f, -240.0f, 342, 342, frame);
  EXPECT_FALSE(behind.valid);
  EXPECT_TRUE(behind.partial);
}

TEST(AgentCapture, CapturesConsecutiveFrames)
{
  ResetForShutdown();
  RequestCapture(7, 2);
  EXPECT_FALSE(IsCapturing());
  // The capture starts at the next frame boundary.
  OnEndFrame(0, false);
  ASSERT_TRUE(IsCapturing());

  RecordCopy({.texture = 0x11});
  DrawRecord* draw = BeginRecord();
  ASSERT_NE(draw, nullptr);
  draw->index = 0;
  RecordCopy({.texture = 0x22});
  OnEndFrame(1, false);

  // Still recording: the second frame.
  ASSERT_TRUE(IsCapturing());
  EXPECT_EQ(GetLastCapture(), nullptr);
  draw = BeginRecord();
  ASSERT_NE(draw, nullptr);
  draw->index = 0;
  RecordCopy({.xfb = true});
  OnEndFrame(2, false);

  EXPECT_FALSE(IsCapturing());
  const auto capture = GetLastCapture();
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(capture->id, 7u);
  EXPECT_EQ(capture->frames, 2u);
  EXPECT_EQ(capture->head_culled, 3u);
  ASSERT_EQ(capture->draws.size(), 2u);
  EXPECT_EQ(capture->draws[0].frame, 0u);
  EXPECT_EQ(capture->draws[1].frame, 1u);
  ASSERT_EQ(capture->copies.size(), 3u);
  EXPECT_EQ(capture->copies[0].after_index, -1);
  EXPECT_EQ(capture->copies[1].after_index, 0);
  EXPECT_EQ(capture->copies[1].frame, 0u);
  EXPECT_EQ(capture->copies[2].frame, 1u);
  ResetForShutdown();
}

TEST(AgentCapture, FormatsCopies)
{
  CopyRecord copy;
  copy.frame = 2;
  copy.after_index = 745;
  copy.to_texture = true;
  copy.src_right = 512;
  copy.src_bottom = 512;
  copy.width = 512;
  copy.height = 512;
  copy.address = 0x80d3e000;
  copy.format = 6;  // RGBA8
  copy.texture = 0xab73a405d61d3092;
  const std::string row = FormatCopyRow(copy);
  EXPECT_NE(row.find("f2 after #745: EFB 0,0 512x512 -> texture ab73a405d61d3092"),
            std::string::npos)
      << row;
  EXPECT_NE(row.find("RGBA8, texture only at 80d3e000"), std::string::npos) << row;

  copy.after_index = -1;
  copy.xfb = true;
  const std::string xfb = FormatCopyRow(copy);
  EXPECT_NE(xfb.find("before any draw: XFB copy"), std::string::npos) << xfb;
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
  // The camera depth: unknown until the capture reads it.
  EXPECT_NE(row.find("| z - |"), std::string::npos) << row;
  EXPECT_NE(details.find("Camera depth: unknown"), std::string::npos) << details;
  DrawRecord near = draw;
  near.depth_min = 2.5f;
  near.depth_max = 3.0f;
  EXPECT_NE(FormatDrawRow(near).find("| z 2.5..3 |"), std::string::npos) << FormatDrawRow(near);
  EXPECT_NE(FormatDrawDetails(near).find("Camera depth: 2.5 to 3 game units\n"), std::string::npos)
      << FormatDrawDetails(near);
  near.depth_sampled = true;
  EXPECT_NE(FormatDrawRow(near).find("| z 2.5..3~ |"), std::string::npos) << FormatDrawRow(near);
  EXPECT_NE(FormatDrawDetails(near).find("(some vertices of this big draw)"), std::string::npos);

  // Where the draw is: unknown until the capture reads the vertices.
  EXPECT_NE(row.find("| at - |"), std::string::npos) << row;
  DrawRecord placed = draw;
  placed.screen_left = 0.25f;
  placed.screen_top = 0.5f;
  placed.screen_right = 0.75f;
  placed.screen_bottom = 1.0f;
  EXPECT_NE(FormatDrawRow(placed).find("| at scr 0.25-0.75,0.50-1.00 |"), std::string::npos)
      << FormatDrawRow(placed);
  placed.screen_partial = true;
  placed.visible_pixels = 1234;
  EXPECT_NE(FormatDrawRow(placed).find("| at scr 0.25-0.75,0.50-1.00~ vis 1234px |"),
            std::string::npos)
      << FormatDrawRow(placed);
  EXPECT_NE(FormatDrawDetails(placed).find("On the game's screen: x 0.250..0.750, y 0.500..1.000"),
            std::string::npos)
      << FormatDrawDetails(placed);
  EXPECT_NE(FormatDrawDetails(placed).find("Visible: 1234 pixels"), std::string::npos);
  DrawRecord world = MakeDraw(43, true);
  world.view_centroid = {100.0f, 0.0f, -100.0f};
  world.view_min = {90.0f, -5.0f, -110.0f};
  world.view_max = {110.0f, 5.0f, -90.0f};
  world.scale_units_per_meter = 50.0f;
  const std::string world_row = FormatDrawRow(world);
  EXPECT_NE(world_row.find("| at yaw +45 pitch +0 dist 141 (2.8m) |"), std::string::npos)
      << world_row;
  const std::string world_details = FormatDrawDetails(world);
  EXPECT_NE(world_details.find("Relative to the camera: yaw +45.0 deg (+ right), pitch +0.0 deg "
                               "(+ up), distance 141.4 game units (2.83 m at 50 units per meter)"),
            std::string::npos)
      << world_details;
  EXPECT_NE(world_details.find("centre (100, 0, -100), x 90..110 y -5..5 z -110..-90"),
            std::string::npos)
      << world_details;
  // An orthographic draw has no direction, only a screen box.
  DrawRecord flat = placed;
  flat.view_centroid = {0.0f, 0.0f, -1.0f};
  EXPECT_EQ(FormatDrawRow(flat).find("yaw"), std::string::npos);

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

TEST(AgentCapture, DecodesDrawIds)
{
  const u8 draw[4] = {0x34, 0x12, 255, 255};
  EXPECT_EQ(DecodeDrawId(draw), 0x1234u);
  const u8 none[4] = {0x34, 0x12, 0, 0};
  EXPECT_EQ(DecodeDrawId(none), DrawIdMap::NO_DRAW);
}

TEST(AgentCapture, FillsTheDrawIdMapByNearestSampling)
{
  DrawIdMap map;
  map.width = 2;
  map.height = 2;
  map.layers = 2;
  map.ids.assign(8, DrawIdMap::NO_DRAW);
  // A 4x4 texture (the EFB at 2x), its rows 20 bytes apart: texel (x, y) holds index x + 4y,
  // except the last one, which shows no draw.
  std::vector<u8> texels(4 * 20, 0);
  for (u32 y = 0; y < 4; ++y)
  {
    for (u32 x = 0; x < 4; ++x)
    {
      u8* texel = &texels[y * 20 + x * 4];
      texel[0] = static_cast<u8>(x + 4 * y);
      texel[2] = (x == 3 && y == 3) ? 0 : 255;
    }
  }
  FillDrawIdMapLayer(&map, 1, texels.data(), 20, 4, 4);
  EXPECT_EQ(map.Get(1, 0, 0), 0u);
  EXPECT_EQ(map.Get(1, 1, 0), 2u);
  EXPECT_EQ(map.Get(1, 0, 1), 8u);
  EXPECT_EQ(map.Get(1, 1, 1), 10u);
  // The other layer is untouched, and the map's edges hold.
  EXPECT_EQ(map.Get(0, 0, 0), DrawIdMap::NO_DRAW);
  EXPECT_EQ(map.Get(1, 2, 0), DrawIdMap::NO_DRAW);
  EXPECT_EQ(map.Get(2, 0, 0), DrawIdMap::NO_DRAW);

  // At the texture's own size, every texel is read.
  DrawIdMap same;
  same.width = 4;
  same.height = 4;
  same.layers = 1;
  same.ids.assign(16, DrawIdMap::NO_DRAW);
  FillDrawIdMapLayer(&same, 0, texels.data(), 20, 4, 4);
  EXPECT_EQ(same.Get(0, 3, 2), 11u);
  EXPECT_EQ(same.Get(0, 3, 3), DrawIdMap::NO_DRAW);
}

TEST(AgentCapture, MapsImagePointsAndCountsDraws)
{
  // An 8x4 map: draw 7 on the left half, draw 9 on the right, nothing at the bottom right.
  DrawIdMap map;
  map.width = 8;
  map.height = 4;
  map.layers = 1;
  map.ids.assign(32, 7);
  for (u32 y = 0; y < 4; ++y)
  {
    for (u32 x = 4; x < 8; ++x)
      map.ids[y * 8 + x] = 9;
  }
  map.ids[3 * 8 + 7] = DrawIdMap::NO_DRAW;
  // The image shows EFB x 2..6.
  map.xfb_left = 2;
  map.xfb_top = 0;
  map.xfb_right = 6;
  map.xfb_bottom = 4;

  auto pixel = MapImagePointToDrawIdPixel(map, 0.0f, 0.0f);
  ASSERT_TRUE(pixel);
  EXPECT_EQ((*pixel)[0], 2u);
  EXPECT_EQ((*pixel)[1], 0u);
  pixel = MapImagePointToDrawIdPixel(map, 0.5f, 0.5f);
  ASSERT_TRUE(pixel);
  EXPECT_EQ((*pixel)[0], 4u);
  EXPECT_EQ((*pixel)[1], 2u);
  // The far edge stays inside the image.
  pixel = MapImagePointToDrawIdPixel(map, 1.0f, 1.0f);
  ASSERT_TRUE(pixel);
  EXPECT_EQ((*pixel)[0], 5u);
  EXPECT_EQ((*pixel)[1], 3u);
  EXPECT_FALSE(MapImagePointToDrawIdPixel(map, 1.5f, 0.0f));
  EXPECT_FALSE(MapImagePointToDrawIdPixel(DrawIdMap{}, 0.5f, 0.5f));
  // Without an XFB rectangle, the whole map is the image.
  map.xfb_right = 0;
  pixel = MapImagePointToDrawIdPixel(map, 0.5f, 0.5f);
  ASSERT_TRUE(pixel);
  EXPECT_EQ((*pixel)[0], 4u);

  // Around (4, 2) within 1 pixel: x 3..5, y 1..3: three of draw 7, six of draw 9.
  const auto around = CountDrawIdsAround(map, 0, 4, 2, 1);
  ASSERT_EQ(around.size(), 2u);
  EXPECT_EQ(around[0].first, 9u);
  EXPECT_EQ(around[0].second, 6u);
  EXPECT_EQ(around[1].first, 7u);
  EXPECT_EQ(around[1].second, 3u);
  // At the corner the window is clipped, and no-draw pixels are counted too.
  const auto corner = CountDrawIdsAround(map, 0, 7, 3, 1);
  ASSERT_EQ(corner.size(), 2u);
  EXPECT_EQ(corner[0].first, 9u);
  EXPECT_EQ(corner[0].second, 3u);
  EXPECT_EQ(corner[1].first, DrawIdMap::NO_DRAW);
  EXPECT_EQ(corner[1].second, 1u);
  EXPECT_TRUE(CountDrawIdsAround(map, 1, 0, 0, 1).empty());

  CapturedFrame frame;
  frame.id_map = map;
  frame.draws.push_back(MakeDraw(7, true));
  frame.draws.push_back(MakeDraw(9, false));
  frame.draws.push_back(MakeDraw(5, false));
  frame.draws.push_back(MakeDraw(7, true));
  frame.draws.back().frame = 1;
  CountVisiblePixels(&frame);
  EXPECT_EQ(frame.draws[0].visible_pixels, 16);
  EXPECT_EQ(frame.draws[1].visible_pixels, 15);
  EXPECT_EQ(frame.draws[2].visible_pixels, 0);
  EXPECT_EQ(frame.draws[3].visible_pixels, -1);
}

TEST(AgentCapture, CaptureWithIdsWithoutABackendReportsIt)
{
  ResetForShutdown();
  RequestCapture(8, 1, true);
  OnEndFrame(0, false);
  ASSERT_TRUE(IsCapturing());
  OnEndFrame(0, false);
  EXPECT_FALSE(IsCapturing());
  const auto capture = GetLastCapture();
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(capture->id, 8u);
  EXPECT_TRUE(capture->draw_ids);
  EXPECT_FALSE(capture->id_map.HasMap());
  EXPECT_FALSE(capture->id_map_error.empty());
  ResetForShutdown();
}

TEST(AgentCapture, DisplayedFrameRegionFollowsTheXFBCopyOutsideVR)
{
  VR::ResetVRFrameRegion();
  NoteXFBCopyRegion(0, 16, 640, 464);
  VR::VRFrameRegion region = GetDisplayedFrameRegion();
  ASSERT_TRUE(region.valid);
  EXPECT_EQ(region.top, 16);
  EXPECT_EQ(region.height, 448);
  // An empty copy is ignored.
  NoteXFBCopyRegion(0, 0, 0, 0);
  EXPECT_EQ(GetDisplayedFrameRegion().height, 448);
  // In a VR session, the latched VR region wins (committed by the next frame's first copy).
  VR::NotifyVRXFBCopyRegion(0, 0, 640, 480);
  VR::NotifyVRXFBCopyRegion(0, 0, 640, 480);
  region = GetDisplayedFrameRegion();
  ASSERT_TRUE(region.valid);
  EXPECT_EQ(region.height, 480);
  VR::ResetVRFrameRegion();
}
