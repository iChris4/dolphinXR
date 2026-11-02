// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "VideoCommon/VR/VRFrameRegion.h"
#include "VideoCommon/XFMemory.h"

namespace
{
Viewport MakeViewport(int left, int top, int width, int height)
{
  return {.wd = width * 0.5f,
          .ht = height * -0.5f,
          .zRange = 0.0f,
          .xOrig = left + width * 0.5f,
          .yOrig = top + height * 0.5f,
          .farZ = 0.0f};
}

// A scissor covering this rectangle (bottom-right corner inclusive, like bpmem).
VR::VRScissor ScissorOf(int left, int top, int width, int height)
{
  return {left, top, left + width - 1, top + height - 1};
}

void LatchDisplayRegion(int width, int height)
{
  VR::NotifyVRXFBCopyRegion(0, 0, width, height);
  VR::NotifyVRXFBCopyRegion(0, 0, width, height);
}
}  // namespace

TEST(VRFrameRegion, TrilogyPresentationExpandsAfterEFBComposition)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 480);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 448), ScissorOf(0, 0, 640, 448), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 480};
  VR::VRFrameRegion presentation_source{};
  EXPECT_TRUE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
  EXPECT_EQ(presentation_source.left, 0);
  EXPECT_EQ(presentation_source.top, 0);
  EXPECT_EQ(presentation_source.width, 640);
  EXPECT_EQ(presentation_source.height, 448);
}

TEST(VRFrameRegion, GolfPresentationIncludesWholeOversizedScene)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 480), ScissorOf(0, 0, 640, 480), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_TRUE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
  EXPECT_EQ(presentation_source.left, 0);
  EXPECT_EQ(presentation_source.top, 0);
  EXPECT_EQ(presentation_source.width, 640);
  EXPECT_EQ(presentation_source.height, 480);
}

TEST(VRFrameRegion, DominantSceneWinsAndMatchingCopyNeedsNoOverride)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 448), ScissorOf(0, 0, 640, 448), 0, 0);
  VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 480), ScissorOf(0, 0, 640, 480), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, SmallPerspectivePaneCannotDrivePresentationCopy)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  VR::ObserveVRPerspectiveViewport(MakeViewport(32, 32, 256, 256), ScissorOf(32, 32, 256, 256), 0,
                                   0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, MKDDLetterboxedCharacterViewportCannotMoveComposedScreen)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(608, 448);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 100, 608, 348), ScissorOf(0, 100, 608, 348), 0,
                                     0);

  const VR::VRFrameRegion xfb_source{0, 0, 608, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, NearMatchingAnimatedViewportCannotMoveComposedScreen)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 1, 640, 446), ScissorOf(0, 1, 640, 446), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, SceneMovedFarOffTheFrameIsDisplaced)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 480);

  // Phantasy Star Online pans its scene 80 px right and 112 px up under its menus.
  EXPECT_EQ(
      VR::ClassifyVRViewport(MakeViewport(80, -112, 640, 480), ScissorOf(80, -112, 640, 480), 0, 0),
      VR::VRViewportClass::DisplacedScene);
  // Moved up only: it used to stay MainScene and be drawn shifted.
  EXPECT_EQ(
      VR::ClassifyVRViewport(MakeViewport(0, -100, 640, 480), ScissorOf(0, -100, 640, 480), 0, 0),
      VR::VRViewportClass::DisplacedScene);
  // Also through the scissor offset alone, as PSO does.
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(342, 342, 640, 480), ScissorOf(342, 342, 640, 480),
                                   262, 454),
            VR::VRViewportClass::DisplacedScene);
}

