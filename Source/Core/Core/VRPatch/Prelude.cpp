// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Lua half of the VR patch API: math types and the wrappers over the raw bindings. Kept as
// several raw string pieces (MSVC limits one literal to 16 KB) joined once at startup.

#include <string>
#include <string_view>

#include "Core/VRPatch/ScriptContext.h"

namespace VRPatch
{
namespace
{
constexpr std::string_view MATH_VEC_QUAT = R"LUA(
local sqrt, sin, cos, atan, asin, abs = math.sqrt, math.sin, math.cos, math.atan, math.asin, math.abs
local pi = math.pi
local DEG, RAD = 180 / pi, pi / 180
local setmetatable, getmetatable, type, error, select = setmetatable, getmetatable, type, error, select
local fmt = string.format

function clamp(x, lo, hi)
  if x < lo then return lo end
  if x > hi then return hi end
  return x
end

function lerp(a, b, t)
  return a + (b - a) * t
end

-- An angle in degrees brought to [-180, 180).
function wrap180(degrees)
  return (degrees + 180) % 360 - 180
end

-- ---- vec3 ----

local V = {}
V.__index = V
local function new_vec3(x, y, z)
  return setmetatable({x = x, y = y, z = z}, V)
end

vec3 = setmetatable({}, {__call = function(_, x, y, z)
  if type(x) == "table" then
    return new_vec3(x.x or x[1] or 0, x.y or x[2] or 0, x.z or x[3] or 0)
  end
  return new_vec3(x or 0, y or 0, z or 0)
end})
vec3.new = new_vec3
function vec3.is(v) return getmetatable(v) == V end
function vec3.zero() return new_vec3(0, 0, 0) end
function vec3.lerp(a, b, t)
  return new_vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t)
end

function V.__add(a, b) return new_vec3(a.x + b.x, a.y + b.y, a.z + b.z) end
function V.__sub(a, b) return new_vec3(a.x - b.x, a.y - b.y, a.z - b.z) end
function V.__unm(a) return new_vec3(-a.x, -a.y, -a.z) end
function V.__mul(a, b)
  if type(a) == "number" then return new_vec3(a * b.x, a * b.y, a * b.z) end
  if type(b) == "number" then return new_vec3(a.x * b, a.y * b, a.z * b) end
  return new_vec3(a.x * b.x, a.y * b.y, a.z * b.z)
end
function V.__div(a, s) return new_vec3(a.x / s, a.y / s, a.z / s) end
function V.__eq(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
function V.__tostring(a) return fmt("(%.4f, %.4f, %.4f)", a.x, a.y, a.z) end
function V:dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end
function V:cross(b)
  return new_vec3(self.y * b.z - self.z * b.y, self.z * b.x - self.x * b.z,
                  self.x * b.y - self.y * b.x)
end
function V:length() return sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end
function V:distance(b) return (self - b):length() end
function V:normalized()
  local l = self:length()
  if l == 0 then return new_vec3(0, 0, 0) end
  return new_vec3(self.x / l, self.y / l, self.z / l)
end
function V:copy() return new_vec3(self.x, self.y, self.z) end
function V:unpack() return self.x, self.y, self.z end

-- ---- quat (x, y, z, w; rotates column vectors: v' = q * v) ----

local Q = {}
Q.__index = Q
local function new_quat(x, y, z, w)
  return setmetatable({x = x, y = y, z = z, w = w}, Q)
end

quat = setmetatable({}, {__call = function(_, x, y, z, w)
  return new_quat(x or 0, y or 0, z or 0, w or 1)
end})
quat.new = new_quat
function quat.is(q) return getmetatable(q) == Q end
function quat.identity() return new_quat(0, 0, 0, 1) end
-- Rotation of `degrees` about `axis` (a vec3, normalized here), right-handed.
function quat.axis_angle(axis, degrees)
  local a = axis:normalized()
  local h = degrees * RAD * 0.5
  local s = sin(h)
  return new_quat(a.x * s, a.y * s, a.z * s, cos(h))
end
function quat.axis_x(degrees) return quat.axis_angle(new_vec3(1, 0, 0), degrees) end
function quat.axis_y(degrees) return quat.axis_angle(new_vec3(0, 1, 0), degrees) end
function quat.axis_z(degrees) return quat.axis_angle(new_vec3(0, 0, 1), degrees) end
-- Rotations in degrees about X (pitch), Y (yaw) and Z (roll), applied in the given order
-- (default "xyz": X first, then Y, then Z, each about the fixed axes).
function quat.from_euler(x, y, z, order)
  order = order or "xyz"
  local parts = {x = quat.axis_x(x or 0), y = quat.axis_y(y or 0), z = quat.axis_z(z or 0)}
  local q = quat.identity()
  for i = 1, 3 do
    q = parts[order:sub(i, i)] * q
  end
  return q
end
function quat.slerp(a, b, t)
  local d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
  if d < 0 then
    b = new_quat(-b.x, -b.y, -b.z, -b.w)
    d = -d
  end
  if d > 0.9995 then
    return new_quat(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t):normalized()
  end
  local theta = math.acos(d)
  local s = sin(theta)
  local wa, wb = sin((1 - t) * theta) / s, sin(t * theta) / s
  return new_quat(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                  a.w * wa + b.w * wb)
end

function Q:rotate(v)
  local x, y, z, w = self.x, self.y, self.z, self.w
  local tx = 2 * (y * v.z - z * v.y)
  local ty = 2 * (z * v.x - x * v.z)
  local tz = 2 * (x * v.y - y * v.x)
  return new_vec3(v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz),
                  v.z + w * tz + (x * ty - y * tx))
end
function Q.__mul(a, b)
  if getmetatable(b) == V then return a:rotate(b) end
  return new_quat(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                  a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                  a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
end
function Q.__unm(a) return new_quat(-a.x, -a.y, -a.z, -a.w) end
function Q.__tostring(q) return fmt("quat(%.4f, %.4f, %.4f, %.4f)", q.x, q.y, q.z, q.w) end
function Q:length() return sqrt(self.x * self.x + self.y * self.y + self.z * self.z + self.w * self.w) end
function Q:normalized()
  local l = self:length()
  if l == 0 then return new_quat(0, 0, 0, 1) end
  return new_quat(self.x / l, self.y / l, self.z / l, self.w / l)
end
function Q:conjugate() return new_quat(-self.x, -self.y, -self.z, self.w) end
function Q:inverse()
  local n = self.x * self.x + self.y * self.y + self.z * self.z + self.w * self.w
  if n == 0 then return new_quat(0, 0, 0, 1) end
  return new_quat(-self.x / n, -self.y / n, -self.z / n, self.w / n)
end
function Q:dot(b) return self.x * b.x + self.y * b.y + self.z * b.z + self.w * b.w end
-- Angle in degrees between two rotations.
function Q:angle_to(b)
  local d = abs(self:normalized():dot(b:normalized()))
  if d > 1 then d = 1 end
  return 2 * math.acos(d) * DEG
end
function Q:unpack() return self.x, self.y, self.z, self.w end
-- Yaw (about +Y), pitch (about +X) and roll (about +Z) in degrees, for the usual OpenXR order
-- (yaw, then pitch, then roll in the rotated frame).
function Q:to_yaw_pitch_roll()
  local x, y, z, w = self.x, self.y, self.z, self.w
  local sinp = 2 * (w * x - y * z)
  local pitch
  if sinp >= 1 then pitch = 90 elseif sinp <= -1 then pitch = -90 else pitch = asin(sinp) * DEG end
  local yaw = atan(2 * (w * y + x * z), 1 - 2 * (x * x + y * y)) * DEG
  local roll = atan(2 * (w * z + x * y), 1 - 2 * (x * x + z * z)) * DEG
  return yaw, pitch, roll
end
)LUA";

constexpr std::string_view MATH_MATRICES = R"LUA(
local sqrt, atan, abs = math.sqrt, math.atan, math.abs
local setmetatable, getmetatable, type, error, select = setmetatable, getmetatable, type, error, select
local fmt = string.format
local new_vec3, new_quat = vec3.new, quat.new
local V = getmetatable(vec3.zero())

-- ---- mat33: row-major m[1..9], v' = M * v ----

local M3 = {}
M3.__index = M3
local function new_mat33(t)
  return setmetatable(t, M3)
end

mat33 = setmetatable({}, {__call = function(_, ...)
  if select("#", ...) == 1 and type((...)) == "table" then
    local t = ...
    return new_mat33({t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9]})
  end
  local a = {...}
  if #a ~= 9 then error("mat33 needs 9 numbers (row-major) or a table of them", 2) end
  return new_mat33(a)
end})
function mat33.is(m) return getmetatable(m) == M3 end
function mat33.identity() return new_mat33({1, 0, 0, 0, 1, 0, 0, 0, 1}) end
function mat33.from_rows(a, b, c)
  return new_mat33({a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z})
end
function mat33.from_columns(a, b, c)
  return new_mat33({a.x, b.x, c.x, a.y, b.y, c.y, a.z, b.z, c.z})
end
function mat33.from_quat(q)
  local x, y, z, w = q.x, q.y, q.z, q.w
  return new_mat33({
    1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
    2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
    2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)})
