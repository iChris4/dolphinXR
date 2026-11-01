// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "VideoCommon/VR/VRPanelHandle.h"
#include "VideoCommon/VR/VRPanelMath.h"

using namespace VR;

namespace
{
constexpr float PI = std::numbers::pi_v<float>;

void ExpectNear(const XrVector3f& a, const XrVector3f& b, float tolerance = 1e-4f)
{
  EXPECT_NEAR(a.x, b.x, tolerance);
  EXPECT_NEAR(a.y, b.y, tolerance);
  EXPECT_NEAR(a.z, b.z, tolerance);
}

float Distance(const XrVector3f& a, const XrVector3f& b)
{
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                   (a.z - b.z) * (a.z - b.z));
}

XrVector3f PointOnPanel(const XrPosef& panel, float x, float y)
{
  const XrVector3f offset = PanelMath::RotateVector(panel.orientation, {x, y, 0.0f});
  return {panel.position.x + offset.x, panel.position.y + offset.y, panel.position.z + offset.z};
}

XrVector3f RayEnd(const XrPosef& aim, float length)
{
  const XrVector3f direction = PanelMath::RotateVector(aim.orientation, {0.0f, 0.0f, -1.0f});
  return {aim.position.x + direction.x * length, aim.position.y + direction.y * length,
          aim.position.z + direction.z * length};
}

// An aim pose at `origin` pointing at `target` (no roll).
XrPosef AimAt(const XrVector3f& origin, const XrVector3f& target)
{
  // A panel heading toward the origin from the target faces back along the ray: its +Z is -aim.
  const std::optional<PanelMath::Heading> back = PanelMath::HeadingToward(target, origin);
  return {PanelMath::HeadingOrientation(*back), origin};
}

// The default menu: 1.2 m x 0.9 m, 1.25 m ahead of the head, 0.12 m below the eyes.
const XrVector3f HEAD{0.0f, 1.6f, 0.0f};
const XrPosef PANEL{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.48f, -1.25f}};
const XrExtent2Df SIZE{1.2f, 0.9f};
}  // namespace

TEST(VRPanelMath, HeadingsHaveNoRoll)
{
  for (const float yaw : {0.0f, 0.7f, -2.5f, PI * 0.5f})
  {
    for (const float pitch : {0.0f, 0.4f, -0.9f})
    {
      const XrQuaternionf q = PanelMath::HeadingOrientation({yaw, pitch});
      EXPECT_NEAR(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0f, 1e-5f);
      ExpectNear(
          PanelMath::RotateVector(q, {0.0f, 0.0f, 1.0f}),
          {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)});
      // The panel's right edge stays level.
      EXPECT_NEAR(PanelMath::RotateVector(q, {1.0f, 0.0f, 0.0f}).y, 0.0f, 1e-5f);
      const PanelMath::Heading back = PanelMath::HeadingOf(q);
      EXPECT_NEAR(back.yaw, yaw, 1e-4f);
      EXPECT_NEAR(back.pitch, pitch, 1e-4f);
    }
  }
  // A yaw-only heading is PoseInFront's orientation.
  const XrQuaternionf in_front =
      PanelMath::PoseInFront({{0.0f, 0.0f, 0.0f}, 0.8f}, 1.0f).orientation;
  const XrQuaternionf heading = PanelMath::HeadingOrientation({0.8f, 0.0f});
  EXPECT_NEAR(in_front.y, heading.y, 1e-6f);
  EXPECT_NEAR(in_front.w, heading.w, 1e-6f);
}

TEST(VRPanelMath, HeadingTowardAPoint)
{
  // From a panel 1 m ahead and 1 m below the head: facing back (+Z), leaning back 45 degrees.
  const std::optional<PanelMath::Heading> heading =
      PanelMath::HeadingToward({0.0f, 0.6f, -1.0f}, HEAD);
  ASSERT_TRUE(heading);
  EXPECT_NEAR(heading->yaw, 0.0f, 1e-5f);
  EXPECT_NEAR(heading->pitch, PI / 4.0f, 1e-5f);
  // From the head's right: facing left (-X).
  EXPECT_NEAR(PanelMath::HeadingToward({2.0f, 1.6f, 0.0f}, HEAD)->yaw, -PI / 2.0f, 1e-5f);
  // Straight below the head the yaw is undefined.
  EXPECT_FALSE(PanelMath::HeadingToward({0.0f, 0.0f, 0.0f}, HEAD));
}