TEST(VRFrameRegion, SceneMovedWithinTheToleranceStaysMainScene)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 480);

  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(0, 0, 640, 480), ScissorOf(0, 0, 640, 480), 0, 0),
            VR::VRViewportClass::MainScene);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(2, 0, 640, 480), ScissorOf(2, 0, 640, 480), 0, 0),
            VR::VRViewportClass::MainScene);
  EXPECT_EQ(
      VR::ClassifyVRViewport(MakeViewport(-1, -2, 640, 480), ScissorOf(-1, -2, 640, 480), 0, 0),
      VR::VRViewportClass::MainScene);
  EXPECT_EQ(
      VR::ClassifyVRViewport(MakeViewport(60, 40, 640, 480), ScissorOf(60, 40, 640, 480), 0, 0),
      VR::VRViewportClass::MainScene);
}

TEST(VRFrameRegion, ViewportMovedInsideAFixedScissorIsNotDisplaced)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  // Mario Golf places each main menu icon with a 640x448 viewport of its own, inside the
  // unmoved full-frame scissor (raw values from a log, with GX's 342-pixel offsets).
  const VR::VRScissor full_frame = ScissorOf(342, 342, 640, 480);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(484, 369, 640, 448), full_frame, 342, 342),
            VR::VRViewportClass::Offscreen);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(177, 238, 640, 448), full_frame, 342, 342),
            VR::VRViewportClass::MainScene);
}

TEST(VRFrameRegion, SceneOutsideTheFrameStaysOffscreen)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 480);

  EXPECT_EQ(
      VR::ClassifyVRViewport(MakeViewport(700, 0, 640, 480), ScissorOf(700, 0, 640, 480), 0, 0),
      VR::VRViewportClass::Offscreen);
}

TEST(VRFrameRegion, MKDDCharacterBandRemapsToItsOriginalScreenPosition)
{
  // These are the raw values captured in the element signature. GX's 342-pixel scissor
  // offset turns the viewport into a 608x348 pane at (0,100) in the 608x448 frame.
  const Viewport viewport{
      .wd = 304.0f, .ht = -174.0f, .zRange = 0.0f, .xOrig = 646.0f, .yOrig = 616.0f, .farZ = 0.0f};
  const VR::VRFrameRegion frame{0, 0, 608, 448, true};

  const auto remap = VR::CalculateVRPaneRemap(viewport, 342, 342, frame);
  EXPECT_FLOAT_EQ(remap[0], 1.0f);
  EXPECT_NEAR(remap[1], 348.0f / 448.0f, 0.0001f);
  EXPECT_FLOAT_EQ(remap[2], 0.0f);
  EXPECT_NEAR(remap[3], -100.0f / 448.0f, 0.0001f);
}

TEST(VRFrameRegion, OffCenterScreenPanesKeepTheirViewportOffsets)
{
  const VR::VRFrameRegion frame{0, 0, 640, 480, true};

  // F-Zero's 128x128 race portrait pane is anchored to the lower-right EFB corner.
  const Viewport fzero_portrait{
      .wd = 64.0f, .ht = -64.0f, .zRange = 0.0f, .xOrig = 918.0f, .yOrig = 758.0f, .farZ = 0.0f};
  const auto portrait_remap = VR::CalculateVRPaneRemap(fzero_portrait, 342, 342, frame);
  EXPECT_NEAR(portrait_remap[0], 128.0f / 640.0f, 0.0001f);
  EXPECT_NEAR(portrait_remap[1], 128.0f / 480.0f, 0.0001f);
  EXPECT_NEAR(portrait_remap[2], 256.0f / 320.0f, 0.0001f);
  EXPECT_NEAR(portrait_remap[3], -176.0f / 240.0f, 0.0001f);

  // Star Fox Adventures uses a full-size viewport shifted right/up; its projection places the
  // inventory within that viewport. The non-zero remap offset must survive the full-frame raster.
  const Viewport starfox_inventory{
      .wd = 320.0f, .ht = -240.0f, .zRange = 0.0f, .xOrig = 906.0f, .yOrig = 537.0f, .farZ = 0.0f};
  const auto inventory_remap = VR::CalculateVRPaneRemap(starfox_inventory, 342, 342, frame);
  EXPECT_FLOAT_EQ(inventory_remap[0], 1.0f);
  EXPECT_FLOAT_EQ(inventory_remap[1], 1.0f);
  EXPECT_NEAR(inventory_remap[2], 244.0f / 320.0f, 0.0001f);
  EXPECT_NEAR(inventory_remap[3], 45.0f / 240.0f, 0.0001f);
}