end

function M3:get(row, column) return self[row * 3 + column + 1] end
function M3:row(i) local b = i * 3 return new_vec3(self[b + 1], self[b + 2], self[b + 3]) end
function M3:column(j) return new_vec3(self[j + 1], self[j + 4], self[j + 7]) end
function M3:transpose()
  local m = self
  return new_mat33({m[1], m[4], m[7], m[2], m[5], m[8], m[3], m[6], m[9]})
end
function M3:determinant()
  local m = self
  return m[1] * (m[5] * m[9] - m[6] * m[8]) - m[2] * (m[4] * m[9] - m[6] * m[7]) +
         m[3] * (m[4] * m[8] - m[5] * m[7])
end
function M3:inverse()
  local m = self
  local d = self:determinant()
  if d == 0 then return nil end
  local i = 1 / d
  return new_mat33({
    (m[5] * m[9] - m[6] * m[8]) * i, (m[3] * m[8] - m[2] * m[9]) * i, (m[2] * m[6] - m[3] * m[5]) * i,
    (m[6] * m[7] - m[4] * m[9]) * i, (m[1] * m[9] - m[3] * m[7]) * i, (m[3] * m[4] - m[1] * m[6]) * i,
    (m[4] * m[8] - m[5] * m[7]) * i, (m[2] * m[7] - m[1] * m[8]) * i, (m[1] * m[5] - m[2] * m[4]) * i})
