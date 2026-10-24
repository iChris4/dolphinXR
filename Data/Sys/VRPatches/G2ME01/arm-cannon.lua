-- Metroid Prime 2: Echoes (NTSC-U): the arm cannon follows a VR controller.
--
-- A DolphinXR VR game patch, after PrimedGun's arm cannon tracking for Metroid Prime (PrimedGun,
-- created by Nobbie, GPL-2.0-or-later). The Echoes addresses (state manager, player, gun) come
-- from PrimeHack's G2ME01 tables and the PrimeDecomp echoes symbol map, as gathered by Primed2Gun
-- (gperkins345). The hook point and the cave are this patch's own.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Arm cannon follows the controller",
  author = "DolphinXR, after PrimedGun (Nobbie) and Primed2Gun (gperkins345)",
  version = "1.0",
  description = "The arm cannon is aimed and placed by your VR controller, and shots leave it "
    .. "where it points. For Metroid Prime 2: Echoes NTSC-U.",
  games = {"G2ME01"},
  revision = 0,
  scratch = 0x200,
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

-- ---- The game (Metroid Prime 2: Echoes NTSC-U) ----

local STATE_MANAGER = 0x803DB6E0
local PLAYER = STATE_MANAGER + 0x14FC  -- CPlayer*
local PLAYER_CAMERA_STATE = 0x388      -- 0: first person
local PLAYER_BALL_STATE = 0x38C        -- 0: not morphed
local PLAYER_GUN = 0xEBC               -- CPlayerGun*
-- Where the game draws the cannon model from the gun transform's origin, in the camera's axes
-- (+X right, +Y forward, +Z up; it does not turn with the gun), game units: the patch moves the
-- transform by minus this, so the model's origin is at the hand.
local GUN_ROOT = vec3(0.25, 0.30, -0.35)
local BASE_MODEL_FORWARD = -0.080      -- PrimedGun's model offset, meters

-- The function that builds the gun's transform (r27 = the gun) ends by copying the finished
-- matrix (r31) into gun+0x24; the caller then derives the aim transform (gun+0x54) from the same
-- matrix. r29 points to the eye position and r30 to the transform the gun is built from.
local HOOK = 0x801DDA84
local HOOK_ORIGINAL = 0x7FE4FB78       -- mr r4, r31

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local BASIS = S + 0x00    -- 3x3 row-major: the hand's rotation in the camera's axes
local OFFSET = S + 0x24   -- 3 floats: the hand's position from the eye, camera axes, game units
local GUARD = S + 0x30    -- the CPlayerGun* the pose is for; 0 = not tracked
local CAVE = S + 0x40

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- When the gun is tracked: rotation = r30's rotation * BASIS, translation = eye + r30's rotation
-- * OFFSET, written into the finished matrix before the game copies it.
local function cave_words()
  local t = {load_address("r11", S), "lwz r12, 0x30(r11)\ncmplw r12, r27\nbne done\n"}
  for i = 0, 8 do
    t[#t + 1] = string.format("lfs f%d, 0x%X(r11)\n", i + 1, i * 4)
  end
  for row = 0, 2 do
    local o = row * 0x10
    t[#t + 1] = string.format("lfs f10, 0x%X(r30)\nlfs f11, 0x%X(r30)\nlfs f12, 0x%X(r30)\n",
                              o, o + 4, o + 8)
    for col = 0, 2 do
      t[#t + 1] = string.format(
        "fmuls f13, f10, f%d\nfmadds f13, f11, f%d, f13\nfmadds f13, f12, f%d, f13\n" ..
        "stfs f13, 0x%X(r31)\n", col + 1, col + 4, col + 7, o + col * 4)
    end
  end
  t[#t + 1] = "lfs f1, 0x24(r11)\nlfs f2, 0x28(r11)\nlfs f3, 0x2C(r11)\n"
  for row = 0, 2 do
    local o = row * 0x10
    t[#t + 1] = string.format(
      "lfs f10, 0x%X(r30)\nlfs f11, 0x%X(r30)\nlfs f12, 0x%X(r30)\nlfs f13, 0x%X(r29)\n" ..
      "fmadds f13, f10, f1, f13\nfmadds f13, f11, f2, f13\nfmadds f13, f12, f3, f13\n" ..
      "stfs f13, 0x%X(r31)\n", o, o + 4, o + 8, row * 4, o + 0xC)
  end
  t[#t + 1] = "done:\nmr r4, r31\n" .. string.format("b `0x%X`\n", HOOK + 4)
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
           "): is this Metroid Prime 2 NTSC-U?")
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

-- OpenXR view axes (+X right, +Y up, -Z forward) to the game's camera axes (+X right,
-- +Y forward, +Z up).
function to_game_basis(rotation)
  local q = rotation * quat.axis_y(settings.yaw) * quat.axis_x(settings.pitch) *
            quat.axis_z(settings.roll)
  local r = mat33.from_quat(q)
  return mat33(r:get(0, 0), -r:get(0, 2), r:get(0, 1),
               -r:get(2, 0), r:get(2, 2), -r:get(2, 1),
               r:get(1, 0), -r:get(1, 2), r:get(1, 1))
end

function to_game_offset(position, scale)
  return vec3((position.x + settings.offset_x) * scale,
              -(position.z + settings.offset_z) * scale,
              (position.y + settings.offset_y) * scale)
end

local smooth = smoother(0.24, 2.0)
local tracking = false

local function stop_tracking()
  if tracking then
    mem.write_u32(GUARD, 0)
    smooth:reset()
    tracking = false
  end
end

local function hand_pose()
  if settings.test_pitch ~= 0 then
    return pose(vec3(0.2, -0.25, -0.35), quat.axis_x(settings.test_pitch))
  end
  return vr.hand(settings.hand, "view")
end

local function track()
  local hand = hand_pose()
  local player = mem.ptr(PLAYER)
  if not hand or not player or mem.u32(player + PLAYER_CAMERA_STATE) ~= 0 or
     mem.u32(player + PLAYER_BALL_STATE) ~= 0 then
    stop_tracking()
    return
  end
  local gun = mem.ptr(player + PLAYER_GUN)
  if not gun then
    stop_tracking()
    return
  end

  local scale = vr.units_per_meter() * settings.scale
  smooth.keep = settings.smoothing
  local m = smooth:update(mat34.from_basis(to_game_basis(hand.rot),
                                           to_game_offset(hand.pos, scale)))
  local basis = m:basis()
  if settings.hand == 0 then
    -- The left hand mirrors the cannon.
    basis = mat33(-basis:get(0, 0), basis:get(0, 1), basis:get(0, 2),
                  -basis:get(1, 0), basis:get(1, 1), basis:get(1, 2),
                  -basis:get(2, 0), basis:get(2, 1), basis:get(2, 2))
  end
  local offset = m:translation() - GUN_ROOT +
    basis * vec3(settings.model_x, BASE_MODEL_FORWARD + settings.model_y, settings.model_z) * scale
  if not mem.write_mat33(BASIS, basis) or not mem.write_vec3(OFFSET, offset) then
    stop_tracking()
    return
  end
  -- Last: arms the cave for this gun.
  mem.write_u32(GUARD, gun)
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
  smooth:reset()
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
