// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

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
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 448), 0, 0);

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
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 480), 0, 0);

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
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 448), 0, 0);
  VR::ObserveVRPerspectiveViewport(MakeViewport(0, 0, 640, 480), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, SmallPerspectivePaneCannotDrivePresentationCopy)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  VR::ObserveVRPerspectiveViewport(MakeViewport(32, 32, 256, 256), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, MKDDLetterboxedCharacterViewportCannotMoveComposedScreen)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(608, 448);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 100, 608, 348), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 608, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, NearMatchingAnimatedViewportCannotMoveComposedScreen)
{
  VR::ResetVRFrameRegion();
  LatchDisplayRegion(640, 448);

  for (int i = 0; i < 10; ++i)
    VR::ObserveVRPerspectiveViewport(MakeViewport(0, 1, 640, 446), 0, 0);

  const VR::VRFrameRegion xfb_source{0, 0, 640, 448};
  VR::VRFrameRegion presentation_source{};
  EXPECT_FALSE(VR::ConsumeVRPresentationSourceRegion(xfb_source, &presentation_source));
}

TEST(VRFrameRegion, MKDDCharacterBandRemapsToItsOriginalScreenPosition)
{
  // These are the raw values captured in the element signature. GX's 342-pixel scissor
  // offset turns the viewport into a 608x348 pane at (0,100) in the 608x448 frame.
  const Viewport viewport{.wd = 304.0f,
                          .ht = -174.0f,
                          .zRange = 0.0f,
                          .xOrig = 646.0f,
                          .yOrig = 616.0f,
                          .farZ = 0.0f};
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
  const Viewport fzero_portrait{.wd = 64.0f,
                                .ht = -64.0f,
                                .zRange = 0.0f,
                                .xOrig = 918.0f,
                                .yOrig = 758.0f,
                                .farZ = 0.0f};
  const auto portrait_remap = VR::CalculateVRPaneRemap(fzero_portrait, 342, 342, frame);
  EXPECT_NEAR(portrait_remap[0], 128.0f / 640.0f, 0.0001f);
  EXPECT_NEAR(portrait_remap[1], 128.0f / 480.0f, 0.0001f);
  EXPECT_NEAR(portrait_remap[2], 256.0f / 320.0f, 0.0001f);
  EXPECT_NEAR(portrait_remap[3], -176.0f / 240.0f, 0.0001f);

  // Star Fox Adventures uses a full-size viewport shifted right/up; its projection places the
  // inventory within that viewport. The non-zero remap offset must survive the full-frame raster.
  const Viewport starfox_inventory{.wd = 320.0f,
                                   .ht = -240.0f,
                                   .zRange = 0.0f,
                                   .xOrig = 906.0f,
                                   .yOrig = 537.0f,
                                   .farZ = 0.0f};
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
