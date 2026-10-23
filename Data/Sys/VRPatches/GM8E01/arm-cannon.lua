-- Metroid Prime (NTSC-U, revision 0): the arm cannon follows a VR controller.
--
-- A DolphinXR VR game patch, ported from PrimedGun's arm cannon tracking (PrimedGun, created by
-- Nobbie, GPL-2.0-or-later: Source/Core/Core/PrimedGun/NativeRuntime.cpp UpdateCannonTracking and
-- PoseToPrimeMatrix, and the cannon hooks of PrimedGunBuiltinPatches.inc). The tracking math, the
-- game addresses and the hook points are PrimedGun's. The code caves are rewritten for this engine:
-- they live in the patch's scratch memory (so the game's Gecko codes keep working), only write
-- while the cannon is tracked, and use only volatile registers that carry no arguments.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Arm cannon follows the controller",
  author = "Nobbie (PrimedGun), ported to DolphinXR",
  version = "1.0",
  description = "The arm cannon is aimed and placed by your VR controller, and shots leave it "
    .. "where it points. Turn the profile's Cannon override off: this patch turns it off while it "
    .. "runs.",
  games = {"GM8E01"},
  revision = 0,
  scratch = 0x230,
  conflicts = {texture = {"Cannon"}},
  settings = {
    {name = "hand", label = "Cannon hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "scale", label = "Hand movement scale", default = 1, min = 0.5, max = 2, step = 0.05,
     description = "Times the profile's Units per Meter (1.5): above 1 the cannon moves farther "
       .. "than the hand."},
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
    {name = "model_x", label = "Model right offset", unit = "m", default = 0, min = -0.3,
     max = 0.3, step = 0.01},
    {name = "model_y", label = "Model forward offset", unit = "m", default = 0, min = -0.3,
     max = 0.3, step = 0.01},
    {name = "model_z", label = "Model up offset", unit = "m", default = 0, min = -0.3, max = 0.3,
     step = 0.01},
    {name = "smoothing", label = "Smoothing", default = 0.24, min = 0, max = 0.9, step = 0.01,
     description = "Part of the previous position kept each frame (PrimedGun: 0.24)."},
    {name = "beams", label = "Shots leave the tracked cannon", type = "bool", default = true},
    {name = "test_pitch", label = "Test without a headset", unit = "deg", default = 0, min = -60,
     max = 60, step = 5,
     description = "Not 0: when there is no VR controller, the cannon is held at this pitch, to "
       .. "check the patch on a flat screen."},
  },
}

-- ---- The game (Metroid Prime NTSC-U 1.00) ----

local STATE_MANAGER = 0x8045A1A8
local PLAYER = STATE_MANAGER + 0x84C       -- CPlayer*
local CAMERA_MANAGER = STATE_MANAGER + 0x86C
local OBJECT_LIST = STATE_MANAGER + 0x810
local GUN_POSITION = 0x8045BCE8            -- the first-person cannon position (3 floats)
local PLAYER_GUN = 0x490                   -- CPlayer -> CPlayerGun*
local GUN_WORLD_XF = 0x4A8                 -- CPlayerGun transforms (3x4)
local GUN_LOCAL_XF = 0x4D8
local GUN_XF = 0x3E8
local BASE_MODEL_FORWARD = -0.080          -- PrimedGun's model offset, meters