end
function M3:rotate(v)
  local m = self
  return new_vec3(m[1] * v.x + m[2] * v.y + m[3] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z,
                  m[7] * v.x + m[8] * v.y + m[9] * v.z)
end
function M3.__mul(a, b)
  if type(b) == "number" then
    local r = {}
    for i = 1, 9 do r[i] = a[i] * b end
    return new_mat33(r)
  end
  if getmetatable(b) == V then return a:rotate(b) end
  local r = {}
  for i = 0, 2 do
    for j = 0, 2 do
      r[i * 3 + j + 1] = a[i * 3 + 1] * b[j + 1] + a[i * 3 + 2] * b[j + 4] + a[i * 3 + 3] * b[j + 7]
    end
  end
  return new_mat33(r)
end
function M3:to_quat()
  local m = self
  local trace = m[1] + m[5] + m[9]
  local x, y, z, w
  if trace > 0 then
    local s = 0.5 / sqrt(trace + 1)
    w = 0.25 / s
    x = (m[8] - m[6]) * s
    y = (m[3] - m[7]) * s
    z = (m[4] - m[2]) * s
  elseif m[1] > m[5] and m[1] > m[9] then
    local s = 2 * sqrt(1 + m[1] - m[5] - m[9])
    w = (m[8] - m[6]) / s
    x = 0.25 * s
    y = (m[2] + m[4]) / s
    z = (m[3] + m[7]) / s
  elseif m[5] > m[9] then
    local s = 2 * sqrt(1 + m[5] - m[1] - m[9])
    w = (m[3] - m[7]) / s
    x = (m[2] + m[4]) / s
    y = 0.25 * s
    z = (m[6] + m[8]) / s
  else
    local s = 2 * sqrt(1 + m[9] - m[1] - m[5])
    w = (m[4] - m[2]) / s
    x = (m[3] + m[7]) / s
    y = (m[6] + m[8]) / s
    z = 0.25 * s
  end
  return new_quat(x, y, z, w):normalized()
