-- Metroid Prime 3: Corruption (NTSC-U, Wii): the arm cannon follows a VR controller.
--
-- A DolphinXR VR game patch, after PrimedGun's arm cannon tracking for Metroid Prime (PrimedGun,
-- created by Nobbie, GPL-2.0-or-later). The state manager address and the player offset come
-- from PrimeHack's RM3E01 tables (shiiion/dolphin), which also pointed at the cannon's transform
-- updater. The camera and gun chains, the hook point and the cave are this patch's own.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Arm cannon follows the controller",
  author = "DolphinXR, after PrimedGun (Nobbie), with PrimeHack's addresses",
  version = "1.0",
  description = "The arm cannon is aimed and placed by your VR controller, and shots and missiles "
    .. "leave it where it points. For Metroid Prime 3: Corruption NTSC-U (Wii).",
  games = {"RM3E01"},
  revision = 0,
  scratch = 0x200,
  -- A Controller Anchor of the cannon draws in the profile (off by default) would move it twice.
  conflicts = {elements = {"Element Draw 111"}},
  settings = {
    {name = "hand", label = "Cannon hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "scale", label = "Hand movement scale", default = 1, min = 0.5, max = 2, step = 0.05,
     description = "Times the profile's Units per Meter: above 1 the cannon moves farther than "
       .. "the hand."},
    {name = "pitch", label = "Pitch offset", unit = "deg", default = 0, min = -90, max = 90,
     step = 1},
    {name = "yaw", label = "Yaw offset", unit = "deg", default = 0, min = -90, max = 90, step = 1},
    {name = "roll", label = "Roll offset", unit = "deg", default = 0, min = -90, max = 90,
     step = 1},
    {name = "offset_x", label = "Right offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "offset_y", label = "Up offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "offset_z", label = "Back offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "model_x", label = "Model right offset", unit = "m", default = 0, min = -0.5,
     max = 0.5, step = 0.01},
    {name = "model_y", label = "Model forward offset", unit = "m", default = 0, min = -1,
     max = 0.5, step = 0.01},
    {name = "model_z", label = "Model up offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "smoothing", label = "Smoothing", default = 0.24, min = 0, max = 0.9, step = 0.01,
     description = "Part of the previous pose kept each frame (PrimedGun: 0.24)."},
    {name = "test_pitch", label = "Test pose", unit = "deg", default = 0, min = -60, max = 60,
     step = 5,
     description = "Not 0: the cannon is held at this pitch in front of the camera instead of "
       .. "following the controller, to check the patch without a headset."},
  },
}

-- ---- The game (Metroid Prime 3: Corruption NTSC-U) ----

local GAME = 0x805C4F70                -- a static object of the game
local STATE = GAME + 0x28              -- -> the state manager (PrimeHack: state_manager)
local STATE_PLAYER = 0x2184            -- state manager -> CPlayer*
local PLAYER_BALL_STATE = 0x358        -- 0: not morphed
local PLAYER_GUN = 0x3AD8              -- -> the player's gun
-- The active camera: [[[GAME + 0x38] + 0x34] + 0x20], its transform at +0x3C (the chain the game
-- follows to copy the camera into the gun).
local CAMERA_CHAIN = {GAME + 0x38, 0x34, 0x20}
local ACTOR_XF = 0x3C
local GUN_XF_HOLDER = 0x4D8            -- r28 in the cannon updater; the transform is at +0x4C
local BASE_MODEL_FORWARD = -0.080      -- PrimedGun's model offset, meters