TEST(VRPanelMath, PlacementFollowsTheHead)
{
  const PanelMath::YawAnchor first{{0.2f, 1.6f, 0.1f}, 0.3f};
  const XrPosef placed{PanelMath::HeadingOrientation({0.5f, 0.2f}), {-0.6f, 1.2f, -0.9f}};
  const PanelMath::AnchoredPose anchored =
      PanelMath::ToAnchorFrame(first, placed.position, PanelMath::HeadingOf(placed.orientation));
  // Under the same anchor it comes back where it was.
  const XrPosef same = PanelMath::FromAnchorFrame(first, anchored);
  ExpectNear(same.position, placed.position);
  EXPECT_NEAR(PanelMath::HeadingOf(same.orientation).yaw, 0.5f, 1e-4f);

  // The head turned half a turn and moved: it is at the same place relative to the head.
  const PanelMath::YawAnchor second{{1.0f, 1.5f, -2.0f}, 0.3f + PI};
  const XrPosef moved = PanelMath::FromAnchorFrame(second, anchored);
  EXPECT_NEAR(Distance(moved.position, second.center), Distance(placed.position, first.center),
              1e-4f);
  EXPECT_NEAR(moved.position.y - second.center.y, placed.position.y - first.center.y, 1e-5f);
  const PanelMath::Heading turned = PanelMath::HeadingOf(moved.orientation);
  EXPECT_NEAR(std::remainder(turned.yaw - (0.5f + PI), 2.0f * PI), 0.0f, 1e-4f);
  EXPECT_NEAR(turned.pitch, 0.2f, 1e-4f);
}

TEST(VRPanelHandle, BarSitsUnderThePanel)
{
  const XrVector3f center = PanelHandle::GetBarCenter(SIZE);
  EXPECT_LT(center.y, -0.45f);
  EXPECT_FLOAT_EQ(center.x, 0.0f);

  EXPECT_TRUE(PanelHandle::IsOnBar({0.0f, center.y, 1.0f}, SIZE));
  // Near it, a laser still grabs it.
  EXPECT_TRUE(PanelHandle::IsOnBar({0.05f, center.y + 0.025f, 1.0f}, SIZE));
  // The panel's bottom edge and the sides are not the bar.
  EXPECT_FALSE(PanelHandle::IsOnBar({0.0f, -0.45f, 1.0f}, SIZE));
  EXPECT_FALSE(PanelHandle::IsOnBar({0.3f, center.y, 1.0f}, SIZE));
  EXPECT_FALSE(PanelHandle::IsOnBar({0.0f, center.y - 0.1f, 1.0f}, SIZE));
}

TEST(VRPanelHandle, LayerLiesLevelUnderThePanel)
{
  const XrRect2Di texture = PanelHandle::GetTextureRect(1472);
  EXPECT_EQ(texture.offset.x, 1472);
  EXPECT_EQ(texture.extent.width, static_cast<int32_t>(PanelHandle::TEXTURE_WIDTH));
  EXPECT_EQ(texture.extent.height, static_cast<int32_t>(PanelHandle::TEXTURE_HEIGHT));

  const XrPosef panel{PanelMath::HeadingOrientation({0.6f, 0.0f}), {-0.7f, 1.48f, -1.0f}};
  const XrCompositionLayerQuad layer = PanelHandle::MakeLayer(
      XR_NULL_HANDLE, XR_NULL_HANDLE, texture, panel, SIZE, PanelHandle::State::Idle);
  ExpectNear(layer.pose.position, PointOnPanel(panel, 0.0f, PanelHandle::GetBarCenter(SIZE).y));
  // The texture's long side (the layer's +Y) runs along the panel's width.
  const XrVector3f along = PanelMath::RotateVector(layer.pose.orientation, {0.0f, 1.0f, 0.0f});
  const XrVector3f panel_x = PanelMath::RotateVector(panel.orientation, {1.0f, 0.0f, 0.0f});
  EXPECT_NEAR(std::abs(along.x * panel_x.x + along.y * panel_x.y + along.z * panel_x.z), 1.0f,
              1e-5f);
  // In the panel's plane.
  const XrVector3f normal = PanelMath::RotateVector(layer.pose.orientation, {0.0f, 0.0f, 1.0f});
  ExpectNear(normal, PanelMath::RotateVector(panel.orientation, {0.0f, 0.0f, 1.0f}));
  // The visible capsule (the texture less its padding) is the bar.
  const float pixel = layer.size.width / PanelHandle::TEXTURE_WIDTH;
  EXPECT_NEAR(pixel * (PanelHandle::TEXTURE_WIDTH - 2.0f * PanelHandle::TEXTURE_PADDING),
              PanelHandle::BAR_THICKNESS * SIZE.width, 1e-5f);
  EXPECT_NEAR(pixel * (PanelHandle::TEXTURE_HEIGHT - 2.0f * PanelHandle::TEXTURE_PADDING),
              PanelHandle::BAR_LENGTH * SIZE.width, 1e-5f);

  // It grows while pointed at.
  const XrCompositionLayerQuad hovered = PanelHandle::MakeLayer(
      XR_NULL_HANDLE, XR_NULL_HANDLE, texture, panel, SIZE, PanelHandle::State::Hovered);
  EXPECT_GT(hovered.size.height, layer.size.height);
}