end
-- Rows of length 1 +- tolerance and pairwise dot products within tolerance (default 0.1).
function M3:is_orthonormal(tolerance)
  tolerance = tolerance or 0.1
  local r0, r1, r2 = self:row(0), self:row(1), self:row(2)
  for _, r in ipairs({r0, r1, r2}) do
    local l = r:length()
    if l ~= l or abs(l - 1) > tolerance then return false end
  end
  return abs(r0:dot(r1)) <= tolerance and abs(r0:dot(r2)) <= tolerance and
         abs(r1:dot(r2)) <= tolerance
end
function M3:unpack() local m = self return m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9] end
function M3.__tostring(m)
  return fmt("mat33(%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f)", m:unpack())
end

-- ---- mat34: an affine transform, row-major m[1..12] = [r00 r01 r02 tx | r10 r11 r12 ty |
-- r20 r21 r22 tz] (the GameCube Mtx layout) ----

local M4 = {}
M4.__index = M4
local function new_mat34(t)
  return setmetatable(t, M4)
end

mat34 = setmetatable({}, {__call = function(_, ...)
  if select("#", ...) == 1 and type((...)) == "table" then
    local t = ...
    local r = {}
    for i = 1, 12 do r[i] = t[i] end
    return new_mat34(r)
  end
  local a = {...}
  if #a ~= 12 then error("mat34 needs 12 numbers (row-major) or a table of them", 2) end
  return new_mat34(a)
end})
function mat34.is(m) return getmetatable(m) == M4 end
function mat34.identity() return new_mat34({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}) end
function mat34.from_basis(b, t)
  t = t or new_vec3(0, 0, 0)
  return new_mat34({b[1], b[2], b[3], t.x, b[4], b[5], b[6], t.y, b[7], b[8], b[9], t.z})
end
function mat34.from_quat(q, t) return mat34.from_basis(mat33.from_quat(q), t) end
function mat34.translation_of(t) return mat34.from_basis(mat33.identity(), t) end

function M4:get(row, column) return self[row * 4 + column + 1] end
function M4:basis()
  local m = self
  return new_mat33({m[1], m[2], m[3], m[5], m[6], m[7], m[9], m[10], m[11]})
end
function M4:translation() return new_vec3(self[4], self[8], self[12]) end
function M4:with_translation(t)
  local m = self
  return new_mat34({m[1], m[2], m[3], t.x, m[5], m[6], m[7], t.y, m[9], m[10], m[11], t.z})
end
function M4:with_basis(b) return mat34.from_basis(b, self:translation()) end
function M4:row(i) local b = i * 4 return new_vec3(self[b + 1], self[b + 2], self[b + 3]) end
function M4:column(j) return new_vec3(self[j + 1], self[j + 5], self[j + 9]) end
function M4:rotate(v)
  local m = self
  return new_vec3(m[1] * v.x + m[2] * v.y + m[3] * v.z, m[5] * v.x + m[6] * v.y + m[7] * v.z,
                  m[9] * v.x + m[10] * v.y + m[11] * v.z)
end
function M4:transform(v)
  local m = self
  return new_vec3(m[1] * v.x + m[2] * v.y + m[3] * v.z + m[4],
                  m[5] * v.x + m[6] * v.y + m[7] * v.z + m[8],
                  m[9] * v.x + m[10] * v.y + m[11] * v.z + m[12])
