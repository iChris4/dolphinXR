// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Camera Anchor a VR patch composes from a head bone and the game camera
// (VideoCommon/VR/CameraAnchorMath): the rig is chosen in world space, so the game camera's turns
// cancel out of the view.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

#include "VideoCommon/VR/CameraAnchorMath.h"
#include "VideoCommon/VR/CameraAnchorSource.h"

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

// The stick: up walks straight ahead in the view. The rig's heading is 220 degrees, the game
// camera's 0 (pitched) or 70 (turned left): the stick turns by the difference.
TEST(CameraAnchorMath, StickTurnsFromTheCameraToTheView)
{
  Options options;
  options.stick = Stick::View;
  const std::pair<Mtx34, float> cases[] = {{PITCHED_VIEW, -140.0f}, {TURNED_VIEW, 150.0f}};
  for (const auto& [view, expected] : cases)
  {
    State state;
    const std::optional<Anchor> anchor = Compose(HEAD, nullptr, view, options, 1.0f, &state);
    ASSERT_TRUE(anchor);
    EXPECT_FALSE(anchor->stick_head);
    const std::optional<float> turn = StickTurn(*anchor, nullptr);
    ASSERT_TRUE(turn);
    EXPECT_NEAR(*turn / DEG, expected, 0.01f);
  }
  // With the rig in the game camera's orientation there is nothing to turn.
  options.rotation = Rotation::Camera;
  State camera_state;
  const std::optional<Anchor> camera =
      Compose(HEAD, nullptr, TURNED_VIEW, options, 1.0f, &camera_state);
  ASSERT_TRUE(camera);
  EXPECT_NEAR(*StickTurn(*camera, nullptr), 0.0f, 1e-5f);
  // Stick::Camera, the default: the game's own.
  State default_state;
  const std::optional<Anchor> game = Compose(HEAD, nullptr, TURNED_VIEW, {}, 1.0f, &default_state);
  ASSERT_TRUE(game);
  EXPECT_FALSE(StickTurn(*game, nullptr));
}

// Where the headset looks: its yaw adds, also when it looks straight down or up.
TEST(CameraAnchorMath, StickFollowsTheHead)
{
  // Yaw about Y, then pitch about X (x, y, z, w).
  const auto head = [](float yaw_deg, float pitch_deg) {
    const float cy = std::cos(yaw_deg * DEG / 2), sy = std::sin(yaw_deg * DEG / 2);
    const float cp = std::cos(pitch_deg * DEG / 2), sp = std::sin(pitch_deg * DEG / 2);
    return std::array<float, 4>{cy * sp, sy * cp, -sy * sp, cy * cp};
  };
  EXPECT_NEAR(HeadYaw(head(30, 0)) / DEG, 30.0f, 0.01f);
  EXPECT_NEAR(HeadYaw(head(-45, 20)) / DEG, -45.0f, 0.01f);
  EXPECT_NEAR(HeadYaw(head(30, -60)) / DEG, 30.0f, 0.01f);
  EXPECT_NEAR(HeadYaw(head(30, -90)) / DEG, 30.0f, 0.01f);
  EXPECT_NEAR(HeadYaw(head(30, 90)) / DEG, 30.0f, 0.01f);

  Options options;
  options.stick = Stick::Head;
  State state;
  const std::optional<Anchor> anchor = Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(anchor);
  EXPECT_TRUE(anchor->stick_head);
  const std::array<float, 4> looking_left = head(30, -10);
  EXPECT_NEAR(*StickTurn(*anchor, &looking_left) / DEG, -110.0f, 0.01f);
  EXPECT_NEAR(*StickTurn(*anchor, nullptr) / DEG, -140.0f, 0.01f);
  // Through the half turn: wrapped.
  const std::array<float, 4> looking_back = head(-60, 0);
  EXPECT_NEAR(*StickTurn(*anchor, &looking_back) / DEG, 160.0f, 0.01f);

  // The view with Fixed is where the headset looks: its rig never turns.
  options.stick = Stick::View;
  options.rotation = Rotation::Fixed;
  State fixed_state;
  const std::optional<Anchor> fixed =
      Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &fixed_state);
  ASSERT_TRUE(fixed);
  EXPECT_TRUE(fixed->stick_head);
}