TEST(VRFrameRegion, ScreenPaneClipIsTheScissorInFrameNDC)
{
  const VR::VRFrameRegion frame{0, 0, 640, 480, true};

  // A 127x127 minimap window at (100,50), with GX's 342-pixel scissor offset. The bottom-right
  // scissor corner is inclusive.
  const auto clip =
      VR::CalculateVRPaneClip(342 + 100, 342 + 50, 342 + 226, 342 + 176, 342, 342, frame);
  EXPECT_FLOAT_EQ(clip[0], (100.0f - 320.0f) / 320.0f);
  EXPECT_FLOAT_EQ(clip[1], (227.0f - 320.0f) / 320.0f);
  EXPECT_FLOAT_EQ(clip[2], -(177.0f - 240.0f) / 240.0f);
  EXPECT_FLOAT_EQ(clip[3], -(50.0f - 240.0f) / 240.0f);
}

TEST(VRFrameRegion, ScreenPaneClipLeavesEdgesAtOrBeyondTheFrameOpen)
{
  const VR::VRFrameRegion frame{0, 16, 640, 448, true};

  const auto full = VR::CalculateVRPaneClip(342, 342 + 16, 342 + 639, 342 + 463, 342, 342, frame);
  EXPECT_FLOAT_EQ(full[0], -VR::VR_PANE_CLIP_OPEN);
  EXPECT_FLOAT_EQ(full[1], VR::VR_PANE_CLIP_OPEN);
  EXPECT_FLOAT_EQ(full[2], -VR::VR_PANE_CLIP_OPEN);
  EXPECT_FLOAT_EQ(full[3], VR::VR_PANE_CLIP_OPEN);

  // Only the right edge is inside the frame.
  const auto right_only = VR::CalculateVRPaneClip(0, 0, 342 + 319, 1023, 342, 342, frame);
  EXPECT_FLOAT_EQ(right_only[0], -VR::VR_PANE_CLIP_OPEN);
  EXPECT_FLOAT_EQ(right_only[1], 0.0f);
  EXPECT_FLOAT_EQ(right_only[2], -VR::VR_PANE_CLIP_OPEN);
  EXPECT_FLOAT_EQ(right_only[3], VR::VR_PANE_CLIP_OPEN);

  const auto no_frame = VR::CalculateVRPaneClip(342 + 100, 342 + 50, 342 + 226, 342 + 176, 342, 342,
                                                VR::VRFrameRegion{});
  EXPECT_FLOAT_EQ(no_frame[0], -VR::VR_PANE_CLIP_OPEN);
  EXPECT_FLOAT_EQ(no_frame[1], VR::VR_PANE_CLIP_OPEN);
}

TEST(VRFrameRegion, ScreenPaneReferenceUsesProjectionForPerVertexMatrixDraws)
{
  EXPECT_FLOAT_EQ(VR::CalculateVRPaneReferenceW(-25.0f, 0.5f, 10.0f), 25.0f);

  // F-Zero: near=0.1, far=90000. Per-vertex matrices leave the shared model origin at zero.
  const float fzero_far = 90000.0f;
  const float fzero_near = 0.1f;
  const float fzero_projection_z = fzero_near / (fzero_near - fzero_far);
  const float fzero_projection_w = fzero_far * fzero_projection_z;
  EXPECT_NEAR(VR::CalculateVRPaneReferenceW(0.0f, fzero_projection_z, fzero_projection_w),
              std::sqrt(fzero_near * fzero_far), 0.001f);

  // Star Fox Adventures: near=2.5, far=10000.
  const float starfox_far = 10000.0f;
  const float starfox_near = 2.5f;
  const float starfox_projection_z = starfox_near / (starfox_near - starfox_far);
  const float starfox_projection_w = starfox_far * starfox_projection_z;
  EXPECT_NEAR(VR::CalculateVRPaneReferenceW(0.0f, starfox_projection_z, starfox_projection_w),
              std::sqrt(starfox_near * starfox_far), 0.001f);
}

