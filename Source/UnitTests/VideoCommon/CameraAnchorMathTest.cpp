// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Camera Anchor a VR patch composes from a head bone and the game camera
// (VideoCommon/VR/CameraAnchorMath): the rig is chosen in world space, so the game camera's turns
// cancel out of the view.

#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include "VideoCommon/VR/CameraAnchorMath.h"

using namespace VR::CameraAnchorMath;

namespace
{
constexpr float DEG = 0.01745329252f;

Mat33 RotX(float deg)
{
  const float c = std::cos(deg * DEG), s = std::sin(deg * DEG);
  return {1, 0, 0, 0, c, -s, 0, s, c};
}

// + turns left: the engine's RotY (forward -Z goes to -X at +90).
Mat33 RotY(float deg)
{
  const float c = std::cos(deg * DEG), s = std::sin(deg * DEG);
  return {c, 0, s, 0, 1, 0, -s, 0, c};
}

Mat33 Transpose(const Mat33& m)
{
  return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

Mtx34 Make(const Mat33& r, const Vec3& t)
{
  return {r[0], r[1], r[2], t[0], r[3], r[4], r[5], t[1], r[6], r[7], r[8], t[2]};
}

template <size_t N>
void ExpectNear(const std::array<float, N>& actual, const std::array<float, N>& expected,
                float tolerance = 1e-4f)
{
  for (size_t i = 0; i < N; ++i)
  {
    SCOPED_TRACE(i);
    EXPECT_NEAR(actual[i], expected[i], tolerance);
  }
}

// A camera pitched down 30 degrees, and one turned and rolled: world -> view.
const Mtx34 PITCHED_VIEW = Make(RotX(-30), {0, -2, -10});
const Mtx34 TURNED_VIEW = Make(Multiply33(RotX(10), RotY(-70)), {5, 1, -3});
// Link's head: turned 40 degrees, at (1, 2, 3); its +Z points where he faces.
const Mtx34 HEAD = Make(RotY(40), {1, 2, 3});
}  // namespace

TEST(CameraAnchorMath, InvertsTransforms)
{
  const Mtx34 m = Make(Multiply33(RotX(25), RotY(-60)), {4, -5, 6});
  const std::optional<Mtx34> inverse = Invert(m);
  ASSERT_TRUE(inverse);
  ExpectNear(Multiply(m, *inverse), Make(RotY(0), {0, 0, 0}));
  // With a scale.
  Mtx34 scaled = m;
  for (float& v : scaled)
    v *= 2.0f;
  ASSERT_TRUE(Invert(scaled));
  ExpectNear(Multiply(scaled, *Invert(scaled)), Make(RotY(0), {0, 0, 0}));
  EXPECT_FALSE(Invert(Mtx34{}));
  // A world camera transform gives back the view.
  const std::optional<Mtx34> view = ViewFromCamera(*Invert(TURNED_VIEW), CameraKind::World);
  ASSERT_TRUE(view);
  ExpectNear(*view, TURNED_VIEW);
}

TEST(CameraAnchorMath, RejectsWhatIsNotATransform)
{
  State state;
  EXPECT_FALSE(Compose(Mtx34{}, nullptr, PITCHED_VIEW, {}, 1.0f, &state));
  Mtx34 broken = HEAD;
  broken[5] = std::nanf("");
  EXPECT_FALSE(Compose(broken, nullptr, PITCHED_VIEW, {}, 1.0f, &state));
  EXPECT_FALSE(Compose(HEAD, nullptr, Mtx34{}, {}, 1.0f, &state));
  EXPECT_FALSE(state.valid);
}

// The level rig follows the character's heading and ignores the game camera: expressed back in
// the world (Rv^T * A) it is the same whatever the camera does.
TEST(CameraAnchorMath, LevelCancelsTheGameCamera)
{
  for (const Mtx34& view : {PITCHED_VIEW, TURNED_VIEW})
  {
    State state;
    const std::optional<Anchor> anchor = Compose(HEAD, nullptr, view, {}, 1.0f, &state);
    ASSERT_TRUE(anchor);
    // Forward (-Z of the rig) is the head's +Z: the rig is turned 40 + 180 degrees.
    ExpectNear(Multiply33(Transpose(Basis(view)), anchor->rotation), RotY(220));
    ExpectNear(anchor->position, TransformPoint(view, {1, 2, 3}));
  }

  // A yaw offset turns it about the world's up; a nod of the head does not tilt it.
  Options options;
  options.yaw_deg = 90;
  State state;
  const Mtx34 nodding = Make(Multiply33(RotY(40), RotX(35)), {1, 2, 3});
  const std::optional<Anchor> anchor =
      Compose(nodding, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(anchor);
  ExpectNear(Multiply33(Transpose(Basis(PITCHED_VIEW)), anchor->rotation), RotY(310));
}

// The heading can come from another bone (the body root), the position from the head.
TEST(CameraAnchorMath, HeadingFromAnotherBone)
{
  State state;
  const Mtx34 root = Make(RotY(-15), {1, 0, 3});
  const std::optional<Anchor> anchor = Compose(HEAD, &root, PITCHED_VIEW, {}, 1.0f, &state);
  ASSERT_TRUE(anchor);
  ExpectNear(Multiply33(Transpose(Basis(PITCHED_VIEW)), anchor->rotation), RotY(165));
  ExpectNear(anchor->position, TransformPoint(PITCHED_VIEW, {1, 2, 3}));
}

TEST(CameraAnchorMath, FixedKeepsTheHeadingItEngagedWith)
{
  Options options;
  options.rotation = Rotation::Fixed;
  State state;
  ASSERT_TRUE(Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state));
  const std::optional<Anchor> turned =
      Compose(Make(RotY(130), {1, 2, 3}), nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(turned);
  ExpectNear(Multiply33(Transpose(Basis(TURNED_VIEW)), turned->rotation), RotY(220));
  // Engaging again takes the new heading.
  state.Reset();
  const std::optional<Anchor> again =
      Compose(Make(RotY(130), {1, 2, 3}), nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(again);
  ExpectNear(Multiply33(Transpose(Basis(TURNED_VIEW)), again->rotation), RotY(310));
}

// Full: the bone's whole orientation (its +Z forward, +Y up), turned by the yaw about its Y.
TEST(CameraAnchorMath, FullFollowsTheBone)
{
  Options options;
  options.rotation = Rotation::Full;
  const Mat33 bone = Multiply33(RotY(40), RotX(25));
  State state;
  const std::optional<Anchor> anchor =
      Compose(Make(bone, {1, 2, 3}), nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(anchor);
  ExpectNear(anchor->rotation, Multiply33(Basis(TURNED_VIEW), Multiply33(bone, RotY(180))));
  options.yaw_deg = 30;
  state.Reset();
  const std::optional<Anchor> yawed =
      Compose(Make(bone, {1, 2, 3}), nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(yawed);
  ExpectNear(yawed->rotation, Multiply33(Basis(TURNED_VIEW), Multiply33(bone, RotY(210))));
}

// Camera and Yaw stay in the game camera's view, like the draw anchor's Off and Yaw modes.
TEST(CameraAnchorMath, CameraAndYawStayInTheGameView)
{
  Options options;
  options.rotation = Rotation::Camera;
  options.offset_m = {0.0f, 0.5f, 0.0f};
  State state;
  const std::optional<Anchor> camera = Compose(HEAD, nullptr, PITCHED_VIEW, options, 10.0f, &state);
  ASSERT_TRUE(camera);
  ExpectNear(camera->rotation, RotY(0));
  // The offset is along the game camera's up.
  const Vec3 head = TransformPoint(PITCHED_VIEW, {1, 2, 3});
  ExpectNear(camera->position, Vec3{head[0], head[1] + 5.0f, head[2]});

  options.rotation = Rotation::Yaw;
  options.offset_m = {};
  state.Reset();
  const std::optional<Anchor> yaw = Compose(HEAD, nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(yaw);
  // A turn about the view's up, facing the head's +Z as the view sees it.
  const Mat33 r = yaw->rotation;
  EXPECT_NEAR(r[4], 1.0f, 1e-4f);
  const Vec3 forward = {-r[2], -r[5], -r[8]};
  const Mat33 v = Basis(TURNED_VIEW);
  const Vec3 seen = {v[0] * std::sin(40 * DEG) + v[2] * std::cos(40 * DEG), 0.0f,
                     v[6] * std::sin(40 * DEG) + v[8] * std::cos(40 * DEG)};
  const float length = std::sqrt(seen[0] * seen[0] + seen[2] * seen[2]);
  ExpectNear(forward, Vec3{seen[0] / length, 0.0f, seen[2] / length});
}

// The meter offsets follow the rig in the world: up is the world's up, forward the heading.
TEST(CameraAnchorMath, OffsetsAlongTheRig)
{
  Options options;
  options.offset_m = {0.0f, 0.5f, 0.2f};
  State state;
  const std::optional<Anchor> anchor = Compose(HEAD, nullptr, PITCHED_VIEW, options, 10.0f, &state);
  ASSERT_TRUE(anchor);
  // Forward is the head's +Z: (sin 40, 0, cos 40), 2 units; up 5 units.
  const Vec3 eye = {1 + 2 * std::sin(40 * DEG), 2 + 5.0f, 3 + 2 * std::cos(40 * DEG)};
  ExpectNear(anchor->position, TransformPoint(PITCHED_VIEW, eye));
  // No bone hide unless asked.
  EXPECT_EQ(anchor->hide_radius, 0.0f);
}

// The bone hide is centred on the head as the game's draw matrices place it: no offsets, no
// smoothing.
TEST(CameraAnchorMath, HideCentreIsTheRawHead)
{
  Options options;
  options.offset_m = {0.0f, 0.5f, 0.2f};
  options.smoothing = 0.5f;
  options.hide_radius = 12.0f;
  State state;
  ASSERT_TRUE(Compose(HEAD, nullptr, PITCHED_VIEW, options, 10.0f, &state));
  const std::optional<Anchor> moved =
      Compose(Make(RotY(40), {1.4f, 2, 3}), nullptr, PITCHED_VIEW, options, 10.0f, &state);
  ASSERT_TRUE(moved);
  ExpectNear(moved->hide_center, TransformPoint(PITCHED_VIEW, {1.4f, 2, 3}));
  EXPECT_EQ(moved->hide_radius, 12.0f);
}

// The hide region: within the radius of the head bone, and not lower than hide_below under it
// along the world's up, whatever the camera's pitch (Wind Waker: the hair above the neck bone,
// the shoulders under it).
TEST(CameraAnchorMath, HideRegionAroundTheHead)
{
  Options options;
  options.hide_radius = 40.0f;
  options.hide_below = 5.0f;
  State state;
  const Mtx34 head = Make(RotY(40), {0, 100, 0});
  const std::optional<Anchor> anchor = Compose(head, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(anchor);
  const auto seen = [&](const Vec3& world) { return TransformPoint(PITCHED_VIEW, world); };
  // Hair 25 above and to the side, a face part 3 above, the neck 4 under: hidden.
  EXPECT_TRUE(IsInHideRegion(*anchor, seen({18, 125, 5})));
  EXPECT_TRUE(IsInHideRegion(*anchor, seen({0, 103, 8})));
  EXPECT_TRUE(IsInHideRegion(*anchor, seen({0, 96, 0})));
  // The shoulders 13 under, the torso: drawn. Far away: drawn.
  EXPECT_FALSE(IsInHideRegion(*anchor, seen({15, 87, 0})));
  EXPECT_FALSE(IsInHideRegion(*anchor, seen({0, 70, 2})));
  EXPECT_FALSE(IsInHideRegion(*anchor, seen({60, 100, 0})));
  // No limit under: a sphere.
  options.hide_below = -1.0f;
  state.Reset();
  const std::optional<Anchor> sphere = Compose(head, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(sphere);
  EXPECT_TRUE(IsInHideRegion(*sphere, seen({15, 87, 0})));
  // No radius: nothing.
  options.hide_radius = 0.0f;
  state.Reset();
  const std::optional<Anchor> none = Compose(head, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(none);
  EXPECT_FALSE(IsInHideRegion(*none, seen({0, 100, 0})));
}

// Smoothing is in world space, and a jump over 2 m snaps.
TEST(CameraAnchorMath, SmoothsInWorldSpace)
{
  Options options;
  options.smoothing = 0.5f;
  options.heading_smoothing = 0.0f;
  State state;
  ASSERT_TRUE(Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state));
  const std::optional<Anchor> moved =
      Compose(Make(RotY(40), {1.4f, 2, 3}), nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(moved);
  ExpectNear(moved->position, TransformPoint(TURNED_VIEW, {1.2f, 2, 3}));
  const std::optional<Anchor> jumped =
      Compose(Make(RotY(40), {9, 2, 3}), nullptr, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(jumped);
  ExpectNear(jumped->position, TransformPoint(TURNED_VIEW, {9, 2, 3}));

  // The heading blends too.
  options.smoothing = 0.0f;
  options.heading_smoothing = 0.5f;
  state.Reset();
  ASSERT_TRUE(Compose(Make(RotY(0), {0, 0, 0}), nullptr, PITCHED_VIEW, options, 1.0f, &state));
  const std::optional<Anchor> turned =
      Compose(Make(RotY(60), {0, 0, 0}), nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(turned);
  ExpectNear(Multiply33(Transpose(Basis(PITCHED_VIEW)), turned->rotation), RotY(210), 1e-3f);
}

TEST(CameraAnchorMath, ZUpWorlds)
{
  // Up is +Z: forward (1, 0, 0.3) is laid on the ground plane.
  const std::optional<Mat33> rig = LevelBasis({1.0f, 0.0f, 0.3f}, {0, 0, 1}, 0);
  ASSERT_TRUE(rig);
  // Y = up, -Z = forward.
  ExpectNear(Vec3{(*rig)[1], (*rig)[4], (*rig)[7]}, Vec3{0, 0, 1});
  ExpectNear(Vec3{-(*rig)[2], -(*rig)[5], -(*rig)[8]}, Vec3{1, 0, 0});
  // No heading straight up.
  EXPECT_FALSE(LevelBasis({0.0f, 0.0f, 1.0f}, {0, 0, 1}, 0));
}

TEST(CameraAnchorMath, ParsesNames)
{
  EXPECT_EQ(ParseRotation("level"), Rotation::Level);
  EXPECT_EQ(ParseRotation("yaw"), Rotation::Yaw);
  EXPECT_FALSE(ParseRotation("Level"));
  EXPECT_EQ(ParseAxis("-x"), Axis::NegX);
  EXPECT_EQ(ParseAxis("z"), Axis::Z);
  EXPECT_FALSE(ParseAxis("w"));
  EXPECT_EQ(ParseCameraKind("world"), CameraKind::World);
  EXPECT_FALSE(ParseCameraKind("eye"));
}