// Facing: the heading bone's own heading, not the smoothed rig the view shows.
TEST(CameraAnchorMath, StickAlongTheFacing)
{
  Options options;
  options.stick = Stick::Facing;
  options.heading_smoothing = 0.9f;
  State state;
  ASSERT_TRUE(Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state));
  // Link turns 30 degrees to his left: the view lags behind, the stick does not.
  const Mtx34 turned = Make(RotY(70), {1, 2, 3});
  State view_state = state;
  const std::optional<Anchor> anchor =
      Compose(turned, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(anchor);
  EXPECT_NEAR(*StickTurn(*anchor, nullptr) / DEG, -110.0f, 0.01f);
  options.stick = Stick::View;
  const std::optional<Anchor> view =
      Compose(turned, nullptr, PITCHED_VIEW, options, 1.0f, &view_state);
  ASSERT_TRUE(view);
  EXPECT_LT(*StickTurn(*view, nullptr) / DEG, -130.0f);
}

// The game camera turning 5 degrees per update (every other field): with stick_lead 1 the stick
// aims from the camera the game will move the character with, one update ahead.
TEST(CameraAnchorMath, StickLeadsTheTurningCamera)
{
  Options options;
  options.stick = Stick::Facing;
  options.stick_lead = 1.0f;
  State state;
  // World -> view of a camera whose heading is `deg` (+ = left).
  const auto view = [](float deg) { return Make(RotY(-deg), {0, 0, -10}); };
  const auto turn = [&](float deg) {
    const std::optional<Anchor> anchor = Compose(HEAD, nullptr, view(deg), options, 1.0f, &state);
    EXPECT_TRUE(anchor);
    return anchor ? *StickTurn(*anchor, nullptr) / DEG : 0.0f;
  };
  EXPECT_NEAR(turn(0), -140.0f, 0.01f);
  // The rig's 220 degrees from the camera's next 10.
  EXPECT_NEAR(turn(5), -150.0f, 0.01f);
  EXPECT_NEAR(turn(5), -150.0f, 0.01f);
  EXPECT_NEAR(turn(5), -150.0f, 0.01f);
  // Three fields without a move: it stopped.
  EXPECT_NEAR(turn(5), -145.0f, 0.01f);
  // A cut is no turn.
  EXPECT_NEAR(turn(65), 155.0f, 0.01f);
  // Without a lead, the camera as it is.
  options.stick_lead = 0.0f;
  EXPECT_NEAR(turn(70), 150.0f, 0.01f);
}

// The stick reference's heading in the world (stick_yaw from the camera, plus the camera's) stays
// the rig's while the camera turns, with its lead.
TEST(CameraAnchorMath, StickReferenceInTheWorld)
{
  Options options;
  options.stick = Stick::Facing;
  options.stick_lead = 1.0f;
  State state;
  const auto view = [](float deg) { return Make(RotY(-deg), {0, 0, -10}); };
  for (const float deg : {0.0f, 5.0f, 10.0f, 70.0f})
  {
    SCOPED_TRACE(deg);
    const std::optional<Anchor> anchor = Compose(HEAD, nullptr, view(deg), options, 1.0f, &state);
    ASSERT_TRUE(anchor);
    EXPECT_NEAR(std::remainder(anchor->stick_yaw + anchor->stick_camera_yaw, 360 * DEG) / DEG,
                -140.0f, 0.01f);
    EXPECT_TRUE(anchor->stick_steer_behind);
  }
  options.rotation = Rotation::Fixed;
  const std::optional<Anchor> fixed = Compose(HEAD, nullptr, view(70), options, 1.0f, &state);
  ASSERT_TRUE(fixed);
  EXPECT_FALSE(fixed->stick_steer_behind);
}

TEST(CameraAnchorMath, TurnsTheStick)
{
  ExpectNear(TurnStick(0, 1, 90 * DEG), {-1, 0});
  ExpectNear(TurnStick(1, 0, 90 * DEG), {0, 1});
  ExpectNear(TurnStick(0, 100, -140 * DEG), {64.2788f, -76.6044f}, 1e-3f);
}