namespace
{
float TanDegrees(float degrees)
{
  return std::tan(degrees * 3.14159265f / 180.0f);
}

// Metroid Prime 2's world projection (hfov 73.27, vfov 55) as raw XF values.
std::array<float, 6> Prime2WorldProjection()
{
  return {
      1.0f / TanDegrees(73.27f / 2.0f), 0.0f, 1.0f / TanDegrees(55.0f / 2.0f), 0.0f, -1.0f, -0.2f};
}

// A left eye that sees farther to the left and down than to the right and up, like the PSVR2's.
VR::VREyeTangents AsymmetricLeftEye()
{
  return {.left = -TanDegrees(52.0f),
          .right = TanDegrees(42.0f),
          .up = TanDegrees(48.0f),
          .down = -TanDegrees(52.0f)};
}
}  // namespace

TEST(VRFrameRegion, GazeMappingGoesThroughBothProjections)
{
  const std::array<float, 6> game = Prime2WorldProjection();

  // An eye with the game's own FOV sees the game's screen unchanged.
  const VR::VREyeTangents same{.left = -TanDegrees(73.27f / 2.0f),
                               .right = TanDegrees(73.27f / 2.0f),
                               .up = TanDegrees(27.5f),
                               .down = -TanDegrees(27.5f)};
  const auto unchanged = VR::MapGameNDCToEyeNDC(game, same, 0.3f, -0.2f);
  EXPECT_NEAR(unchanged[0], 0.3f, 1e-5f);
  EXPECT_NEAR(unchanged[1], -0.2f, 1e-5f);

  // Straight ahead is right of and above the asymmetric eye's image centre.
  const auto center = VR::MapGameNDCToEyeNDC(game, AsymmetricLeftEye(), 0.0f, 0.0f);
  EXPECT_NEAR(center[0], 0.17407f, 1e-4f);
  EXPECT_NEAR(center[1], 0.07083f, 1e-4f);
  const auto off_center = VR::MapGameNDCToEyeNDC(game, AsymmetricLeftEye(), 0.5f, 0.25f);
  EXPECT_NEAR(off_center[0], 0.51513f, 1e-4f);
  EXPECT_NEAR(off_center[1], 0.17971f, 1e-4f);
}

// Echoes copies a 32x64 block at the centre of its 640x448 scene to find the object under the
// scan reticle; in the left eye's image that direction is 56 pixels right and 16 up.
TEST(VRFrameRegion, GazeCopyRectangleMovesToStraightAhead)
{
  const VR::VRFrameRegion frame{.left = 0, .top = 0, .width = 640, .height = 448, .valid = true};
  const MathUtil::Rectangle<int> block{304, 192, 336, 256};

  EXPECT_EQ(VR::MapVRGazeCopyRectangle(block, frame, Prime2WorldProjection(), AsymmetricLeftEye()),
            (MathUtil::Rectangle<int>{360, 176, 392, 240}));

  // It stays inside the frame.
  const VR::VREyeTangents far_left{.left = -TanDegrees(80.0f),
                                   .right = TanDegrees(5.0f),
                                   .up = TanDegrees(45.0f),
                                   .down = -TanDegrees(45.0f)};
  const MathUtil::Rectangle<int> clamped =
      VR::MapVRGazeCopyRectangle(block, frame, Prime2WorldProjection(), far_left);
  EXPECT_EQ(clamped.right, 640);
  EXPECT_EQ(clamped.GetWidth(), 32);

  // Unknown frame: unchanged.
  EXPECT_EQ(VR::MapVRGazeCopyRectangle(block, VR::VRFrameRegion{}, Prime2WorldProjection(),
                                       AsymmetricLeftEye()),
            block);
}