TEST(VRPanelHandle, GrabDoesNotMoveThePanel)
{
  const XrVector3f bar = PointOnPanel(PANEL, 0.03f, PanelHandle::GetBarCenter(SIZE).y);
  const XrPosef aim = AimAt({0.25f, 1.1f, -0.35f}, bar);
  const std::optional<PanelMath::QuadHit> hit = PanelMath::IntersectRayQuad(aim, PANEL);
  ASSERT_TRUE(hit);
  ASSERT_TRUE(PanelHandle::IsOnBar(*hit, SIZE));

  PanelHandle::Drag drag(aim, *hit, PANEL, SIZE, HEAD);
  EXPECT_NEAR(drag.GetRayLength(), Distance(aim.position, bar), 1e-4f);
  drag.Update(aim, HEAD, 0.0f, 1.0f / 90.0f);
  // The panel only starts turning toward the head (a few millimeters): the grabbed point stays,
  // the size too.
  ExpectNear(PointOnPanel(drag.GetPose(), hit->x, hit->y), bar);
  ExpectNear(drag.GetPose().position, PANEL.position, 1e-2f);
  EXPECT_NEAR(drag.GetSize().width, SIZE.width, 1e-4f);
}

TEST(VRPanelHandle, DraggedPanelFacesTheHeadAndKeepsItsApparentSize)
{
  const XrVector3f bar = PointOnPanel(PANEL, 0.0f, PanelHandle::GetBarCenter(SIZE).y);
  const XrVector3f hand{0.2f, 1.1f, -0.3f};
  PanelHandle::Drag drag(AimAt(hand, bar), *PanelMath::IntersectRayQuad(AimAt(hand, bar), PANEL),
                         PANEL, SIZE, HEAD);
  const float start_distance = Distance(bar, HEAD);
  const float ray_length = drag.GetRayLength();

  // Swing the laser to the right and up, and let the panel settle.
  const XrPosef aim = AimAt(hand, {1.2f, 1.6f, -0.8f});
  for (int frame = 0; frame < 120; ++frame)
    drag.Update(aim, HEAD, 0.0f, 1.0f / 90.0f);

  // The grabbed point is at the end of the laser, as far along it as when grabbed.
  const XrVector3f grabbed = RayEnd(aim, ray_length);
  const PanelMath::QuadHit start_hit = *PanelMath::IntersectRayQuad(AimAt(hand, bar), PANEL);
  const float scale = drag.GetSize().width / SIZE.width;
  ExpectNear(PointOnPanel(drag.GetPose(), start_hit.x * scale, start_hit.y * scale), grabbed,
             1e-3f);
  // As big as when grabbed, seen from the head.
  EXPECT_NEAR(scale, Distance(grabbed, HEAD) / start_distance, 1e-4f);
  // Facing the head from its centre, upright.
  const PanelMath::Heading facing = *PanelMath::HeadingToward(drag.GetPose().position, HEAD);
  const PanelMath::Heading heading = PanelMath::HeadingOf(drag.GetPose().orientation);
  EXPECT_NEAR(heading.yaw, facing.yaw, 2e-3f);
  EXPECT_NEAR(heading.pitch, facing.pitch, 2e-3f);
  EXPECT_NEAR(PanelMath::RotateVector(drag.GetPose().orientation, {1.0f, 0.0f, 0.0f}).y, 0.0f,
              1e-5f);
}

TEST(VRPanelHandle, ThumbstickPushesThePanelAway)
{
  const XrVector3f bar = PointOnPanel(PANEL, 0.0f, PanelHandle::GetBarCenter(SIZE).y);
  const XrPosef aim = AimAt({0.2f, 1.1f, -0.3f}, bar);
  PanelHandle::Drag drag(aim, *PanelMath::IntersectRayQuad(aim, PANEL), PANEL, SIZE, HEAD);
  const float ray_length = drag.GetRayLength();

  // A full push for a second doubles the distance along the laser.
  drag.Update(aim, HEAD, 8.0f, 1.0f / 90.0f);
  EXPECT_NEAR(drag.GetRayLength(), 2.0f * ray_length, 1e-3f);
  EXPECT_GT(drag.GetSize().width, SIZE.width * 1.5f);
  // Pulled back, it comes back.
  drag.Update(aim, HEAD, -8.0f, 1.0f / 90.0f);
  EXPECT_NEAR(drag.GetRayLength(), ray_length, 1e-3f);
  EXPECT_NEAR(drag.GetSize().width, SIZE.width, 1e-3f);
  // It never gets past the hand.
  drag.Update(aim, HEAD, -200.0f, 1.0f / 90.0f);
  EXPECT_GT(drag.GetRayLength(), 0.1f);
  EXPECT_GE(drag.GetSize().width, 0.25f - 1e-5f);
}