// A push back holds the reference's heading in the world: while the character turns around (the
// reference with him) and the game camera swings, the stick keeps sending him to the same back.
// Turned around, it steers from behind him; back in the front half or the centre, it lets go.
TEST(CameraAnchorMath, StickHeldBackTurnsAroundOnce)
{
  StickHold hold;
  // The reference 30 degrees left of the game camera, which heads 10 degrees left: 40 in the world.
  ExpectNear(TurnStickHeld(0, -100, 25, 30 * DEG, 10 * DEG, true, &hold),
             TurnStick(0, -100, 30 * DEG), 1e-3f);
  EXPECT_TRUE(hold.held);
  // He turned 80 degrees and the camera 40: the stick's back is still the world's 220.
  const float camera = 50 * DEG;
  ExpectNear(TurnStickHeld(0, -100, 25, 120 * DEG - camera, camera, true, &hold),
             TurnStick(0, -100, 40 * DEG - camera), 1e-3f);
  // Turned around (the reference at 220): back runs ahead, back-left to his front-left.
  const float turned = 220 * DEG - camera;
  ExpectNear(TurnStickHeld(0, -100, 25, turned, camera, true, &hold), TurnStick(0, 100, turned),
             1e-3f);
  ExpectNear(TurnStickHeld(-70.7f, -70.7f, 25, turned, camera, true, &hold),
             TurnStick(-70.7f, 70.7f, turned), 1e-3f);
  EXPECT_TRUE(hold.held);
  // The front half lets go: the plain turn.
  ExpectNear(TurnStickHeld(-100, 10, 25, turned, camera, true, &hold), TurnStick(-100, 10, turned),
             1e-3f);
  EXPECT_FALSE(hold.held);
  // Less than 120 degrees from up: a sideways push, not held.
  ExpectNear(TurnStickHeld(-100, -40, 25, turned, camera, true, &hold),
             TurnStick(-100, -40, turned), 1e-3f);
  EXPECT_FALSE(hold.held);
  // Held again from the new heading, then let go at the centre.
  TurnStickHeld(0, -100, 25, turned, camera, true, &hold);
  EXPECT_TRUE(hold.held);
  EXPECT_NEAR(hold.heading / DEG, 220.0f, 0.01f);
  TurnStickHeld(5, -10, 25, 0, camera, true, &hold);
  EXPECT_FALSE(hold.held);
}