end
function M4.__mul(a, b)
  if getmetatable(b) == V then return a:transform(b) end
  local r = {}
  for i = 0, 2 do
    local a0, a1, a2, a3 = a[i * 4 + 1], a[i * 4 + 2], a[i * 4 + 3], a[i * 4 + 4]
    r[i * 4 + 1] = a0 * b[1] + a1 * b[5] + a2 * b[9]
    r[i * 4 + 2] = a0 * b[2] + a1 * b[6] + a2 * b[10]
    r[i * 4 + 3] = a0 * b[3] + a1 * b[7] + a2 * b[11]
    r[i * 4 + 4] = a0 * b[4] + a1 * b[8] + a2 * b[12] + a3
  end
  return new_mat34(r)
end
function M4:inverse()
  local bi = self:basis():inverse()
  if not bi then return nil end
  local t = bi:rotate(self:translation())
  return mat34.from_basis(bi, new_vec3(-t.x, -t.y, -t.z))
end
-- PrimedGun's test for a live game transform: finite, rows of length 0.5..1.5, pairwise |dot| at
-- most 0.25.
function M4:looks_like_transform()
  for i = 1, 12 do
    local v = self[i]
    if v ~= v or v == math.huge or v == -math.huge then return false end
  end
  local r0, r1, r2 = self:row(0), self:row(1), self:row(2)
  for _, r in ipairs({r0, r1, r2}) do
    local l = r:length()
    if l < 0.5 or l > 1.5 then return false end
  end
  return abs(r0:dot(r1)) <= 0.25 and abs(r0:dot(r2)) <= 0.25 and abs(r1:dot(r2)) <= 0.25
end
function M4:unpack()
  local m = self
  return m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12]
end
function M4.__tostring(m)
  return fmt("mat34(%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f)", m:unpack())
end
)LUA";

constexpr std::string_view MATH_POSE_SMOOTH = R"LUA(
local setmetatable, getmetatable, type, abs = setmetatable, getmetatable, type, math.abs
local new_vec3, new_quat = vec3.new, quat.new
local V = getmetatable(vec3.zero())
local M4 = getmetatable(mat34.identity())

-- ---- Pose: a position (vec3) and a rotation (quat) ----

local P = {}
P.__index = P
local function new_pose(position, rotation)
  return setmetatable({pos = position, rot = rotation}, P)
end
pose = setmetatable({}, {__call = function(_, position, rotation)
  return new_pose(position or new_vec3(0, 0, 0), rotation or new_quat(0, 0, 0, 1))
end})
function pose.is(p) return getmetatable(p) == P end
function P:matrix() return mat34.from_quat(self.rot, self.pos) end
function P:inverse()
  local r = self.rot:conjugate()
  return new_pose(-(r:rotate(self.pos)), r)
end
function P.__mul(a, b)
  if getmetatable(b) == V then return a.pos + a.rot:rotate(b) end
  return new_pose(a.pos + a.rot:rotate(b.pos), a.rot * b.rot)
end
function P:transform(v) return self.pos + self.rot:rotate(v) end
-- OpenXR axes: -Z forward, +Y up, +X right.
function P:forward() return self.rot:rotate(new_vec3(0, 0, -1)) end
function P:up() return self.rot:rotate(new_vec3(0, 1, 0)) end
function P:right() return self.rot:rotate(new_vec3(1, 0, 0)) end
function P.__tostring(p) return "pose(" .. tostring(p.pos) .. ", " .. tostring(p.rot) .. ")" end

