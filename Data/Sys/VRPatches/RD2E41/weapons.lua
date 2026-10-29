-- Red Steel 2 (NTSC-U): the revolver and the sword follow a VR controller, the sword's orientation
-- in the game is the controller's, and the revolver aims where it points.
--
-- Drawing: the renderer draws each object from its render node (802983E8: the node's world
-- rotation and position, then 802982F8 multiplies them by the view and loads the result). A cave
-- on the branch from one to the other gives the nodes of the player's weapons, recognized by their
-- model objects, the world matrix that puts the weapon where the controller is seen from the game
-- camera: the controller's pose in the camera's view space times the inverse of the view, both
-- read in the same frame, so it does not lag when the camera turns. A weapon of several parts (the
-- revolver's cylinder and hammer) keeps them where they are on its body.
--
-- The game: it reads the Wii Remote through KPAD, which integrates the MotionPlus samples into the
-- remote's direction (KPADRead, 8036E054) and turns the sensor bar dots into the pointer. A second
-- cave, right after that step and before the status is copied to the game, replaces the direction
-- with the controller's real orientation while the sword is drawn (the game's sword and arm follow
-- it: no MotionPlus drift and none of the emulated gyro's approximations; the next samples
-- integrate from it), and the pointer with the point the revolver aims at while the revolver is
-- drawn (the game shoots through the pointer).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Weapons follow the controller",
  author = "DolphinXR",
  version = "1.2",
  description = "The revolver and the sword are drawn in your hand, where the VR controller is "
    .. "and pointing where it points; the game takes the sword's orientation from the controller "
    .. "and aims the revolver where it points. For Red Steel 2 NTSC-U.",
  games = {"RD2E41"},
  revision = 0,
  scratch = 0x500,
  settings = {
    {name = "hand", label = "Weapon hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "revolver", label = "Revolver", type = "bool", default = true,
     description = "The revolver is drawn where the controller is."},
    {name = "sword", label = "Sword", type = "bool", default = true,
     description = "The sword is drawn where the controller is, its blade along the controller's "
       .. "aim."},
    {name = "true_pose", label = "True sword pose", type = "bool", default = true,
     description = "While the sword is out, the game reads the Wii Remote's orientation from the "
       .. "controller itself instead of integrating the emulated MotionPlus: the game's sword "
       .. "and swings follow the controller exactly, without drift."},
    {name = "true_aim", label = "True revolver aim", type = "bool", default = true,
     description = "While the revolver is out, the Wii pointer is where the revolver points: the "
       .. "game shoots along the barrel."},
    {name = "aim_distance", label = "Aim distance", unit = "m", default = 8, min = 1, max = 50,
     step = 0.5,
     description = "True aim: the distance at which the shot crosses the barrel's line (the game "
       .. "shoots from the eyes, not from the barrel)."},
    {name = "sword_pitch", label = "Sword pitch offset", unit = "deg", default = 0, min = -90,
     max = 90, step = 1,
     description = "Tilts the drawn blade up (positive) or down from the controller's aim."},
    {name = "scale", label = "Hand movement scale", default = 1, min = 0.5, max = 2, step = 0.05,
     description = "Times the profile's Units per Meter: above 1 the weapon moves farther than "
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
    {name = "hide_arms", label = "Hide the arms", type = "bool", default = true,
     description = "While a weapon follows the controller, the game's arms are hidden: they stay "
       .. "where the game holds them, without the weapon."},
    {name = "smoothing", label = "Smoothing", default = 0, min = 0, max = 0.9, step = 0.01,
     description = "Part of the previous pose kept each frame (the drawn weapon only)."},
    {name = "test_pitch", label = "Test pose", unit = "deg", default = 0, min = -60, max = 60,
     step = 5,
     description = "Not 0: the weapon is held at this pitch in front of the camera instead of "
       .. "following the controller, to check the patch without a headset."},
  },
}

-- ---- The game (Red Steel 2 NTSC-U) ----

local RENDERER = 0x8054E178     -- [r13 - 0x6F48]: the renderer, its view (4x4, row vectors:
                                -- clip = p * V * P) at +0x210, the world matrix it draws an
                                -- object with at +0x2D0, the projection at +0x6FC