namespace
{
// OpenXRManager::GetEyeProjectionRows for an eye at the game camera, turned left by yaw degrees.
VR::VREyeViewRows TurnedEyeRows(const VR::VREyeTangents& eye, float yaw_degrees)
{
  const float c = std::cos(yaw_degrees * 3.14159265f / 180.0f);
  const float s = std::sin(yaw_degrees * 3.14159265f / 180.0f);
  // The eye's axes in view space, as columns: a rotation about +Y.
  const float m[3][3] = {{c, 0.0f, s}, {0.0f, 1.0f, 0.0f}, {-s, 0.0f, c}};
  const float p0x = 2.0f / (eye.right - eye.left);
  const float p0z = (eye.right + eye.left) / (eye.right - eye.left);
  const float p1y = 2.0f / (eye.up - eye.down);
  const float p1z = (eye.up + eye.down) / (eye.up - eye.down);
  return {.x = {m[0][0] * p0x + m[0][2] * p0z, m[1][0] * p0x + m[1][2] * p0z,
                m[2][0] * p0x + m[2][2] * p0z, 0.0f},
          .y = {m[0][1] * p1y + m[0][2] * p1z, m[1][1] * p1y + m[1][2] * p1z,
                m[2][1] * p1y + m[2][2] * p1z, 0.0f},
          .z = {m[0][2], m[1][2], m[2][2], 0.0f}};
}
}  // namespace

TEST(VRFrameRegion, ViewMappingFollowsTheHeadTurn)
{
  const std::array<float, 6> game = Prime2WorldProjection();
  const VR::VREyeTangents eye = AsymmetricLeftEye();

  // Looking along the game camera, the eye's view is the tangent mapping.
  const auto straight = VR::MapGameNDCToEyeViewNDC(game, TurnedEyeRows(eye, 0.0f), 0.5f, 0.25f);
  const auto gaze = VR::MapGameNDCToEyeNDC(game, eye, 0.5f, 0.25f);
  EXPECT_NEAR(straight[0], gaze[0], 1e-5f);
  EXPECT_NEAR(straight[1], gaze[1], 1e-5f);

  // With the head turned 10 degrees left, the game's screen centre is 10 degrees right of the
  // eye's straight ahead.
  const auto turned = VR::MapGameNDCToEyeViewNDC(game, TurnedEyeRows(eye, 10.0f), 0.0f, 0.0f);
  EXPECT_NEAR(turned[0], 2.0f * (TanDegrees(10.0f) - eye.left) / (eye.right - eye.left) - 1.0f,
              1e-4f);

  // Behind the eye: no place in its image, and a copy rectangle stays where it is.
  EXPECT_TRUE(std::isnan(VR::MapGameNDCToEyeViewNDC(game, TurnedEyeRows(eye, 180.0f), 0, 0)[0]));
  const VR::VRFrameRegion frame{.left = 0, .top = 0, .width = 640, .height = 448, .valid = true};
  const MathUtil::Rectangle<int> block{270, 204, 302, 236};
  EXPECT_EQ(VR::MapVRViewCopyRectangle(block, frame, game, TurnedEyeRows(eye, 180.0f)), block);

  // Without a head turn the view mapping moves a block like the gaze mapping.
  EXPECT_EQ(VR::MapVRViewCopyRectangle(block, frame, game, TurnedEyeRows(eye, 0.0f)),
            VR::MapVRGazeCopyRectangle(block, frame, game, eye));
}

