// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The Camera Anchor of a VR game patch (Core/VRPatch): the VR camera placed in a character's head
// from the game's memory (vr.set_camera_anchor_matrices). The patch reads the head bone's world
// transform and the game camera's matrix, and this composes the anchor the engine applies (game
// view space, game units, the space of the draws' position matrices).
//
// The rig is chosen in WORLD space and then expressed in view space: the anchor's rotation is
// Rv * L (Rv the game camera's world->view rotation, L the rig in world space), so the eye rig
// A * R (OpenXRManager::ComputeEyeRigs) turns the game camera back out and the third-person
// camera's swings no longer move the VR view. Smoothing is done here, in world space, for the
// same reason: a blend in view space would lag behind the game camera's turns.

#include <array>
#include <optional>
#include <string_view>

#include "Common/CommonTypes.h"

namespace VR::CameraAnchorMath
{
// A GameCube Mtx: row-major 3x4, [r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz].
using Mtx34 = std::array<float, 12>;
// Row-major 3x3.
using Mat33 = std::array<float, 9>;
using Vec3 = std::array<float, 3>;

// What the camera matrix a patch names is.
enum class CameraKind : u8
{
  // World -> view (the matrix the game multiplies its models by, e.g. J3D's view Mtx).
  View,
  // The camera's own transform in the world (its position is the translation): inverted.
  World,
};

// How the VR camera rig is turned.
enum class Rotation : u8
{
  // Level, turned with the character's heading: a bone's forward axis on the ground plane.
  Level,
  // Level, with the heading the character had when the anchor engaged: the headset does all the
  // turning afterwards.
  Fixed,
  // The bone's whole orientation (cockpits).
  Full,
  // The game camera's orientation (position only): its swings move the view.
  Camera,
  // The bone's heading in the game camera's view: the camera's pitch and roll move the view (the
  // draw anchor's Yaw mode).
  Yaw,
};

// What the main stick's up means while the anchor holds the camera. The game moves its character
// relative to its own camera, which no longer turns the view: the engine turns the stick by the
// angle between the game camera's heading and this one's.
enum class Stick : u8
{
  // The game's camera (the stick is not turned).
  Camera,
  // Where the character faces: the heading bone's forward axis, not smoothed (with the yaw offset).
  // Up keeps him going straight; a sideways push turns him for as long as it is held; a push back
  // turns him around once (TurnStickHeld).
  Facing,
  // Straight ahead in the view: the rig's heading as shown (smoothed), the character's with Level
  // (where the headset looks with Fixed, whose rig never turns).
  View,
  // Where the headset looks. With Level, looking aside while walking turns the character, and the
  // rig with him, for as long as the head stays turned.
  Head,
};

enum class Axis : u8
{
  X,
  Y,
  Z,
  NegX,
  NegY,
  NegZ,
};

struct Options
{
  Rotation rotation = Rotation::Level;
  // Degrees about the up axis, + turns left (the draw anchor's anchor_yaw).
  float yaw_deg = 0.0f;
  // Meters to the right, up and forward of the rig (of the game camera for Camera and Yaw).
  Vec3 offset_m{};
  // The world's up axis.
  Axis up_axis = Axis::Y;
  // The heading bone's axis that points where the character faces.
  Axis forward_axis = Axis::Z;
  // Full: the bone's axis that points up.
  Axis bone_up_axis = Axis::Y;
  // Fraction of the previous position and rotation kept at each update, 0 to 0.95.
  float smoothing = 0.0f;
  float heading_smoothing = 0.0f;
  // Game units: the draws whose centre is within this distance of the head bone and at most
  // hide_below under it (along the world's up) are hidden while the anchor holds the camera; 0 =
  // none. hide_below < 0: no limit.
  float hide_radius = 0.0f;
  float hide_below = -1.0f;
  Stick stick = Stick::Camera;
  // Camera updates the game's control angle is ahead of the camera matrix the patch reads when it
  // reads the stick: the camera's last turn is added that many times. Zelda games (Wind Waker,
  // Twilight Princess) read the pad, then update the camera, then move the player with it: 1.
  float stick_lead = 0.0f;
  // Degrees per second; 0 = off. With Fixed, the stick's left and right turn the rig at up to this
  // speed instead of reaching the game, and up and back walk along the stick's reference (with View
  // or Head: where the headset looks): back walks backward, the view does not turn with him.
  float stick_turn_speed = 0.0f;
};

// Smoothing state, in world space. Reset() when the anchor engages again.
struct State
{
  bool valid = false;
  Vec3 position{};
  // The rig in world space (columns = its axes); Level, Fixed and Full only.
  Mat33 rotation{};
  // Fixed: the heading latched when the anchor engaged.
  bool fixed_valid = false;
  Vec3 fixed_forward{};
  // The game camera's heading about the world's up at the last update, its last turn (radians, + =
  // left) and the updates since it last turned (games move it every other field at 30 fps).
  bool camera_valid = false;
  float camera_heading = 0.0f;
  float camera_turn = 0.0f;
  int camera_still = 0;