local VIEW_OFFSET = 0x210
local WORLD_OFFSET = 0x2D0
local PROJECTION_OFFSET = 0x6FC
-- A render node holds its world rotation rows (+0, +0x10, +0x20), its position (+0x80) and its
-- model object (+0x90): 802983E8 receives it in r4.
local NODE_POSITION = 0x80
local NODE_MODEL = 0x90
local MODEL_CLASS = 0x8046FF30  -- the vtable of a model object
local INVERSE = 0x800ECA1C      -- rigid inverse of a 4x4: r3 = inverse(r4)
local CONCAT = 0x800379C4       -- 4x4 multiplication: r3 = r4 * r5
local DRAW = 0x802982F8         -- multiplies the world matrix by the view and loads it
-- 802983E8 copies the node into the renderer's world matrix and ends with "b DRAW".
local DRAW_HOOK = 0x80298438
local DRAW_HOOK_ORIGINAL = 0x4BFFFEC0
local MAX_DISTANCE = 2.0        -- game units from the camera: a held weapon is closer
-- KPADRead: after the MotionPlus step (bl 8036E054, r21 the channel's status), "li r0, 30" starts
-- copying the status to the game's buffer.
local KPAD_HOOK = 0x8036A2B0
local KPAD_HOOK_ORIGINAL = 0x3800001E
local KPAD_STATUS = 0x805259C0  -- KPAD's status of the first Wii Remote
local KPAD_POS = 0x20           -- the pointer (x right, y down, -1 to 1 across the screen)
local KPAD_DIR = 0xC8           -- the remote's left, top and pointing axes (x left, y up,
                                -- z toward the screen)
local SIGNATURE = {{0x802983E8, 0xE0E40000}, {0x8029840C, 0xE0240080}, {0x80298418, 0xF0E302D0},
                   {0x8036A2AC, 0x48003DA9}, {0x8036A2B4, 0x38BAFFFC}}

