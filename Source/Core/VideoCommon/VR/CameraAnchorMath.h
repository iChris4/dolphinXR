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
// or the heading cannot be found (the forward axis points straight up) before any was.
std::optional<Anchor> Compose(const Mtx34& head, const Mtx34* heading, const Mtx34& view,
                              const Options& options, float upm, State* state);

// Whether a draw centred at `point` (game view space) is in the anchor's hide region: within its
// radius of the head bone, and at most hide_below under it along the world's up.
bool IsInHideRegion(const Anchor& anchor, const Vec3& point);

std::optional<Rotation> ParseRotation(std::string_view name);
std::optional<Axis> ParseAxis(std::string_view name);
std::optional<CameraKind> ParseCameraKind(std::string_view name);
}  // namespace VR::CameraAnchorMath