// The blocks Metroid Prime 2 and 3 read back under their reticles, and the copies that must stay
// where they are.
TEST(VRFrameRegion, ReticleCopyRectangles)
{
  const VR::VRFrameRegion frame{.left = 0, .top = 0, .width = 640, .height = 448, .valid = true};
  // Echoes: the scan block and the scan box's view, centred.
  EXPECT_TRUE(VR::IsVRReticleCopyRectangle({304, 192, 336, 256}, frame));
  EXPECT_TRUE(VR::IsVRReticleCopyRectangle({216, 178, 424, 270}, frame));
  // Corruption: a block under the pointer.
  EXPECT_TRUE(VR::IsVRReticleCopyRectangle({270, 204, 302, 236}, frame));
  EXPECT_TRUE(VR::IsVRReticleCopyRectangle({382, 350, 414, 382}, frame));
  // A quarter of the scene, render-to-texture passes and downscaled buffers at the edges, a
  // centred copy bigger than half the frame.
  EXPECT_FALSE(VR::IsVRReticleCopyRectangle({0, 0, 320, 224}, frame));
  EXPECT_FALSE(VR::IsVRReticleCopyRectangle({0, 384, 64, 448}, frame));
  EXPECT_FALSE(VR::IsVRReticleCopyRectangle({0, 416, 32, 448}, frame));
  EXPECT_FALSE(VR::IsVRReticleCopyRectangle({0, 0, 160, 112}, frame));
  EXPECT_FALSE(VR::IsVRReticleCopyRectangle({114, 86, 522, 372}, frame));
  EXPECT_FALSE(VR::IsVRReticleCopyRectangle({270, 204, 302, 236}, VR::VRFrameRegion{}));

  // Centred: Echoes' scan blocks, the Trilogy's scan box views (Prime, Echoes); not the blocks
  // under the Trilogy's or Corruption's pointer, nor Echoes' sun glare test (a block around the
  // sun, restored in place by the game; GameCube, Trilogy).
  EXPECT_TRUE(VR::IsVRCentredCopyRectangle({304, 192, 336, 256}, frame));
  EXPECT_TRUE(VR::IsVRCentredCopyRectangle({216, 178, 424, 270}, frame));
  EXPECT_TRUE(VR::IsVRCentredCopyRectangle({266, 176, 374, 272}, frame));
  EXPECT_TRUE(VR::IsVRCentredCopyRectangle({276, 184, 364, 264}, frame));
  EXPECT_FALSE(VR::IsVRCentredCopyRectangle({350, 206, 382, 270}, frame));
  EXPECT_FALSE(VR::IsVRCentredCopyRectangle({270, 204, 302, 236}, frame));
  EXPECT_FALSE(VR::IsVRCentredCopyRectangle({390, 76, 454, 140}, frame));
  EXPECT_TRUE(VR::IsVRReticleCopyRectangle({390, 76, 454, 140}, frame));
  EXPECT_FALSE(VR::IsVRCentredCopyRectangle({326, 92, 390, 156}, frame));
  EXPECT_TRUE(VR::IsVRReticleCopyRectangle({326, 92, 390, 156}, frame));

  // Blocks drawn back in place (PerEyeScreenCopies): Metroid Prime's charged shot warp (192x192,
  // also cut at the frame's edge), Echoes' sun glare test; not a quarter-size effect buffer nor a
  // copy wider than a third of the frame.
  EXPECT_TRUE(VR::IsVRPerEyeCopyRectangle({232, 140, 424, 332}, frame));
  EXPECT_TRUE(VR::IsVRPerEyeCopyRectangle({0, 140, 100, 332}, frame));
  EXPECT_TRUE(VR::IsVRPerEyeCopyRectangle({390, 76, 454, 140}, frame));
  EXPECT_FALSE(VR::IsVRPerEyeCopyRectangle({0, 0, 320, 224}, frame));
  EXPECT_FALSE(VR::IsVRPerEyeCopyRectangle({200, 140, 440, 332}, frame));
  EXPECT_FALSE(VR::IsVRPerEyeCopyRectangle({232, 140, 232, 332}, frame));
  EXPECT_FALSE(VR::IsVRPerEyeCopyRectangle({232, 140, 424, 332}, VR::VRFrameRegion{}));
  EXPECT_FALSE(VR::IsVRCentredCopyRectangle({114, 86, 522, 372}, frame));
  EXPECT_FALSE(VR::IsVRCentredCopyRectangle({304, 192, 336, 256}, VR::VRFrameRegion{}));
}