-- The hooks (PrimedGun's): the instruction after the game rebuilds the cannon's transform, the
-- cannon model's render function, and the six calls that create a beam's projectile.
local ROTATION_HOOK = 0x80040FEC
local ROTATION_HOOK_ORIGINAL = 0x807C0740  -- lwz r3, 0x740(r28)
local MODEL_HOOK = 0x80041A8C               -- stwu r1, ... (the function's first instruction)
local PROJECTILE_FUNCTION = 0x801BB888
local PROJECTILE_CALLS = {0x800DF8DC, 0x800DF008, 0x800DF038, 0x800E1240, 0x8018C950,
                          0x8018C988}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local BASIS = S + 0x00        -- 3x3, row-major: the cannon's rotation in world axes
local GUARD = S + 0x38        -- the CPlayerGun* the basis is for; 0 = not tracked
local MODEL_OFFSET = S + 0x40 -- 3 floats, world axes
-- S + 0x50: the cannon position plus the model offset (written by the model cave)
-- S + 0x60: a projectile transform (12 floats, written by the projectile cave)
-- S + 0x90: r12, saved by the rotation cave
local ROTATION_CAVE = S + 0xA0
local MODEL_CAVE = S + 0x140
local MODEL_CAVE_ORIGINAL = 16 -- word index of the relocated first instruction in the model cave
local PROJECTILE_CAVE = S + 0x190

-- "lis rD, hi; ori rD, rD, lo" with a signed hi (lis takes a signed immediate).
local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

local function assemble(text, base)
  local words, err = asm.assemble(text, base)
  if not words then error("assembling the cave at " .. mem.hex(base) .. ": " .. err) end
  return words
end

-- After the game rebuilds the cannon's transforms: when the basis is for this cannon (r28),
-- write it into the rotation of the world transform (+0x4A8) and the gun transform (+0x3E8).
local function rotation_cave()
  local text = "lwz r3, 0x740(r28)\n" .. load_address("r11", S) ..
    "stw r12, 0x90(r11)\nlwz r12, 0x38(r11)\ncmplw r12, r28\nbne done\n"
  local slots = {0x0, 0x4, 0x8, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28}
  for i, slot in ipairs(slots) do
    text = text .. string.format("lwz r12, 0x%X(r11)\nstw r12, 0x%X(r28)\nstw r12, 0x%X(r28)\n",
                                 (i - 1) * 4, GUN_WORLD_XF + slot, GUN_XF + slot)
  end
  text = text .. "done:\nlwz r12, 0x90(r11)\n" ..
    string.format("b `0x%X`\n", ROTATION_HOOK + 4)
  return assemble(text, ROTATION_CAVE)
end

-- At the cannon model's render function: the position argument (r5) plus the model offset.
local function model_cave(original)
  local text = load_address("r11", S)
  for i = 0, 2 do
    text = text .. string.format(
      "lfs f0, %d(r5)\nlfs f13, 0x%X(r11)\nfadds f0, f0, f13\nstfs f0, 0x%X(r11)\n",
      i * 4, 0x40 + i * 4, 0x50 + i * 4)
  end
  text = text .. "addi r5, r11, 0x50\n"
  local words = assemble(text, MODEL_CAVE)
  -- Then the instruction the hook replaced, and the way back.
  words[#words + 1] = original
  words[#words + 1] = asm.branch(MODEL_CAVE + #words * 4, MODEL_HOOK + 4)
  assert(words[MODEL_CAVE_ORIGINAL] == original, "the model cave layout changed")
  return words
end

-- In place of the projectile function: give it the cannon's rotation and the original
-- transform's (r6) position.
local function projectile_cave()
  local text = "stwu r1, -16(r1)\nmflr r0\nstw r0, 20(r1)\n" .. load_address("r11", S) ..
    "lwz r12, 0x38(r11)\ncmpwi r12, 0\nbeq call\naddi r12, r11, 0x60\n"
  local rows = {{0x0, 0x4, 0x8, 0xC}, {0x10, 0x14, 0x18, 0x1C}, {0x20, 0x24, 0x28, 0x2C}}
  local basis = 0
  for _, row in ipairs(rows) do
    for column = 1, 3 do
      text = text .. string.format("lwz r0, 0x%X(r11)\nstw r0, 0x%X(r12)\n", basis, row[column])
      basis = basis + 4
    end
    text = text .. string.format("lwz r0, 0x%X(r6)\nstw r0, 0x%X(r12)\n", row[4], row[4])
  end
  text = text .. "mr r6, r12\ncall:\n" ..
    string.format("bl `0x%X`\n", PROJECTILE_FUNCTION) ..
    "lwz r0, 20(r1)\nmtlr r0\naddi r1, r1, 16\nblr\n"
  return assemble(text, PROJECTILE_CAVE)
end

-- ---- Installing and removing the hooks ----

local caves = nil          -- {rotation = words, model = words, projectile = words}
local model_original = nil -- the model function's first instruction
local installed = false
local warned = false

-- The target of a b or bl (relative) at `from`, nil for other instructions.
local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- The model function's original first instruction, also when a hook (ours, maybe from a
-- savestate made with another scratch address) is already there.
local function find_model_original()
  local current = mem.u32(MODEL_HOOK)
  if current == nil then return nil end
  if current & 0xFFFF0000 == 0x94210000 then return current end
  local target = branch_target(MODEL_HOOK, current)
  if target then
    local saved = mem.u32(target + (MODEL_CAVE_ORIGINAL - 1) * 4)
    if saved and saved & 0xFFFF0000 == 0x94210000 then return saved end
  end
  return nil
end

local function check_game_code()
  local rotation = mem.u32(ROTATION_HOOK)
  if rotation ~= ROTATION_HOOK_ORIGINAL and not branch_target(ROTATION_HOOK, rotation or 0) then
    return "unexpected code at " .. mem.hex(ROTATION_HOOK) .. " (" .. mem.hex(rotation) ..
      "): is this Metroid Prime NTSC-U 1.00?"
  end
  model_original = model_original or find_model_original()
  if not model_original then
    return "unexpected code at " .. mem.hex(MODEL_HOOK)
  end
  return nil
end

local function install()
  local problem = check_game_code()
  if problem then
    if not warned then warn("not installed: " .. problem) end
    warned = true
    installed = false
    return
  end
  if not caves then
    caves = {rotation = rotation_cave(), model = model_cave(model_original),
             projectile = projectile_cave()}
  end
  -- The caves first, then the branches into them.
  local changed = mem.ensure_code(ROTATION_CAVE, caves.rotation)
  changed = mem.ensure_code(MODEL_CAVE, caves.model) or changed
  changed = mem.ensure_code(PROJECTILE_CAVE, caves.projectile) or changed
  changed = mem.ensure_code(ROTATION_HOOK, asm.branch(ROTATION_HOOK, ROTATION_CAVE)) or changed
  changed = mem.ensure_code(MODEL_HOOK, asm.branch(MODEL_HOOK, MODEL_CAVE)) or changed
  for _, call in ipairs(PROJECTILE_CALLS) do
    local original = asm.branch(call, PROJECTILE_FUNCTION, true)
    local hooked = asm.branch(call, PROJECTILE_CAVE, true)
    local current = mem.u32(call)
    local wanted = settings.beams and hooked or original
    if current == original or current == hooked or branch_target(call, current or 0) then
      changed = mem.ensure_code(call, wanted) or changed
    else
      warn("projectile call at " .. mem.hex(call) .. " is not the expected bl: skipped")
    end
  end
  if changed then log("hooks installed at", mem.hex(S)) end
  installed = true
end

-- Puts the game's code back, also over hooks to caves at another address (a savestate).
local function uninstall()
  mem.write_u32(GUARD, 0)
  installed = false
  if branch_target(ROTATION_HOOK, mem.u32(ROTATION_HOOK) or 0) then
    mem.write_code(ROTATION_HOOK, ROTATION_HOOK_ORIGINAL)
  end
  if model_original and branch_target(MODEL_HOOK, mem.u32(MODEL_HOOK) or 0) then
    mem.write_code(MODEL_HOOK, model_original)
  end
  for _, call in ipairs(PROJECTILE_CALLS) do
    local target = branch_target(call, mem.u32(call) or 0)
    if target and target ~= PROJECTILE_FUNCTION then
      mem.write_code(call, asm.branch(call, PROJECTILE_FUNCTION, true))
    end
  end
end

-- ---- Tracking (PrimedGun's UpdateCannonTracking) ----

-- The yaw of the active camera's transform, degrees (PrimedGun: ReadYawFromTransform2D).
local function camera_yaw()
  local manager, objects = mem.u32(CAMERA_MANAGER), mem.u32(OBJECT_LIST)
  if not manager or manager < 0x80000000 or not objects or objects < 0x80000000 then return nil end
  local word = mem.u32(manager)
  if not word then return nil end
  local uid = word >> 16
  if uid == 0xFFFF then return nil end
  local camera = mem.u32(objects + ((uid & 0x3FF) << 3) + 4)
  if not camera or camera < 0x80000000 then return nil end
  local transform = camera + 0x34
  local m00, m01 = mem.f32s(transform, 2)
  local m10, m11 = mem.f32s(transform + 0x10, 2)
  if m00 == nil or m10 == nil then return nil end
  local row0, row1 = math.sqrt(m00 * m00 + m01 * m01), math.sqrt(m10 * m10 + m11 * m11)
  if row0 < 0.7 or row0 > 1.3 or row1 < 0.7 or row1 > 1.3 or
     math.abs(m00 * m11 - m01 * m10) < 0.5 then
    return nil
  end
  return math.deg(math.atan(m01, m00))
end

-- The OpenXR rotation (view space, +Y up, -Z forward) as Prime's world basis (+Z up, +Y forward),
-- turned to the camera's yaw, with the local rotation offsets (PrimedGun: PoseToPrimeMatrix).
function to_prime_basis(rotation, yaw_delta)
  local q = quat.axis_y(yaw_delta) * rotation
  local offset = quat.axis_y(settings.yaw) * quat.axis_x(settings.pitch) * quat.axis_z(settings.roll)
  local r = mat33.from_quat(q * offset)
  return mat33(-r[1], r[3], -r[2],
               r[7], -r[9], r[8],
               r[4], -r[6], r[5])
end

-- The view-space position as Prime's camera-local offset (+X right, +Y forward, +Z up), in game
-- units, with the calibration offsets (meters: x right, y up, z back). PrimedGun also turns its
-- offsets by the camera's yaw; they stay camera-local here, like the position they add to (all
-- default to 0, so the default placement is PrimedGun's).
function to_prime_position(position, scale)
  return vec3((position.x + settings.offset_x) * scale,
              -(position.z + settings.offset_z) * scale,
              (position.y + settings.offset_y) * scale)
end

local function looks_valid_player(player)
  local camera_state, morph_state = mem.u32(player + 0x2F4), mem.u32(player + 0x2F8)
  local gun_alpha, holster = mem.f32(player + 0x494), mem.u32(player + 0x498)
  -- First person, not morphed, the cannon drawn and out (PrimedGun: PlayerIsFirstPersonGunReady).
  return camera_state == 0 and morph_state == 0 and gun_alpha ~= nil and gun_alpha >= 0.95 and
         holster == 2
end

local smooth = smoother(0.24, 2.0)
local tracking = false

local function stop_tracking()
  if tracking then
    mem.write_u32(GUARD, 0)
    mem.write_f32s(MODEL_OFFSET, 0, 0, 0)
    smooth:reset()
    tracking = false
  end
end

-- The controller's pose seen from the game camera, or a test pose on a flat screen.
local function hand_pose()
  local hand = vr.hand(settings.hand, "view")
  if hand then return hand end
  if settings.test_pitch ~= 0 then
    return pose(vec3(0.2, -0.25, -0.35), quat.axis_x(settings.test_pitch))
  end
  return nil
end

local function track()
  local hand = hand_pose()
  local player = mem.ptr(PLAYER)
  if not hand or not player or not looks_valid_player(player) then
    stop_tracking()
    return
  end
  local gun = mem.u32(player + PLAYER_GUN)
  if not gun or gun < 0x80000000 then
    stop_tracking()
    return
  end
  local world, local_xf = mem.mat34(gun + GUN_WORLD_XF), mem.mat34(gun + GUN_LOCAL_XF)
  if not world or not local_xf or not world:looks_like_transform() or
     not local_xf:looks_like_transform() then
    stop_tracking()
    return
  end

  local yaw = camera_yaw() or 180
  local yaw_delta = -wrap180(yaw + 180)
  local scale = vr.units_per_meter() * settings.scale
  smooth.keep = settings.smoothing
  local m = smooth:update(mat34.from_basis(to_prime_basis(hand.rot, yaw_delta),
                                           to_prime_position(hand.pos, scale)))

  -- The model's basis: the left hand mirrors the cannon.
  local basis = m:basis()
  if settings.hand == 0 then
    basis = mat33(-basis[1], basis[2], basis[3], -basis[4], basis[5], basis[6],
                  -basis[7], basis[8], basis[9])
  end
  local model_offset = basis * vec3(settings.model_x * scale,
                                    (BASE_MODEL_FORWARD + settings.model_y) * scale,
                                    settings.model_z * scale)
  if not mem.write_mat33(BASIS, basis) or not mem.write_vec3(GUN_POSITION, m:translation()) or
     not mem.write_vec3(MODEL_OFFSET, model_offset) then
    stop_tracking()
    return
  end
  -- Last: arms the caves for this cannon.
  mem.write_u32(GUARD, gun)
  tracking = true
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(GUARD, 0)
  install()
end

function on_state_loaded()
  -- The loaded RAM may have other code (or none) at the hooks, and a stale guard.
  mem.write_u32(GUARD, 0)
  tracking = false
  smooth:reset()
  install()
end

function on_setting_changed(name)
  if name == "beams" then install() end
end

function on_frame()
  -- Like PrimedGun, put the hooks back now and then (a module reload or a code may restore them).
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