  void Reset() { *this = {}; }
};

// The anchor the engine applies (OpenXRManager::LatchCameraAnchorSource).
struct Anchor
{
  // Game view space, game units.
  Vec3 position{};
  // Columns = the camera rig's axes in view space.
  Mat33 rotation{};
  // The anchor's own world scale; <= 0 keeps the profile's.
  float units_per_meter = 0.0f;
  // The head bone in game view space, as the game's draw matrices place it (not smoothed, without
  // the offsets), the world's up in view space, and the region of the draws hidden around it
  // (radius 0 = none; below < 0 = no limit).
  Vec3 hide_center{};
  Vec3 hide_up{0.0f, 1.0f, 0.0f};
  float hide_radius = 0.0f;
  float hide_below = -1.0f;
  // The main stick: whether it is turned, the rig's heading from the game camera's about the
  // world's up (radians, + = left), and whether the headset's yaw adds to it (Stick::Head, or View
  // with Fixed).
  bool stick_turn = false;
  float stick_yaw = 0.0f;
  bool stick_head = false;
  // The game camera's heading about the world's up as the game will move the character with it
  // (stick_lead included; radians, + = left, from a fixed world direction): stick_yaw plus this is
  // the stick reference's heading in the world. Whether the reference turns with the character
  // (Facing, or a rig that is not Fixed): a push back is held (TurnStickHeld). And whether the rig
  // turns with him (Level, Full): a stick held back then steers as seen from behind him.
  float stick_camera_yaw = 0.0f;
  bool stick_hold = false;
  bool stick_steer_behind = false;
  // Options::stick_turn_speed when it applies (Fixed), else 0: the stick's left and right turn the
  // rig (Compose's turn_deg) and do not reach the game.
  float stick_turn_speed = 0.0f;
};

// A push back on the main stick, held (TurnStickHeld).
struct StickHold
{
  bool held = false;
  // The stick reference's heading about the world's up when the push began (radians, + = left).
  float heading = 0.0f;
};

bool IsFinite(const Mtx34& m);
// Finite, a 3x3 that is not degenerate (rows 0.001 to 10000 long, |determinant| > 1e-9) and a
// translation under 1e7: what a bone or camera transform can be (with a scale).
bool IsPlausibleTransform(const Mtx34& m);
std::optional<Mtx34> Invert(const Mtx34& m);
Mtx34 Multiply(const Mtx34& a, const Mtx34& b);
Mat33 Multiply33(const Mat33& a, const Mat33& b);
Vec3 TransformPoint(const Mtx34& m, const Vec3& p);
Mat33 Basis(const Mtx34& m);
Vec3 Translation(const Mtx34& m);
// The world->view transform of a camera matrix; nullopt when it is not a plausible transform.
std::optional<Mtx34> ViewFromCamera(const Mtx34& camera, CameraKind kind);
Vec3 AxisVector(Axis axis);
// The matrix's axis: a column of its 3x3, negated for the Neg axes.
Vec3 Column(const Mtx34& m, Axis axis);
// The rig whose forward (-Z) is `forward` laid on the plane normal to `up`, turned by yaw_deg about
// `up` (+ = left), with up as its Y. nullopt when forward is (nearly) parallel to up.
std::optional<Mat33> LevelBasis(const Vec3& forward, const Vec3& up, float yaw_deg);
// Gram-Schmidt on the columns, Y kept (up stays up). False when a column is degenerate.
bool Orthonormalize(Mat33* m);

// The anchor of a head bone (world transform), the bone giving the heading (world transform;
// null: the head's) and the game camera's world->view transform. `upm` converts the meter offsets
// to game units. Smooths in world space through `state`. nullopt when a transform is implausible
// or the heading cannot be found (the forward axis points straight up) before any was. turn_deg:
// the stick's turn since the last update (+ = left), which turns a Fixed rig.
std::optional<Anchor> Compose(const Mtx34& head, const Mtx34* heading, const Mtx34& view,
                              const Options& options, float upm, State* state,
                              float turn_deg = 0.0f);

// Whether a draw centred at `point` (game view space) is in the anchor's hide region: within its
// radius of the head bone, and at most hide_below under it along the world's up.
bool IsInHideRegion(const Anchor& anchor, const Vec3& point);

// The heading of a rig (columns = its axes in game view space) from the game camera's, about `up`
// (in view space): radians, + = left. 0 when a heading is undefined.
float RigYaw(const Mat33& rig, const Vec3& up);
// The yaw of a headset orientation (x, y, z, w; OpenXR: Y up, -Z forward) about its Y: radians, + =
// left.
float HeadYaw(const std::array<float, 4>& orientation);
// The angle to turn the main stick by (radians, + = counterclockwise: up turns to the left), or
// nullopt when the anchor does not turn it. `head`: the headset's orientation, null if unknown.
std::optional<float> StickTurn(const Anchor& anchor, const std::array<float, 4>* head);
// Turns a stick position (x right, y up) by `radians` (+ = counterclockwise).
std::array<float, 2> TurnStick(float x, float y, float radians);
// TurnStick for the stick the game reads. The reference turns with the character (Facing; View and
// Head with Level), so a push back would turn him toward a back that turns with him, round and
// round. From the moment the stick points back (more than 120 degrees from up) until it comes
// back to the front half or within `dead_zone` of the centre, the reference keeps the heading in
// the world it had then: he turns around once and runs. With `steer_behind` the stick then steers
// as seen from behind him once turned (back runs ahead, back-left to his left); without, as seen
// from the held heading. (x, y): the stick from its centre; camera_yaw: Anchor::stick_camera_yaw.
std::array<float, 2> TurnStickHeld(float x, float y, float dead_zone, float radians,
                                   float camera_yaw, bool steer_behind, StickHold* hold);

std::optional<Rotation> ParseRotation(std::string_view name);
std::optional<Axis> ParseAxis(std::string_view name);
std::optional<CameraKind> ParseCameraKind(std::string_view name);
std::optional<Stick> ParseStick(std::string_view name);
}  // namespace VR::CameraAnchorMath