-- ---- Smoother: exponential smoothing of numbers, vec3, quat or mat34 ----
-- keep: the part of the previous value kept each update (0 = none, 0.24 = PrimedGun's cannon).
-- snap: a jump larger than this (in translation for vec3 and mat34) skips the smoothing.

local S = {}
S.__index = S
function smoother(keep, snap)
  return setmetatable({keep = keep or 0, snap = snap, last = nil}, S)
end
function S:reset() self.last = nil end
function S:update(value)
  local keep, last = self.keep, self.last
  local kind = type(value) == "number" and "number" or getmetatable(value)
  if last == nil or last.kind ~= kind or keep <= 0 then
    self.last = {kind = kind, value = value}
    return value
  end
  local old = last.value
  local result
  if kind == "number" then
    if self.snap and abs(value - old) > self.snap then result = value
    else result = old * keep + value * (1 - keep) end
  elseif kind == V then
    if self.snap and (value - old):length() > self.snap then result = value
    else result = vec3.lerp(value, old, keep) end
  elseif kind == M4 then
    if self.snap and (value:translation() - old:translation()):length() > self.snap then
      result = value
    else
      local r = {}
      for i = 1, 12 do r[i] = old[i] * keep + value[i] * (1 - keep) end
      result = mat34(r)
    end
  elseif quat.is(value) then
    result = quat.slerp(value, old, keep)
  else
    error("smoother:update takes a number, vec3, quat or mat34", 2)
  end
  self.last = {kind = kind, value = result}
  return result
end
)LUA";

constexpr std::string_view API = R"LUA(
local select, tostring, concat, type = select, tostring, table.concat, type
local raw_log = __vrpatch_log
__vrpatch_log = nil

local function join(...)
  local n = select("#", ...)
  local parts = {}
  for i = 1, n do parts[i] = tostring((select(i, ...))) end
  return concat(parts, " ")
end
function log(...) raw_log(0, join(...)) end
function warn(...) raw_log(1, join(...)) end
print = log

-- ---- vr ----
local raw_pose, raw_velocity = vr.raw_pose, vr.raw_velocity
local new_vec3, new_quat = vec3.new, quat.new

local function make_pose(ok, px, py, pz, qx, qy, qz, qw)
  if not ok then return nil end
  return pose(new_vec3(px, py, pz), new_quat(qx, qy, qz, qw))
end
function vr.head(kind) return make_pose(raw_pose("head", kind)) end
function vr.hand(hand, kind) return make_pose(raw_pose(hand, kind or "aim")) end
function vr.hand_in_head_space(hand, kind)
  local head, p = vr.head(), vr.hand(hand, kind)
  if not head or not p then return nil end
  return head:inverse() * p
end
function vr.velocity(hand)
  local lv, lx, ly, lz, av, ax, ay, az = raw_velocity(hand)
  return lv and new_vec3(lx, ly, lz) or nil, av and new_vec3(ax, ay, az) or nil
end

-- ---- mem ----
local f32s, write_f32s = mem.f32s, mem.write_f32s
function mem.vec3(address)
  local x, y, z = f32s(address, 3)
  if x == nil then return nil end
  return new_vec3(x, y, z)
end
function mem.write_vec3(address, v) return write_f32s(address, v.x, v.y, v.z) end
function mem.mat33(address)
  local t = {f32s(address, 9)}
  if t[1] == nil then return nil end
  return mat33(t)
end
function mem.write_mat33(address, m) return write_f32s(address, m:unpack()) end
function mem.mat34(address)
  local t = {f32s(address, 12)}
  if t[1] == nil then return nil end
  return mat34(t)
end
function mem.write_mat34(address, m) return write_f32s(address, m:unpack()) end
-- Writes {address, word} pairs (a list of code patches) with ensure_code; true when one changed.
function mem.ensure_patches(list)
  local changed = false
  for _, entry in ipairs(list) do
    local c = mem.ensure_code(entry[1], entry[2])
    if c == nil then return nil end
    changed = changed or c
  end
  return changed
end
function mem.hex(value)
  if value == nil then return "nil" end
  return string.format("%08X", value & 0xFFFFFFFF)
end
)LUA";

const std::string& JoinedMath()
{
  static const std::string text =
      std::string(MATH_VEC_QUAT) + std::string(MATH_MATRICES) + std::string(MATH_POSE_SMOOTH);
  return text;
}
}  // namespace

std::string_view GetPreludeMath()
{
  return JoinedMath();
}

std::string_view GetPreludeApi()
{
  return API;
}
}  // namespace VRPatch