// VR controls: the stick's turn turns a Fixed rig, which the character's turns do not move; the
// stick reference is where the headset looks and is not held back. The turn speed applies with
// Fixed only.
TEST(CameraAnchorMath, StickTurnsAFixedRig)
{
  Options options;
  options.rotation = Rotation::Fixed;
  options.stick = Stick::Head;
  options.stick_turn_speed = 90.0f;
  State state;
  const auto world_rig = [](const Anchor& anchor) {
    return Multiply33(Transpose(Basis(PITCHED_VIEW)), anchor.rotation);
  };
  const std::optional<Anchor> engaged = Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(engaged);
  ExpectNear(world_rig(*engaged), RotY(220));
  EXPECT_EQ(engaged->stick_turn_speed, 90.0f);
  EXPECT_TRUE(engaged->stick_head);
  EXPECT_FALSE(engaged->stick_hold);
  // Turned 30 degrees left by the stick while the character turns 90: the rig follows the stick.
  const std::optional<Anchor> turned =
      Compose(Make(RotY(130), {1, 2, 3}), nullptr, PITCHED_VIEW, options, 1.0f, &state, 30.0f);
  ASSERT_TRUE(turned);
  ExpectNear(world_rig(*turned), RotY(250));
  const std::optional<Anchor> still =
      Compose(Make(RotY(130), {1, 2, 3}), nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(still);
  ExpectNear(world_rig(*still), RotY(250));

  options.stick = Stick::Facing;
  EXPECT_TRUE(Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state)->stick_hold);
  options.rotation = Rotation::Level;
  options.stick = Stick::Head;
  const std::optional<Anchor> level = Compose(HEAD, nullptr, PITCHED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(level);
  EXPECT_EQ(level->stick_turn_speed, 0.0f);
  EXPECT_TRUE(level->stick_hold);
}

// Without steer_behind (the view does not turn with him), a held push steers as seen from the held
// heading.
TEST(CameraAnchorMath, StickHeldBackFromTheHeldHeading)
{
  StickHold hold;
  TurnStickHeld(0, -100, 25, 30 * DEG, 10 * DEG, false, &hold);
  ExpectNear(TurnStickHeld(-70.7f, -70.7f, 25, 200 * DEG, 10 * DEG, false, &hold),
             TurnStick(-70.7f, -70.7f, 30 * DEG), 1e-3f);
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
  EXPECT_EQ(ParseStick("facing"), Stick::Facing);
  EXPECT_EQ(ParseStick("view"), Stick::View);
  EXPECT_EQ(ParseStick("head"), Stick::Head);
  EXPECT_FALSE(ParseStick("body"));
}

// An anchor composed from one frame's pair, moved onto another frame's: the rig stays where it was
// relative to the root (which moved on) and keeps its world rotation whatever the camera did, so
// it is what composing from the new pair gives. A rig that follows the camera keeps following it.
TEST(CameraAnchorMath, RebasesAnAnchorOntoAnotherFramesPair)
{
  Options options;
  options.rotation = Rotation::Level;
  options.offset_m = {0.0f, 0.1f, 0.2f};
  options.hide_radius = 1.0f;
  const Mtx34 root = Make(RotY(40), {1, 0, 3});
  State state;
  const std::optional<Anchor> anchor = Compose(HEAD, &root, TURNED_VIEW, options, 1.0f, &state);
  ASSERT_TRUE(anchor);
  EXPECT_TRUE(anchor->rotation_in_world);
  // The next frame: he moved on, the camera followed him and turned.
  const Mtx34 root_now = Make(RotY(40), {1.5f, 0.0f, 2.8f});
  const Mtx34 head_now = Make(RotY(40), {1.5f, 2.0f, 2.8f});
  const Mtx34 view_now = Multiply(Make(RotY(10), {0, 0, 0}),
                                  Multiply(TURNED_VIEW, Make(RotY(0), {-0.5f, 0.0f, 0.2f})));
  const std::optional<Anchor> rebased =
      RebaseAnchor(*anchor, TURNED_VIEW, root, view_now, root_now);
  ASSERT_TRUE(rebased);
  State fresh;
  const std::optional<Anchor> composed =
      Compose(head_now, &root_now, view_now, options, 1.0f, &fresh);
  ASSERT_TRUE(composed);
  ExpectNear(rebased->position, composed->position);
  ExpectNear(rebased->rotation, composed->rotation);
  ExpectNear(rebased->hide_center, composed->hide_center);
  ExpectNear(rebased->hide_up, composed->hide_up);
  EXPECT_EQ(rebased->hide_radius, 1.0f);

  options.rotation = Rotation::Camera;
  State following_state;
  const std::optional<Anchor> following =
      Compose(HEAD, &root, TURNED_VIEW, options, 1.0f, &following_state);
  ASSERT_TRUE(following);
  EXPECT_FALSE(following->rotation_in_world);
  const std::optional<Anchor> moved =
      RebaseAnchor(*following, TURNED_VIEW, root, view_now, root_now);
  ASSERT_TRUE(moved);
  ExpectNear(moved->rotation, following->rotation);
  // The head, a frame on, in the new view.
  ExpectNear(moved->hide_center, TransformPoint(view_now, {1.5f, 2.0f, 2.8f}));

  // A pair that is not a transform: nothing.
  EXPECT_FALSE(RebaseAnchor(*anchor, TURNED_VIEW, root, Mtx34{}, root_now));
}

// The frame pairs a patch's cave records: an entry decodes when its two sequence numbers agree
// and its matrices are transforms, and a frame takes the newest entry recorded before the
// command the GPU decodes, by the FIFO write pointer on the ring, the oldest when all are ahead.
TEST(CameraAnchorMath, PicksTheFramePairRecordedBeforeTheCommand)
{
  const auto words = [](u32 seq, u32 write, const Mtx34& view, const Mtx34& root, u32 seq_end) {
    std::array<u32, FRAME_PAIR_WORDS> w{};
    w[0] = seq;
    w[1] = write;
    for (size_t i = 0; i < 12; ++i)
    {
      w[FRAME_PAIR_VIEW / 4 + i] = std::bit_cast<u32>(view[i]);
      w[FRAME_PAIR_ROOT / 4 + i] = std::bit_cast<u32>(root[i]);
    }
    w[FRAME_PAIR_SEQ_END / 4] = seq_end;
    return w;
  };
  const std::optional<FramePair> pair = DecodeFramePair(words(5, 0x1234, TURNED_VIEW, HEAD, 5));
  ASSERT_TRUE(pair);
  EXPECT_EQ(pair->seq, 5u);
  EXPECT_EQ(pair->fifo_write, 0x1234u);
  ExpectNear(pair->view, TURNED_VIEW);
  ExpectNear(pair->root, HEAD);
  EXPECT_FALSE(DecodeFramePair(words(5, 0x1234, TURNED_VIEW, HEAD, 4)));
  EXPECT_FALSE(DecodeFramePair(words(0, 0x1234, TURNED_VIEW, HEAD, 0)));
  EXPECT_FALSE(DecodeFramePair(words(5, 0x1234, Mtx34{}, HEAD, 5)));

  // A 256 KiB FIFO: pair 1 recorded near its end, 2 and 3 after the wrap, 4 ahead of the GPU.
  constexpr u32 BASE = 0x0060D880, SIZE = 0x40000;
  const auto at = [](u32 seq, u32 offset) {
    FramePair p;
    p.seq = seq;
    p.fifo_write = BASE + offset;
    return p;
  };
  const std::array<FramePair, 4> pairs = {at(1, 0x3F000), at(2, 0x200), at(3, 0x2000),
                                          at(4, 0x4000)};
  // Decoding at 0x3500 with 0x1000 bytes written after it: pair 3.
  EXPECT_EQ(PickFramePair(pairs, BASE, SIZE, 0x3500, 0x1000), 2u);
  // Decoding the command pair 1 was recorded at, pairs 2 to 4 ahead: pair 1.
  EXPECT_EQ(PickFramePair(pairs, BASE, SIZE, 0x3F000, 0x5000), 0u);
  // The GPU further behind, all of them ahead: the oldest.
  EXPECT_EQ(PickFramePair(pairs, BASE, SIZE, 0x3E000, 0x6000), 0u);
  EXPECT_FALSE(PickFramePair({}, BASE, SIZE, 0x3E000, 0x6000));
}

// A frame takes the last anchor published before its commands in the FIFO, not the newest: with
// dual core the CPU thread may have published the next field's one before the GPU thread draws.
TEST(CameraAnchorSource, FrameTakesTheAnchorBeforeItsCommands)
{
  const auto anchor_at = [](float x) {
    Anchor anchor;
    anchor.position = {x, 0, 0};
    return anchor;
  };
  FramePairs pairs;
  pairs.ring_address = 0x80002A00;
  pairs.entries = 3;
  VR::CameraAnchorSource::Set(anchor_at(1), 1000);
  VR::CameraAnchorSource::Set(anchor_at(2), 2000, pairs);
  VR::CameraAnchorSource::Set(anchor_at(3), 3000);
  int newer = -1;
  std::optional<FramePairs> taken;
  std::optional<Anchor> anchor = VR::CameraAnchorSource::GetAt(2500, &newer, &taken);
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->position[0], 2.0f);
  EXPECT_EQ(newer, 1);
  // Its frame pairs come with it.
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->ring_address, 0x80002A00u);
  EXPECT_EQ(taken->entries, 3u);
  VR::CameraAnchorSource::GetAt(3000, &newer, &taken);
  EXPECT_FALSE(taken);
  anchor = VR::CameraAnchorSource::GetAt(3000, &newer);
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->position[0], 3.0f);
  EXPECT_EQ(newer, 0);
  EXPECT_EQ(VR::CameraAnchorSource::Get()->position[0], 3.0f);
  // All newer than the frame (a FIFO reset): the newest.
  anchor = VR::CameraAnchorSource::GetAt(500, &newer);
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->position[0], 3.0f);
  // No anchor any more: none, whatever the frame.
  VR::CameraAnchorSource::Set(std::nullopt, 4000);
  EXPECT_FALSE(VR::CameraAnchorSource::GetAt(3500));
  EXPECT_FALSE(VR::CameraAnchorSource::Get());
}
