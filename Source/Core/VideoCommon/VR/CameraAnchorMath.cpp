// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VR/CameraAnchorMath.h"

#include <algorithm>
#include <cmath>

namespace VR::CameraAnchorMath
{
namespace
{
constexpr float DEG_TO_RAD = 0.01745329252f;
constexpr Mat33 IDENTITY = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
// A head that moves farther than this between two updates (a respawn, a door) is not smoothed.
constexpr float SNAP_METERS = 2.0f;

float Dot(const Vec3& a, const Vec3& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float Length(const Vec3& v)
{
  return std::sqrt(Dot(v, v));
}

bool Normalize(Vec3* v)
{
  const float length = Length(*v);
  if (!(length > 1e-6f))
    return false;
  for (float& c : *v)
    c /= length;
  return true;
}

Vec3 GetColumn(const Mat33& m, int c)
{
  return {m[c], m[3 + c], m[6 + c]};
}

Mat33 FromColumns(const Vec3& x, const Vec3& y, const Vec3& z)
{
  return {x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
}

Vec3 Rotate(const Mat33& m, const Vec3& v)
{
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

float Determinant(const Mat33& m)
{
  return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
         m[2] * (m[3] * m[7] - m[4] * m[6]);
}

bool IsFiniteMat(const Mat33& m)
{
  return std::ranges::all_of(m, [](float v) { return std::isfinite(v); });
}

// The rig of a bone with its forward axis as -Z and its up axis as Y, turned by yaw about Y.
std::optional<Mat33> BoneBasis(const Mtx34& bone, const Options& options)
{
  Vec3 z = Column(bone, options.forward_axis);
  if (!Normalize(&z))
    return std::nullopt;
  z = {-z[0], -z[1], -z[2]};
  Vec3 y = Column(bone, options.bone_up_axis);
  const float along = Dot(y, z);
  y = {y[0] - z[0] * along, y[1] - z[1] * along, y[2] - z[2] * along};
  if (!Normalize(&y))
    return std::nullopt;
  const Vec3 x = Cross(y, z);
  const float yaw = options.yaw_deg * DEG_TO_RAD;
  const float c = std::cos(yaw), s = std::sin(yaw);
  return FromColumns({c * x[0] - s * z[0], c * x[1] - s * z[1], c * x[2] - s * z[2]}, y,
                     {s * x[0] + c * z[0], s * x[1] + c * z[1], s * x[2] + c * z[2]});
}

// Follows the game camera's heading about the world's up from update to update: its last turn,
// while it keeps turning (a game at 30 fps moves it every other field; a cut is no turn).
float TrackCameraTurn(const Mat33& view_rotation, const Vec3& up, State* state)
{
  constexpr float PI = 3.14159265359f;
  constexpr float CUT = 30.0f * DEG_TO_RAD;
  // The camera's axes in the world: the view rotation transposed.
  const Mat33 camera = {view_rotation[0], view_rotation[3], view_rotation[6],
                        view_rotation[1], view_rotation[4], view_rotation[7],
                        view_rotation[2], view_rotation[5], view_rotation[8]};
  const float heading = RigYaw(camera, up);
  if (state->camera_valid)
  {
    const float turn = std::remainder(heading - state->camera_heading, 2.0f * PI);
    if (std::abs(turn) > 1e-5f)
    {
      state->camera_turn = std::abs(turn) < CUT ? turn : 0.0f;
      state->camera_still = 0;
    }
    else if (++state->camera_still >= 3)
    {
      state->camera_turn = 0.0f;
    }
  }
  state->camera_valid = true;
  state->camera_heading = heading;
  return state->camera_turn;
}

// The rig in world space for Level, Fixed and Full; turn_deg turns a Fixed rig.
std::optional<Mat33> WorldRig(const Mtx34& heading_bone, const Options& options, State* state,
                              float turn_deg)
{
  const Vec3 up = AxisVector(options.up_axis);
  switch (options.rotation)
  {
  case Rotation::Level:
    return LevelBasis(Column(heading_bone, options.forward_axis), up, options.yaw_deg);
  case Rotation::Fixed:
    if (!state->fixed_valid)
    {
      // The heading the character has when the anchor engages, laid on the ground plane.
      const std::optional<Mat33> rig =
          LevelBasis(Column(heading_bone, options.forward_axis), up, 0);
      if (!rig)
        return std::nullopt;
      const Vec3 z = GetColumn(*rig, 2);
      state->fixed_forward = {-z[0], -z[1], -z[2]};
      state->fixed_valid = true;
    }
    if (turn_deg != 0.0f)
    {
      if (const std::optional<Mat33> turned = LevelBasis(state->fixed_forward, up, turn_deg))
      {
        const Vec3 z = GetColumn(*turned, 2);
        state->fixed_forward = {-z[0], -z[1], -z[2]};
      }
    }
    return LevelBasis(state->fixed_forward, up, options.yaw_deg);
  case Rotation::Full:
    return BoneBasis(heading_bone, options);
  default:
    return std::nullopt;
  }
}
}  // namespace

bool IsFinite(const Mtx34& m)
{
  return std::ranges::all_of(m, [](float v) { return std::isfinite(v); });
}

bool IsPlausibleTransform(const Mtx34& m)
{
  if (!IsFinite(m))
    return false;
  for (int r = 0; r < 3; ++r)
  {
    const float length = Length({m[r * 4], m[r * 4 + 1], m[r * 4 + 2]});
    if (length < 0.001f || length > 10000.0f || std::abs(m[r * 4 + 3]) > 1e7f)
      return false;
  }
  return std::abs(Determinant(Basis(m))) > 1e-9f;
}

std::optional<Mtx34> Invert(const Mtx34& m)
{
  const Mat33 b = Basis(m);
  const float det = Determinant(b);
  if (!std::isfinite(det) || std::abs(det) < 1e-12f)
    return std::nullopt;
  const float inv = 1.0f / det;
  const Mat33 i = {(b[4] * b[8] - b[5] * b[7]) * inv, (b[2] * b[7] - b[1] * b[8]) * inv,
                   (b[1] * b[5] - b[2] * b[4]) * inv, (b[5] * b[6] - b[3] * b[8]) * inv,
                   (b[0] * b[8] - b[2] * b[6]) * inv, (b[2] * b[3] - b[0] * b[5]) * inv,
                   (b[3] * b[7] - b[4] * b[6]) * inv, (b[1] * b[6] - b[0] * b[7]) * inv,
                   (b[0] * b[4] - b[1] * b[3]) * inv};
  const Vec3 t = Rotate(i, Translation(m));
  return Mtx34{i[0], i[1], i[2], -t[0], i[3], i[4], i[5], -t[1], i[6], i[7], i[8], -t[2]};
}

Mtx34 Multiply(const Mtx34& a, const Mtx34& b)
{
  Mtx34 out{};
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 4; ++c)
    {
      float value = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];
      if (c == 3)
        value += a[r * 4 + 3];
      out[r * 4 + c] = value;
    }
  }
  return out;
}

Mat33 Multiply33(const Mat33& a, const Mat33& b)
{
  Mat33 out{};
  for (int r = 0; r < 3; ++r)
  {
    for (int c = 0; c < 3; ++c)
      out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
  }
  return out;
}

Vec3 TransformPoint(const Mtx34& m, const Vec3& p)
{
  return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
          m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
          m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

Mat33 Basis(const Mtx34& m)
{
  return {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
}

Vec3 Translation(const Mtx34& m)
{
  return {m[3], m[7], m[11]};
}

std::optional<Mtx34> ViewFromCamera(const Mtx34& camera, CameraKind kind)
{
  if (!IsPlausibleTransform(camera))
    return std::nullopt;
  if (kind == CameraKind::View)
    return camera;
  return Invert(camera);
}

Vec3 AxisVector(Axis axis)
{
  switch (axis)
  {
  case Axis::X:
    return {1.0f, 0.0f, 0.0f};
  case Axis::Y:
    return {0.0f, 1.0f, 0.0f};
  case Axis::Z:
    return {0.0f, 0.0f, 1.0f};
  case Axis::NegX:
    return {-1.0f, 0.0f, 0.0f};
  case Axis::NegY:
    return {0.0f, -1.0f, 0.0f};
  case Axis::NegZ:
  default:
    return {0.0f, 0.0f, -1.0f};
  }
}

Vec3 Column(const Mtx34& m, Axis axis)
{
  const int c = static_cast<int>(axis) % 3;
  const float sign = static_cast<int>(axis) >= 3 ? -1.0f : 1.0f;
  return {m[c] * sign, m[4 + c] * sign, m[8 + c] * sign};
}

std::optional<Mat33> LevelBasis(const Vec3& forward, const Vec3& up, float yaw_deg)
{
  Vec3 u = up;
  if (!Normalize(&u))
    return std::nullopt;
  const float along = Dot(forward, u);
  Vec3 f = {forward[0] - u[0] * along, forward[1] - u[1] * along, forward[2] - u[2] * along};
  // Under about 0.5 degree from the up axis there is no heading.
  if (Length(f) < 0.01f * Length(forward) || !Normalize(&f))
    return std::nullopt;
  // Rodrigues about u (f is perpendicular to it): + turns left.
  const float yaw = yaw_deg * DEG_TO_RAD;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const Vec3 side = Cross(u, f);
  f = {f[0] * c + side[0] * s, f[1] * c + side[1] * s, f[2] * c + side[2] * s};
  const Vec3 z = {-f[0], -f[1], -f[2]};
  return FromColumns(Cross(u, z), u, z);
}

bool Orthonormalize(Mat33* m)
{
  Vec3 y = GetColumn(*m, 1);
  if (!Normalize(&y))
    return false;
  Vec3 x = GetColumn(*m, 0);
  const float along = Dot(x, y);
  x = {x[0] - y[0] * along, x[1] - y[1] * along, x[2] - y[2] * along};
  if (!Normalize(&x))
    return false;
  *m = FromColumns(x, y, Cross(x, y));
  return true;
}

std::optional<Anchor> Compose(const Mtx34& head, const Mtx34* heading, const Mtx34& view,
                              const Options& options, float upm, State* state, float turn_deg)
{
  if (!IsPlausibleTransform(head) || !IsPlausibleTransform(view) ||
      (heading != nullptr && !IsPlausibleTransform(*heading)))
  {
    return std::nullopt;
  }
  const Mtx34& heading_bone = heading != nullptr ? *heading : head;
  const float scale = std::max(upm, 0.0001f);
  const float position_keep = std::clamp(options.smoothing, 0.0f, 0.95f);
  const float rotation_keep = std::clamp(options.heading_smoothing, 0.0f, 0.95f);
  const Mat33 view_rotation = Basis(view);
  const Vec3 head_position = Translation(head);

  // Position, in world space.
  const Vec3 moved = {head_position[0] - state->position[0], head_position[1] - state->position[1],
                      head_position[2] - state->position[2]};
  const bool snap = !state->valid || Length(moved) > SNAP_METERS * scale;
  Vec3 position = head_position;
  if (!snap)
  {
    for (int i = 0; i < 3; ++i)
      position[i] = position_keep * state->position[i] + (1.0f - position_keep) * head_position[i];
  }
  const Vec3 offset = {options.offset_m[0] * scale, options.offset_m[1] * scale,
                       -options.offset_m[2] * scale};

  Anchor anchor;
  if (options.rotation == Rotation::Camera || options.rotation == Rotation::Yaw)
  {
    // In the game camera's view: its turns move the rig with it.
    anchor.rotation = IDENTITY;
    if (options.rotation == Rotation::Yaw)
    {
      const Vec3 h = Rotate(view_rotation, Column(heading_bone, options.forward_axis));
      if (h[0] * h[0] + h[2] * h[2] < 1e-8f)
        return std::nullopt;
      const float yaw = std::atan2(-h[0], -h[2]) + options.yaw_deg * DEG_TO_RAD;
      const float c = std::cos(yaw), s = std::sin(yaw);
      anchor.rotation = {c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c};
    }
    const Vec3 in_view = TransformPoint(view, position);
    anchor.position = {in_view[0] + offset[0], in_view[1] + offset[1], in_view[2] + offset[2]};
  }
  else
  {
    std::optional<Mat33> rig = WorldRig(heading_bone, options, state, turn_deg);
    if (!rig)
    {
      // The forward axis points straight up for a moment: keep the last heading.
      if (!state->valid)
        return std::nullopt;
      rig = state->rotation;
    }
    else if (!snap)
    {
      Mat33 blended;
      for (size_t i = 0; i < blended.size(); ++i)
        blended[i] = rotation_keep * state->rotation[i] + (1.0f - rotation_keep) * (*rig)[i];
      // Through a half turn the blend degenerates: take the new rig.
      if (Orthonormalize(&blended))
        rig = blended;
    }
    state->rotation = *rig;
    const Vec3 eye = Rotate(*rig, offset);
    anchor.position =
        TransformPoint(view, {position[0] + eye[0], position[1] + eye[1], position[2] + eye[2]});
    anchor.rotation = Multiply33(view_rotation, *rig);
  }
  if (!IsFiniteMat(anchor.rotation) ||
      !std::ranges::all_of(anchor.position, [](float v) { return std::isfinite(v); }))
  {
    return std::nullopt;
  }
  state->position = position;
  state->valid = true;
  anchor.hide_center = TransformPoint(view, head_position);
  anchor.hide_radius = std::max(options.hide_radius, 0.0f);
  anchor.hide_below = options.hide_below;
  Vec3 up = Rotate(view_rotation, AxisVector(options.up_axis));
  if (Normalize(&up))
    anchor.hide_up = up;
  // The stick: up walks along the character's facing, or the rig as it is shown (smoothed), from
  // the camera the game will move him with (stick_lead updates ahead of this one).
  const float camera_turn = TrackCameraTurn(view_rotation, AxisVector(options.up_axis), state);
  Mat33 stick_rig = anchor.rotation;
  if (options.stick == Stick::Facing)
  {
    const std::optional<Mat33> facing = LevelBasis(Column(heading_bone, options.forward_axis),
                                                   AxisVector(options.up_axis), options.yaw_deg);
    if (facing)
      stick_rig = Multiply33(view_rotation, *facing);
  }
  anchor.stick_turn = options.stick != Stick::Camera;
  anchor.stick_yaw = RigYaw(stick_rig, anchor.hide_up) - options.stick_lead * camera_turn;
  anchor.stick_head = options.stick == Stick::Head ||
                      (options.stick == Stick::View && options.rotation == Rotation::Fixed);
  anchor.stick_camera_yaw = state->camera_heading + options.stick_lead * camera_turn;
  anchor.stick_hold = options.stick == Stick::Facing || options.rotation != Rotation::Fixed;
  anchor.stick_steer_behind =
      options.rotation == Rotation::Level || options.rotation == Rotation::Full;
  anchor.stick_turn_speed =
      options.rotation == Rotation::Fixed ? std::max(options.stick_turn_speed, 0.0f) : 0.0f;
  return anchor;
}

bool IsInHideRegion(const Anchor& anchor, const Vec3& point)
{
  if (!(anchor.hide_radius > 0.0f))
    return false;
  const Vec3 d = {point[0] - anchor.hide_center[0], point[1] - anchor.hide_center[1],
                  point[2] - anchor.hide_center[2]};
  if (!(Dot(d, d) <= anchor.hide_radius * anchor.hide_radius))
    return false;
  return anchor.hide_below < 0.0f || Dot(d, anchor.hide_up) >= -anchor.hide_below;
}

float RigYaw(const Mat33& rig, const Vec3& up)
{
  Vec3 u = up;
  if (!Normalize(&u))
    return 0.0f;
  // A direction's heading on the plane normal to u, or nothing when it is (nearly) along u.
  const auto flatten = [&u](const Vec3& v) -> std::optional<Vec3> {
    const float along = Dot(v, u);
    Vec3 flat = {v[0] - u[0] * along, v[1] - u[1] * along, v[2] - u[2] * along};
    if (Length(flat) < 0.1f * Length(v) || !Normalize(&flat))
      return std::nullopt;
    return flat;
  };
  // The camera looks along -Z. Looking straight down, its up (+Y) points ahead on the ground;
  // straight up, its down does.
  std::optional<Vec3> camera = flatten({0.0f, 0.0f, -1.0f});
  if (!camera)
    camera = flatten(u[2] > 0.0f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, -1.0f, 0.0f});
  const Vec3 z = GetColumn(rig, 2);
  std::optional<Vec3> forward = flatten({-z[0], -z[1], -z[2]});
  if (!forward)
  {
    const Vec3 y = GetColumn(rig, 1);
    const float down = Dot(z, u) > 0.0f ? 1.0f : -1.0f;
    forward = flatten({y[0] * down, y[1] * down, y[2] * down});
  }
  if (!camera || !forward)
    return 0.0f;
  return std::atan2(Dot(Cross(*camera, *forward), u), Dot(*camera, *forward));
}

float HeadYaw(const std::array<float, 4>& orientation)
{
  const auto [x, y, z, w] = orientation;
  // The rotation's columns: the headset's Y (up) and Z (back) axes.
  const Vec3 up = {2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x)};
  const Vec3 back = {2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)};
  return RigYaw(FromColumns(Cross(up, back), up, back), {0.0f, 1.0f, 0.0f});
}