// The Mii library composes faces with an orthographic projection over the whole frame and a
// scissor clipping it to a small box at the EFB's corner (Wii Sports: 64x64 in a 608x448
// viewport), then copies the box: a render-to-texture pass. The same scissor elsewhere, or one
// as large as the viewport, is not.
TEST(VRFrameRegion, SmallScissorAtTheCornerIsRenderToTexture)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 456);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(0, 0, 608, 448), ScissorOf(0, 0, 64, 64), 0, 0),
            VR::VRViewportClass::RenderToTexture);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(0, 0, 608, 448), ScissorOf(0, 0, 8, 8), 0, 0),
            VR::VRViewportClass::RenderToTexture);
  // With the EFB origin offset (342) the game's raw coordinates.
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(342, 342, 608, 448), ScissorOf(342, 342, 64, 64),
                                   342, 342),
            VR::VRViewportClass::RenderToTexture);
  EXPECT_NE(VR::ClassifyVRViewport(MakeViewport(0, 0, 608, 448), ScissorOf(40, 40, 64, 64), 0, 0),
            VR::VRViewportClass::RenderToTexture);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(0, 0, 640, 456), ScissorOf(0, 0, 640, 456), 0, 0),
            VR::VRViewportClass::MainScene);
  EXPECT_NE(VR::ClassifyVRViewport(MakeViewport(0, 0, 300, 200), ScissorOf(0, 0, 300, 200), 0, 0),
            VR::VRViewportClass::RenderToTexture);
}

// Wii Sports' Boxing draws each boxer of its "VS" screen in a 350x456 pane at the EFB's corner,
// copies the pane to a texture and composes both copies on the screen: the pane is a render
// target from the frame after the copy, not the left half of a split screen.
TEST(VRFrameRegion, PaneCopiedToATextureIsRenderToTexture)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 456);
  const Viewport pane = MakeViewport(342, 342, 350, 456);
  const VR::VRScissor pane_scissor = ScissorOf(342, 342, 350, 456);
  EXPECT_EQ(VR::ClassifyVRViewport(pane, pane_scissor, 342, 342), VR::VRViewportClass::SplitScreen);

  // The copies count once the frame is complete (its XFB copy).
  VR::NotifyVRTextureCopyRegion(0, 0, 350, 456);
  VR::NotifyVRTextureCopyRegion(0, 0, 175, 228);
  EXPECT_EQ(VR::ClassifyVRViewport(pane, pane_scissor, 342, 342), VR::VRViewportClass::SplitScreen);
  VR::NotifyVRXFBCopyRegion(0, 0, 640, 456);
  EXPECT_EQ(VR::ClassifyVRViewport(pane, pane_scissor, 342, 342),
            VR::VRViewportClass::RenderToTexture);
  // Another pane, the whole frame and a letterboxed scene copied whole stay what they are.
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(342, 342, 320, 456), ScissorOf(342, 342, 320, 456),
                                   342, 342),
            VR::VRViewportClass::SplitScreen);
  VR::NotifyVRTextureCopyRegion(0, 0, 640, 456);
  VR::NotifyVRTextureCopyRegion(0, 0, 640, 400);
  VR::NotifyVRXFBCopyRegion(0, 0, 640, 456);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(342, 342, 640, 456), ScissorOf(342, 342, 640, 456),
                                   342, 342),
            VR::VRViewportClass::MainScene);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(342, 342, 640, 400), ScissorOf(342, 342, 640, 400),
                                   342, 342),
            VR::VRViewportClass::Letterboxed);
  // The pane was not copied in that frame: a split-screen half again.
  EXPECT_EQ(VR::ClassifyVRViewport(pane, pane_scissor, 342, 342), VR::VRViewportClass::SplitScreen);
  // A copied pane away from the corner is not taken for a render target.
  VR::NotifyVRTextureCopyRegion(320, 0, 640, 456);
  VR::NotifyVRXFBCopyRegion(0, 0, 640, 456);
  EXPECT_EQ(VR::ClassifyVRViewport(MakeViewport(662, 342, 320, 456), ScissorOf(662, 342, 320, 456),
                                   342, 342),
            VR::VRViewportClass::SplitScreen);
}