-- The cannon updater (zz_80186bd8_, r28 = gun + 0x4D8) ends every aim mode at 8018748C: it puts
-- the cannon position (sp + 0x208) into the finished matrix (sp + 0x2D8) and copies the matrix
-- to r28 + 0x4C.
local HOOK = 0x8018748C
local HOOK_ORIGINAL = 0xC0010208       -- lfs f0, 0x208(r1)
local MATRIX = 0x2D8                   -- stack offsets
local POSITION = 0x208

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local BASIS = S + 0x00    -- 3x3 row-major: the hand's rotation in the camera's axes
local OFFSET = S + 0x24   -- 3 floats: the hand's position from the camera, its axes, game units
local GUARD = S + 0x30    -- gun + 0x4D8 (r28) when the pose is for that gun; 0 = not tracked
local CAMERA = S + 0x34   -- the camera's transform (3x4)
local CAVE = S + 0x40

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- When the gun is tracked: rotation = the camera's rotation * BASIS, position = the camera's
-- position + its rotation * OFFSET, written where the game is about to take them; then the
-- replaced instruction.
local function cave_words()
  local t = {load_address("r11", S), "lwz r12, 0x30(r11)\ncmplw r12, r28\nbne done\n",
             "lwz r12, 0x34(r11)\n"}
  for i = 0, 8 do
    t[#t + 1] = string.format("lfs f%d, 0x%X(r11)\n", i + 1, i * 4)
  end
  for row = 0, 2 do
    local o = row * 0x10
    t[#t + 1] = string.format("lfs f10, 0x%X(r12)\nlfs f11, 0x%X(r12)\nlfs f12, 0x%X(r12)\n",
                              o, o + 4, o + 8)
    for col = 0, 2 do
      t[#t + 1] = string.format(
        "fmuls f13, f10, f%d\nfmadds f13, f11, f%d, f13\nfmadds f13, f12, f%d, f13\n" ..
        "stfs f13, 0x%X(r1)\n", col + 1, col + 4, col + 7, MATRIX + o + col * 4)
    end
  end
  t[#t + 1] = "lfs f1, 0x24(r11)\nlfs f2, 0x28(r11)\nlfs f3, 0x2C(r11)\n"
  for row = 0, 2 do
    local o = row * 0x10
    t[#t + 1] = string.format(
      "lfs f10, 0x%X(r12)\nlfs f11, 0x%X(r12)\nlfs f12, 0x%X(r12)\nlfs f13, 0x%X(r12)\n" ..
      "fmadds f13, f10, f1, f13\nfmadds f13, f11, f2, f13\nfmadds f13, f12, f3, f13\n" ..
      "stfs f13, 0x%X(r1)\n", o, o + 4, o + 8, o + 0xC, POSITION + row * 4)
  end
  t[#t + 1] = string.format("done:\nlfs f0, 0x%X(r1)\nb `0x%X`\n", POSITION, HOOK + 4)
  local words, err = asm.assemble(table.concat(t), CAVE)
  if not words then error("assembling the cave: " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hook ----

local cave = nil
local installed = false
local warned = false

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

local function install()
  local current = mem.u32(HOOK)
  if current ~= HOOK_ORIGINAL and not branch_target(HOOK, current or 0) then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(HOOK) .. " (" .. mem.hex(current) ..
           "): is this Metroid Prime 3 NTSC-U?")
    end
    warned = true
    installed = false
    return
  end
  cave = cave or cave_words()
  local changed = mem.ensure_code(CAVE, cave)
  changed = mem.ensure_code(HOOK, asm.branch(HOOK, CAVE)) or changed
  if changed then log("hook installed, cave at", mem.hex(CAVE)) end
  installed = true
end

local function uninstall()
  mem.write_u32(GUARD, 0)
  installed = false
  if branch_target(HOOK, mem.u32(HOOK) or 0) then mem.write_code(HOOK, HOOK_ORIGINAL) end
end

-- ---- Tracking ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local sin, cos, rad = math.sin, math.cos, math.rad
local raw_pose, write_f32s = vr.raw_pose, mem.write_f32s

-- The Hamilton product a * b of two quaternions (x, y, z, w).
local function qmul(ax, ay, az, aw, bx, by, bz, bw)
  return aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
         aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz
end

-- PrimedGun's smoothing of the hand pose: the basis (row-major) then the offset, in place. Each
-- field keeps settings.smoothing of the previous pose, unless the offset jumped by more than SNAP
-- game units.
local SNAP = 2.0
local target = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
local smoothed = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
local has_smoothed = false

local function smooth(keep)
  local t, s = target, smoothed
  local dx, dy, dz = t[10] - s[10], t[11] - s[11], t[12] - s[12]
  if not has_smoothed or keep <= 0 or dx * dx + dy * dy + dz * dz > SNAP * SNAP then
    for i = 1, 12 do s[i] = t[i] end
  else
    for i = 1, 12 do s[i] = s[i] * keep + t[i] * (1 - keep) end
  end
  has_smoothed = true
end

local tracking = false

local function stop_tracking()
  if tracking then
    mem.write_u32(GUARD, 0)
    has_smoothed = false
    tracking = false
  end
end

-- The controller's pose seen from the game camera (OpenXR view space: +X right, +Y up,
-- -Z forward, meters), or the test pose.
local function hand_pose()
  if settings.test_pitch ~= 0 then
    local h = rad(settings.test_pitch) * 0.5
    return true, 0.2, -0.25, -0.35, sin(h), 0, 0, cos(h)
  end
  return raw_pose(settings.hand, "view")
end

local function track()
  local ok, px, py, pz, qx, qy, qz, qw = hand_pose()
  local player = mem.ptr(STATE, STATE_PLAYER)
  if not ok or not player or mem.u32(player + PLAYER_BALL_STATE) ~= 0 then
    stop_tracking()
    return
  end
  local gun = mem.ptr(player + PLAYER_GUN)
  local camera = mem.ptr(CAMERA_CHAIN[1], CAMERA_CHAIN[2], CAMERA_CHAIN[3])
  if not gun or not camera then
    stop_tracking()
    return
  end

  -- The rotation with the offsets (yaw, pitch, roll in the hand's axes), from OpenXR's view axes
  -- (+X right, +Y up, -Z forward) to the game camera's (+X right, +Y forward, +Z up).
  local h = rad(settings.yaw) * 0.5
  local x, y, z, w = qmul(qx, qy, qz, qw, 0, sin(h), 0, cos(h))
  h = rad(settings.pitch) * 0.5
  x, y, z, w = qmul(x, y, z, w, sin(h), 0, 0, cos(h))
  h = rad(settings.roll) * 0.5
  x, y, z, w = qmul(x, y, z, w, 0, 0, sin(h), cos(h))
  local t = target
  t[1], t[2], t[3] = 1 - 2 * (y * y + z * z), -2 * (x * z + y * w), 2 * (x * y - z * w)
  t[4], t[5], t[6] = -2 * (x * z - y * w), 1 - 2 * (x * x + y * y), -2 * (y * z + x * w)
  t[7], t[8], t[9] = 2 * (x * y + z * w), -2 * (y * z - x * w), 1 - 2 * (x * x + z * z)
  -- The position in the camera's axes, game units, with the calibration offsets (meters: x right,
  -- y up, z back).
  local scale = vr.units_per_meter() * settings.scale
  t[10] = (px + settings.offset_x) * scale
  t[11] = -(pz + settings.offset_z) * scale
  t[12] = (py + settings.offset_y) * scale
  smooth(settings.smoothing)

  local s = smoothed
  local b00, b01, b02, b10, b11, b12, b20, b21, b22 = s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                                                      s[8], s[9]
  -- The left hand mirrors the cannon.
  if settings.hand == 0 then b00, b10, b20 = -b00, -b10, -b20 end
  -- The cannon's origin: the hand, plus the model offset along the cannon.
  local mx, my, mz = settings.model_x * scale, (BASE_MODEL_FORWARD + settings.model_y) * scale,
                     settings.model_z * scale
  if not write_f32s(BASIS, b00, b01, b02, b10, b11, b12, b20, b21, b22) or
     not write_f32s(OFFSET, s[10] + b00 * mx + b01 * my + b02 * mz,
                    s[11] + b10 * mx + b11 * my + b12 * mz,
                    s[12] + b20 * mx + b21 * my + b22 * mz) or
     not mem.write_u32(CAMERA, camera + ACTOR_XF) then
    stop_tracking()
    return
  end
  -- Last: arms the cave for this gun.
  mem.write_u32(GUARD, gun + GUN_XF_HOLDER)
  tracking = true
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(GUARD, 0)
  install()
end

function on_state_loaded()
  mem.write_u32(GUARD, 0)
  tracking = false
  has_smoothed = false
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then install() end
  if not installed then
    stop_tracking()
    return
  end
  track()
end

function on_unload()
  uninstall()
end