-- The player's weapons, by the model objects of their nodes (allocated at the same addresses in
-- every level seen so far, whether the weapon is drawn or not). The body's pose is the
-- controller's; the parts keep their place on it. The axes are the model's in the controller's:
-- the revolver's barrel is its +X, its left +Y, its top +Z; the sword's blade is its +Z, its +X
-- faces away from the hand when held upright, its +Y to the left.
local REVOLVER, SWORD = 1, 2
local WEAPONS = {
  {setting = "revolver", body = 0x812AD7A0, parts = {0x812AD6E0, 0x812AD620, 0x812AD560, 0x812AD860}},
  {setting = "sword", body = 0x812A8BA0, parts = {}},
}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local DRAWN = S + 0x00         -- per weapon: counts its nodes drawn
local RETURN = S + 0x08        -- the caller's return address
local RENDERER_SAVE = S + 0x0C
local NODE_SAVE = S + 0x10
local HAND_SAVE = S + 0x14     -- the offset from S of the weapon's HAND
local LIMIT = S + 0x18         -- MAX_DISTANCE squared (f32)
local DRAWN_SAVE = S + 0x1C    -- the offset from S of the weapon's DRAWN
local MODELS = S + 0x20        -- the model objects of the tracked nodes; 0 = not tracked
local MAX_MODELS = 6
local BODY_NODES = S + 0x38    -- per weapon: its body node, as the cave last saw it drawn
local KPAD_ARMED = S + 0x40    -- KPAD_STATUS when the KPAD cave writes; 0 = not
local KPAD_FLAGS = S + 0x44    -- 1: the direction, 2: the pointer
local POS = S + 0x48           -- the pointer (2 floats)
local DIR = S + 0x50           -- the direction (9 floats)
local MOVES = S + 0x74         -- per weapon: 1 when its drawing follows the controller
local MOVE_SAVE = S + 0x7C     -- the offset from S of the weapon's MOVES
local HANDS = S + 0x80         -- 4x4 per weapon: its body in the camera's view space
local BODY = S + 0x100         -- 4x4: the body's world matrix, then temporary results
local INV_BODY = S + 0x140     -- 4x4: its inverse
local INV_VIEW = S + 0x180     -- 4x4: the inverse of the view (the camera's world matrix)
local MOVE = S + 0x1C0         -- 4x4: inverse(BODY) * HAND * inverse(view)
local CAVES = S + 0x200

local function hand_matrix(w) return HANDS + (w - 1) * 0x40 end

-- The model slot of each tracked model, in WEAPONS order: {model, weapon index, is_body}.
local SLOTS = {}
for w, weapon in ipairs(WEAPONS) do
  SLOTS[#SLOTS + 1] = {weapon.body, w, true}
  for _, part in ipairs(weapon.parts) do SLOTS[#SLOTS + 1] = {part, w, false} end
end
assert(#SLOTS <= MAX_MODELS, "too many weapon models")

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

local function o(address) return address - S end

-- A copy of count words from (from + 4) up, to (to + 4) up: from and to hold the addresses 4
-- before, in r9 and r10.
local function copy_loop(label, count)
  return string.format("li r0, %d\nmtctr r0\n%s:\nlwzu r0, 4(r9)\nstwu r0, 4(r10)\nbdnz %s\n",
                       count, label, label)
end

-- In place of "b DRAW" at the end of 802983E8 (r3 the renderer, r4 the node, already copied to
-- the renderer's world matrix at +0x2D0): for a node of a tracked weapon near the camera, that
-- matrix becomes world * inverse(body) * HAND * inverse(view), which puts the weapon's body where
-- the controller is and keeps its other parts where they are on it. The return address is kept
-- for DRAW, which returns to 802983E8's caller.
local function draw_cave(t)
  local reload = load_address("r11", S)
  t[#t + 1] = reload .. string.format("lwz r12, 0x%X(r4)\n", NODE_MODEL)
  for i = 1, #SLOTS do
    t[#t + 1] = string.format("lwz r0, 0x%X(r11)\ncmplw r12, r0\nbeq slot%d\n",
                              o(MODELS) + (i - 1) * 4, i)
  end
  t[#t + 1] = string.format("b `0x%X`\n", DRAW)
  for i, slot in ipairs(SLOTS) do
    local w, is_body = slot[2], slot[3]
    t[#t + 1] = string.format("slot%d:\n", i)
    if is_body then
      t[#t + 1] = string.format("stw r4, 0x%X(r11)\n", o(BODY_NODES) + (w - 1) * 4)
    end
    t[#t + 1] = string.format("li r9, 0x%X\nli r10, 0x%X\nli r8, 0x%X\nli r7, 0x%X\nb hit\n",
                              o(BODY_NODES) + (w - 1) * 4, o(hand_matrix(w)),
                              o(DRAWN) + (w - 1) * 4, o(MOVES) + (w - 1) * 4)
  end
  t[#t + 1] = "hit:\nlwzx r12, r11, r9\ncmpwi r12, 0\nbeq skip\n"
  t[#t + 1] = string.format("stw r10, 0x%X(r11)\nstw r8, 0x%X(r11)\nstw r7, 0x%X(r11)\n" ..
                            "mflr r0\nstw r0, 0x%X(r11)\n", o(HAND_SAVE), o(DRAWN_SAVE),
                            o(MOVE_SAVE), o(RETURN))
  t[#t + 1] = string.format("stw r3, 0x%X(r11)\nstw r4, 0x%X(r11)\n", o(RENDERER_SAVE),
                            o(NODE_SAVE))
  -- BODY: the body node's rotation rows (+0 to +0x30) and position row (+0x80).
  t[#t + 1] = string.format("addi r9, r12, -4\naddi r10, r11, 0x%X\n", o(BODY) - 4) ..
              copy_loop("rows", 12) .. string.format("addi r9, r12, 0x%X\n", NODE_POSITION - 4) ..
              copy_loop("position", 4)
  local function renderer_plus(register, offset)
    return string.format("lwz %s, 0x%X(r11)\naddi %s, %s, 0x%X\n", register, o(RENDERER_SAVE),
                         register, register, offset)
  end
  t[#t + 1] = string.format("addi r3, r11, 0x%X\n", o(INV_VIEW)) ..
              renderer_plus("r4", VIEW_OFFSET) .. string.format("bl `0x%X`\n", INVERSE)
  -- A tracked model far from the camera is not a held weapon: left as it is.
  t[#t + 1] = reload .. string.format("lwz r12, 0x%X(r11)\n", o(NODE_SAVE))
  for axis = 0, 2 do
    t[#t + 1] = string.format("lfs f%d, 0x%X(r12)\nlfs f0, 0x%X(r11)\nfsubs f%d, f%d, f0\n",
                              axis + 1, NODE_POSITION + axis * 4, o(INV_VIEW) + 0x30 + axis * 4,
                              axis + 1, axis + 1)
  end
  t[#t + 1] = string.format("fmuls f4, f1, f1\nfmadds f4, f2, f2, f4\nfmadds f4, f3, f3, f4\n" ..
                            "lfs f0, 0x%X(r11)\nfcmpo cr0, f4, f0\nbgt restore\n", o(LIMIT))
  -- Counted whether it is moved or not (the KPAD cave needs to know the weapon is out). Not
  -- with r0: "addi r0, r0, 1" is "li r0, 1".
  t[#t + 1] = string.format("lwz r12, 0x%X(r11)\nlwzx r9, r11, r12\naddi r9, r9, 1\n" ..
                            "stwx r9, r11, r12\nlwz r12, 0x%X(r11)\nlwzx r0, r11, r12\n" ..
                            "cmpwi r0, 0\nbeq restore\n", o(DRAWN_SAVE), o(MOVE_SAVE))
  t[#t + 1] = string.format("addi r3, r11, 0x%X\naddi r4, r11, 0x%X\nbl `0x%X`\n", o(INV_BODY),
                            o(BODY), INVERSE)
  t[#t + 1] = reload .. string.format("addi r3, r11, 0x%X\naddi r4, r11, 0x%X\n" ..
                                      "lwz r5, 0x%X(r11)\nadd r5, r5, r11\nbl `0x%X`\n",
                                      o(BODY), o(INV_BODY), o(HAND_SAVE), CONCAT)
  t[#t + 1] = reload .. string.format("addi r3, r11, 0x%X\naddi r4, r11, 0x%X\n" ..
                                      "addi r5, r11, 0x%X\nbl `0x%X`\n", o(MOVE), o(BODY),
                                      o(INV_VIEW), CONCAT)
  t[#t + 1] = reload .. string.format("addi r3, r11, 0x%X\n", o(BODY)) ..
              renderer_plus("r4", WORLD_OFFSET) ..
              string.format("addi r5, r11, 0x%X\nbl `0x%X`\n", o(MOVE), CONCAT)
  -- The renderer's world matrix = BODY.
  t[#t + 1] = reload .. string.format("lwz r10, 0x%X(r11)\naddi r10, r10, 0x%X\n" ..
                                      "addi r9, r11, 0x%X\n", o(RENDERER_SAVE),
                                      WORLD_OFFSET - 4, o(BODY) - 4) .. copy_loop("world", 16)
  t[#t + 1] = "restore:\n" .. reload ..
              string.format("lwz r0, 0x%X(r11)\nmtlr r0\nlwz r3, 0x%X(r11)\n", o(RETURN),
                            o(RENDERER_SAVE))
  t[#t + 1] = string.format("skip:\nb `0x%X`\n", DRAW)
end

-- In place of "li r0, 30" after KPADRead's MotionPlus step (r21 the channel's status): for the
-- armed status, the direction and/or the pointer are written there before it is copied to the
-- game. KPADRead keeps no value in r9, r10, r11, r12 or cr0 across that point (it just called).
local function kpad_cave(t)
  t[#t + 1] = load_address("r11", S) ..
              string.format("lwz r12, 0x%X(r11)\ncmplw r12, r21\nbne kdone\n", o(KPAD_ARMED)) ..
              string.format("lwz r12, 0x%X(r11)\nandi. r0, r12, 1\nbeq kpos\n", o(KPAD_FLAGS)) ..
              string.format("addi r9, r11, 0x%X\naddi r10, r21, 0x%X\n", o(DIR) - 4, KPAD_DIR - 4) ..
              copy_loop("kdir", 9) ..
              "kpos:\nandi. r0, r12, 2\nbeq kdone\n" ..
              string.format("lwz r0, 0x%X(r11)\nstw r0, 0x%X(r21)\nlwz r0, 0x%X(r11)\n" ..
                            "stw r0, 0x%X(r21)\n", o(POS), KPAD_POS, o(POS) + 4, KPAD_POS + 4) ..
              string.format("kdone:\nli r0, 30\nb `0x%X`\n", KPAD_HOOK + 4)
end

local DRAW_CAVE = CAVES
local KPAD_CAVE = nil

local function cave_words()
  local t = {}
  draw_cave(t)
  local words, err = asm.assemble(table.concat(t), DRAW_CAVE)
  if not words then error("assembling the draw cave: " .. err) end
  KPAD_CAVE = DRAW_CAVE + #words * 4
  local k = {}
  kpad_cave(k)
  local kwords, kerr = asm.assemble(table.concat(k), KPAD_CAVE)
  if not kwords then error("assembling the KPAD cave: " .. kerr) end
  for _, word in ipairs(kwords) do words[#words + 1] = word end
  assert(DRAW_CAVE + #words * 4 <= S + mem.scratch_size,
         "the caves do not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hooks ----

local cave = nil
local installed = false
local waiting_logged = false

local function branch_target(from, word)
  if word & 0xFC000003 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- A branch from a hook into the patches' scratch memory is this patch's cave, maybe from an
-- earlier version or another address, kept by a savestate made while it ran (1.0, the revolver
-- patch, had its cave elsewhere): installing replaces it.
local function our_branch(hook, word)
  local target = branch_target(hook, word or 0)
  return target ~= nil and target >= 0x80001800 and target < 0x80003000
end

local function hook_free(hook, original)
  local word = mem.u32(hook)
  return word == original or our_branch(hook, word)
end

local function present()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then return false end
  end
  return hook_free(DRAW_HOOK, DRAW_HOOK_ORIGINAL) and hook_free(KPAD_HOOK, KPAD_HOOK_ORIGINAL)
end

local function install()
  if not present() then
    installed = false
    if not waiting_logged then log("waiting for the game's render and KPAD code") end
    waiting_logged = true
    return
  end
  cave = cave or cave_words()
  mem.write_f32(LIMIT, MAX_DISTANCE * MAX_DISTANCE)
  local changed = mem.ensure_code(DRAW_CAVE, cave)
  changed = mem.ensure_code(DRAW_HOOK, asm.branch(DRAW_HOOK, DRAW_CAVE)) or changed
  changed = mem.ensure_code(KPAD_HOOK, asm.branch(KPAD_HOOK, KPAD_CAVE)) or changed
  if changed or not installed then
    log("hooks installed, caves at", mem.hex(DRAW_CAVE), "and", mem.hex(KPAD_CAVE))
  end
  installed = true
  waiting_logged = false
end

local function disarm()
  for i = 0, MAX_MODELS - 1 do mem.write_u32(MODELS + i * 4, 0) end
  mem.write_u32(KPAD_ARMED, 0)
end

local function forget_bodies()
  for w = 1, #WEAPONS do mem.write_u32(BODY_NODES + (w - 1) * 4, 0) end
end

local function uninstall()
  disarm()
  if our_branch(DRAW_HOOK, mem.u32(DRAW_HOOK)) then mem.write_code(DRAW_HOOK, DRAW_HOOK_ORIGINAL) end
  if our_branch(KPAD_HOOK, mem.u32(KPAD_HOOK)) then mem.write_code(KPAD_HOOK, KPAD_HOOK_ORIGINAL) end
  installed = false
end

-- ---- Tracking ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local sin, cos, rad = math.sin, math.cos, math.rad
local raw_pose, f32s, write_f32s, u32 = vr.raw_pose, mem.f32s, mem.write_f32s, mem.u32

-- The Hamilton product a * b of two quaternions (x, y, z, w).
local function qmul(ax, ay, az, aw, bx, by, bz, bw)
  return aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
         aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz
end

-- The right (+X), up (+Y) and forward (-Z) axes of a rotation of OpenXR's view space (the columns
-- of its matrix), in the game's view axes (+X right, +Y up, +Z forward: z negated).
local function axes(x, y, z, w)
  return 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), -2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), -2 * (y * z + x * w),
         -2 * (x * z + y * w), -2 * (y * z - x * w), 1 - 2 * (x * x + y * y)
end

-- The drawn weapon's pose kept between fields (its right, up and forward axes, then its
-- position), smoothed in place: each field keeps settings.smoothing of the previous pose, unless
-- the position jumped by more than SNAP game units.
local SNAP = 1.0
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
    disarm()
    has_smoothed = false
    tracking = false
  end
end

-- Whether each weapon was drawn in the last fields: the cave counts its draws.
local RECENT_FIELDS = 6
local last_drawn = {0, 0}
local drawn_field = {-1000, -1000}

local function update_drawn()
  local field = game.field()
  for w = 1, #WEAPONS do
    local drawn = u32(DRAWN + (w - 1) * 4) or 0
    if drawn ~= last_drawn[w] then
      last_drawn[w] = drawn
      drawn_field[w] = field
    end
  end
end

local function drawn_recently(w)
  return game.field() - drawn_field[w] <= RECENT_FIELDS
end

-- The game's arms are hidden through a flag of the game's VR profile (a Texture override of
-- their texture with condition=ARMS_FLAG) while a tracked weapon is drawn.
local ARMS_FLAG = "rd2-weapon-hide-arms"
local arms_hidden = false

local function update_arms()
  local hide = tracking and settings.hide_arms and
               ((settings.revolver and drawn_recently(REVOLVER)) or
                (settings.sword and drawn_recently(SWORD)))
  if hide ~= arms_hidden then
    vr.set_flag(ARMS_FLAG, hide)
    arms_hidden = hide
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

local function valid_model(model)
  return u32(model) == MODEL_CLASS
end

-- Writes the weapon's body in the camera's view space: its rows are the model's axes in the
-- game's view axes, then the position.
local function write_hand(w)
  local s = smoothed
  local rx, ry, rz, ux, uy, uz, fx, fy, fz = s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9]
  if w == REVOLVER then
    -- Barrel forward, left, top up.
    return write_f32s(hand_matrix(w), fx, fy, fz, 0, -rx, -ry, -rz, 0, ux, uy, uz, 0,
                      s[10], s[11], s[12], 1)
  end
  -- The blade along the aim, tilted by sword_pitch about the hand's right axis; +X faces away
  -- from the hand (down the controller when it points forward), +Y to the left.
  local p = rad(settings.sword_pitch)
  local cp, sp = cos(p), sin(p)
  local bx, by, bz = fx * cp + ux * sp, fy * cp + uy * sp, fz * cp + uz * sp
  local dx, dy, dz = ux * cp - fx * sp, uy * cp - fy * sp, uz * cp - fz * sp
  return write_f32s(hand_matrix(w), -dx, -dy, -dz, 0, -rx, -ry, -rz, 0, bx, by, bz, 0,
                    s[10], s[11], s[12], 1)
end

-- The pitch between the controller's aim and the emulated Wii Remote (DolphinXR's
-- WiimoteMotionPitch, 20 by default: a held remote points above a Touch controller's aim). The
-- key is not one vr.setting reads in every version: the default then.
local remote_pitch = 20
local function read_remote_pitch()
  local ok, value = pcall(vr.setting, "WiimoteMotionPitch")
  value = ok and tonumber(value or "")
  if value then remote_pitch = value end
end

-- KPAD's direction: the remote's left, top and pointing axes in KPAD's frame (x left, y up,
-- z toward the screen: the game's view axes with x negated), from the controller's raw
-- orientation pitched up by the remote pitch. Measured against the emulated MotionPlus: pointing
-- right gives -x, rolling the right side up turns the left axis down.
local function write_direction(qx, qy, qz, qw)
  local rx, ry, rz, ux, uy, uz, fx, fy, fz = axes(qx, qy, qz, qw)
  local p = rad(remote_pitch)
  local cp, sp = cos(p), sin(p)
  local tx, ty, tz = ux * cp - fx * sp, uy * cp - fy * sp, uz * cp - fz * sp
  local ax, ay, az = fx * cp + ux * sp, fy * cp + uy * sp, fz * cp + uz * sp
  return write_f32s(DIR, rx, -ry, -rz, -tx, ty, tz, -ax, ay, az)
end

-- KPAD's pointer: where the point the revolver's barrel aims at, aim_distance ahead, is on the
-- game's screen through its projection.
local function write_pointer(renderer, scale)
  local p00, _, _, _, _, p11 = f32s(renderer + PROJECTION_OFFSET, 6)
  local h = hand_matrix(REVOLVER)
  local bx, by, bz = f32s(h, 3)
  local ox, oy, oz = f32s(h + 0x30, 3)
  if not p00 or not bx or not ox then return false end
  local d = settings.aim_distance * scale
  local x, y, z = ox + bx * d, oy + by * d, oz + bz * d
  if z < 0.01 then return false end
  return write_f32s(POS, p00 * x / z, -p11 * y / z)
end

local function track()
  local ok, px, py, pz, qx, qy, qz, qw = hand_pose()
  local renderer = u32(RENDERER)
  if not ok or not renderer or renderer < 0x80000000 or renderer >= 0x81800000 then
    stop_tracking()
    return
  end

  -- The drawn weapon: the rotation with the offsets (yaw, pitch, roll in the hand's axes).
  local h = rad(settings.yaw) * 0.5
  local x, y, z, w = qmul(qx, qy, qz, qw, 0, sin(h), 0, cos(h))
  h = rad(settings.pitch) * 0.5
  x, y, z, w = qmul(x, y, z, w, sin(h), 0, 0, cos(h))
  h = rad(settings.roll) * 0.5
  x, y, z, w = qmul(x, y, z, w, 0, 0, sin(h), cos(h))
  local t = target
  t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9] = axes(x, y, z, w)
  -- The position in the game's view axes and units, with the calibration offsets (meters:
  -- x right, y up, z back).
  local scale = vr.units_per_meter() * settings.scale
  t[10] = (px + settings.offset_x) * scale
  t[11] = (py + settings.offset_y) * scale
  t[12] = -(pz + settings.offset_z) * scale
  smooth(settings.smoothing)

  local slot = 0
  for wi, weapon in ipairs(WEAPONS) do
    -- The cave counts the weapon's draws whenever its models are there, and moves it when its
    -- setting is on.
    local on = valid_model(weapon.body)
    for _, model in ipairs(weapon.parts) do
      if not valid_model(model) then on = false end
    end
    on = on and write_hand(wi)
    local move = on and settings[weapon.setting]
    mem.write_u32(MOVES + (wi - 1) * 4, move and 1 or 0)
    -- Last: arms the cave for this weapon's models.
    mem.write_u32(MODELS + slot * 4, on and weapon.body or 0)
    slot = slot + 1
    for _, model in ipairs(weapon.parts) do
      mem.write_u32(MODELS + slot * 4, on and model or 0)
      slot = slot + 1
    end
  end

  -- The game's Wii Remote: the true direction while the sword is out, the true aim while the
  -- revolver is.
  local flags = 0
  if settings.true_pose and drawn_recently(SWORD) and write_direction(qx, qy, qz, qw) then
    flags = flags | 1
  end
  if settings.true_aim and drawn_recently(REVOLVER) and
     write_pointer(renderer, scale) then
    flags = flags | 2
  end
  mem.write_u32(KPAD_FLAGS, flags)
  mem.write_u32(KPAD_ARMED, flags ~= 0 and KPAD_STATUS or 0)
  tracking = true
end

-- ---- Hooks ----

function on_load()
  disarm()
  forget_bodies()
  read_remote_pitch()
  install()
end

function on_state_loaded()
  disarm()
  forget_bodies()
  tracking = false
  has_smoothed = false
  installed = false
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then
    install()
    read_remote_pitch()
  end
  update_drawn()
  if not installed then
    stop_tracking()
  else
    track()
  end
  update_arms()
end

function on_unload()
  uninstall()
end