std::optional<float> StickTurn(const Anchor& anchor, const std::array<float, 4>* head)
{
  if (!anchor.stick_turn)
    return std::nullopt;
  float yaw = anchor.stick_yaw;
  if (anchor.stick_head && head != nullptr)
    yaw += HeadYaw(*head);
  if (!std::isfinite(yaw))
    return std::nullopt;
  return std::remainder(yaw, 6.28318530718f);
}

std::array<float, 2> TurnStick(float x, float y, float radians)
{
  const float c = std::cos(radians), s = std::sin(radians);
  return {x * c - y * s, x * s + y * c};
}

std::array<float, 2> TurnStickHeld(float x, float y, float dead_zone, float radians,
                                   float camera_yaw, bool steer_behind, StickHold* hold)
{
  const float length = std::hypot(x, y);
  if (length <= dead_zone || y >= 0.0f)
    hold->held = false;
  else if (!hold->held && y < -0.5f * length)
    *hold = {true, radians + camera_yaw};
  if (!hold->held)
    return TurnStick(x, y, radians);
  const float held = std::remainder(hold->heading - camera_yaw, 6.28318530718f);
  // From behind him, turned around: the held reference turned by half a turn, the stick's y
  // flipped (back runs toward the held back).
  return steer_behind ? TurnStick(-x, y, held) : TurnStick(x, y, held);
}

std::optional<Rotation> ParseRotation(std::string_view name)
{
  if (name == "level")
    return Rotation::Level;
  if (name == "fixed")
    return Rotation::Fixed;
  if (name == "full")
    return Rotation::Full;
  if (name == "camera")
    return Rotation::Camera;
  if (name == "yaw")
    return Rotation::Yaw;
  return std::nullopt;
}

std::optional<Axis> ParseAxis(std::string_view name)
{
  if (name == "x")
    return Axis::X;
  if (name == "y")
    return Axis::Y;
  if (name == "z")
    return Axis::Z;
  if (name == "-x")
    return Axis::NegX;
  if (name == "-y")
    return Axis::NegY;
  if (name == "-z")
    return Axis::NegZ;
  return std::nullopt;
}

std::optional<Stick> ParseStick(std::string_view name)
{
  if (name == "camera")
    return Stick::Camera;
  if (name == "facing")
    return Stick::Facing;
  if (name == "view")
    return Stick::View;
  if (name == "head")
    return Stick::Head;
  return std::nullopt;
}

std::optional<CameraKind> ParseCameraKind(std::string_view name)
{
  if (name == "view")
    return CameraKind::View;
  if (name == "world")
    return CameraKind::World;
  return std::nullopt;
}
}  // namespace VR::CameraAnchorMath
