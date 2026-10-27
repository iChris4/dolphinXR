-- Metroid Prime 2: Echoes (NTSC-U, GameCube): change visors with a controller next to the head.
--
-- A DolphinXR VR game patch, after PrimedGun's visor gesture for Metroid Prime (PrimedGun,
-- created by Nobbie): bring a controller next to your head and push its thumbstick towards a
-- visor. The directions are the game's D-pad, as the HUD's visor icons show them: up the combat
-- visor, left scan, down echo, right dark.
--
-- The patch changes visors as the game does for a D-pad press, with the fields found in the game
-- and PrimeHack's Echoes tables (shiiion/dolphin, PRIME_2: the visor the game changes to, the
-- powerups): when no visor change is in progress, and only to a visor the player owns, it sets the
-- visor the game changes to (the player state's word 0x34) and lowers the arm cannon for the scan
-- visor as the game does (holster state 3, 0.2 s); the game raises it again by itself after the
-- scan visor. It presses no button. A push while a visor change is in progress waits for its end.
--
-- The thumbstick belongs to the gesture once it was centred next to the head (a hand that comes
-- with its thumbstick pushed keeps moving the player): the patch then holds released what the
-- stock profile puts on it (the control stick for the left hand, the C-stick and D-pad for the
-- right one) and raises the flag thumbstick_left_gesture or thumbstick_right_gesture, which the
-- dual-stick patch follows.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Change visors next to your head",
  author = "DolphinXR, after PrimedGun (Nobbie), with PrimeHack's addresses",
  version = "1.0",
  description = "Bring a controller next to your head and push its thumbstick towards a visor, as "
    .. "on the D-pad: up combat, left scan, down echo, right dark. For Metroid Prime 2: Echoes "
    .. "NTSC-U (GameCube).",
  games = {"G2ME01"},
  revision = 0,
  settings = {
    {name = "hand", label = "Hand", type = "choice", options = {"Left", "Right"}, default = 0,
     description = "The hand without the arm cannon (PrimedGun's default)."},
    {name = "radius", label = "Head zone radius", unit = "m", default = 0.24, min = 0.12,
     max = 0.4, step = 0.01, description = "How close to the headset the controller must be."},
    {name = "haptics", label = "Vibrate on a visor change", type = "bool", default = true},
  },
}

-- ---- The game ----

local u32, f32, ptr = mem.u32, mem.f32, mem.ptr

-- The game is recognized by its code at the strafe site dual-stick.lua hooks (the game's
-- instruction or that patch's branch) and the instruction after it.
local SITE, SITE_ORIGINAL, SITE_AFTER = 0x801880FC, 0x801E03A4, 0x2C000000
-- The state manager, its player and pause screen (0 none); the player's camera state (0 first
-- person), morph ball state, orbit state, arm cannon (CPlayerGun) and player state, as in
-- metroid-state.lua and dual-stick.lua.
local STATE_MANAGER = 0x803DB6E0
local SM_PLAYER, SM_MENU = 0x14FC, 0x2470
local CAMERA, BALL, ORBIT, GUN, PLAYER_STATE = 0x388, 0x38C, 0x3A4, 0xEBC, 0x1314
local ORBIT_GRAPPLE = 5   -- as in Metroid Prime
-- The arm cannon's holster state and time (as Metroid Prime's CPlayer::x498_gunHolsterState).
local GUN_HOLSTER, GUN_HOLSTER_TIME = 0x3A8, 0x388
local GUN_DRAWING, GUN_DRAWN, GUN_HOLSTERING = 1, 2, 3
local HOLSTER_TIME = 0.2   -- the game's, as measured
local DRAW_TIME = 0.45
-- The player state: the current visor, the visor it changes to, the change's progress (0.2 when
-- done) and the powerups (109 of them, each its amount, capacity and a third word).
local VISOR, NEXT_VISOR, VISOR_FACTOR, POWERUP_COUNT, POWERUPS = 0x30, 0x34, 0x50, 0x58, 0x5C
local VISOR_DONE = 0.2 - 1e-6
local SCAN_VISOR = 2

-- Per thumbstick direction: the visor (0 combat, 1 echo, 2 scan, 3 dark) and its powerup.
local DIRECTIONS = {up = {0, 0x08}, left = {2, 0x09}, down = {1, 0x0B}, right = {3, 0x0A}}

local function present()
  if u32(SITE + 4) ~= SITE_AFTER then return false end
  local word = u32(SITE)
  return word == SITE_ORIGINAL or (word ~= nil and word & 0xFC000003 == 0x48000000)
end

-- The player in first person and playing (not morphed, no map or pause screen) and its player
-- state, or nil.
local function playing()
  local player = ptr(STATE_MANAGER + SM_PLAYER)
  if not player or u32(STATE_MANAGER + SM_MENU) ~= 0 then return nil end
  if u32(player + CAMERA) ~= 0 or u32(player + BALL) ~= 0 then return nil end
  local state = ptr(player + PLAYER_STATE)
  if not state or u32(state + POWERUP_COUNT) ~= 109 then return nil end
  return player, state
end

-- As Metroid Prime's CPlayer::HolsterGun: from drawn, the game's holster time; from drawing, what
-- the draw did.
local function holster(player)
  local gun_object = ptr(player + GUN)
  if not gun_object then return end
  local gun = u32(gun_object + GUN_HOLSTER)
  if gun == GUN_DRAWN then
    mem.write_f32(gun_object + GUN_HOLSTER_TIME, HOLSTER_TIME)
  elseif gun == GUN_DRAWING then
    local drawn = f32(gun_object + GUN_HOLSTER_TIME) or 0
    mem.write_f32(gun_object + GUN_HOLSTER_TIME, HOLSTER_TIME * (1 - drawn / DRAW_TIME))
  else
    return
  end
  mem.write_u32(gun_object + GUN_HOLSTER, GUN_HOLSTERING)
end

-- Changes to the direction's visor. False while the game cannot change visors (try again).
local function select_visor(player, state, dir)
  local visor, item = DIRECTIONS[dir][1], DIRECTIONS[dir][2]
  local target = u32(state + NEXT_VISOR)
  local capacity = u32(state + POWERUPS + item * 12 + 4)
  if target == visor or not capacity or capacity == 0 or capacity >= 0x80000000 then return true end
  if u32(state + VISOR) ~= target or (f32(state + VISOR_FACTOR) or 0) < VISOR_DONE or
     u32(player + ORBIT) == ORBIT_GRAPPLE then
    return false
  end
  mem.write_u32(state + NEXT_VISOR, visor)
  if visor == SCAN_VISOR then holster(player) end
  if settings.haptics then vr.haptic(settings.hand == 1 and "right" or "left", 0.4, 0.04) end
  log("visor", visor)
  return true
end

-- ---- The gesture ----

-- PrimedGun's head zone: within the radius of the headset, from a little below it to above it.
local ZONE_BELOW, ZONE_ABOVE = 0.18, 0.28
local ZONE_GRACE = 16    -- fields the zone lasts after the hand leaves it
local PICK, REARM = 0.5, 0.3   -- thumbstick magnitude that picks a visor, and that re-arms
local DPAD = {"D-Pad/Up", "D-Pad/Down", "D-Pad/Left", "D-Pad/Right"}

local raw_pose, axis = vr.raw_pose, vr.axis

local last_in_zone = nil   -- the field the hand was last next to the head
local owned = nil          -- the hand whose thumbstick the gesture uses, or nil
local armed = false        -- the thumbstick came back to the centre since the last pick
local warned = false

local function hand_name()
  return settings.hand == 1 and "right" or "left"
end

-- The game's use of the hand's thumbstick held released, and the flag raised; or both undone.
local function take(hand)
  if hand == owned then return end
  if owned then
    vr.set_flag("thumbstick_" .. owned .. "_gesture", false)
    if owned == "left" then
      pad.release(1, "Main Stick")
    else
      pad.release(1, "C-Stick")
      for _, name in ipairs(DPAD) do pad.release(1, name) end
    end
  end
  owned = hand
  if hand then
    vr.set_flag("thumbstick_" .. hand .. "_gesture", true)
    if hand == "left" then
      pad.set_stick(1, "Main Stick", 0, 0)
    else
      pad.set_stick(1, "C-Stick", 0, 0)
      for _, name in ipairs(DPAD) do pad.set_value(1, name, 0) end
    end
  end
end

local function near_head(field)
  local ok, hx, hy, hz = raw_pose("head")
  local hand_ok, px, py, pz = raw_pose(hand_name())
  if ok and hand_ok then
    local dx, dy, dz = px - hx, py - hy, pz - hz
    local r = settings.radius
    if dx * dx + dy * dy + dz * dz <= r * r and dy >= -ZONE_BELOW and dy <= ZONE_ABOVE then
      last_in_zone = field
      return true
    end
  end
  return last_in_zone ~= nil and field - last_in_zone < ZONE_GRACE
end

local function direction(x, y)
  if math.max(math.abs(x), math.abs(y)) < PICK then return nil end
  if math.abs(y) >= math.abs(x) then return y > 0 and "up" or "down" end
  return x > 0 and "right" or "left"
end

function on_frame()
  local field = game.field()
  local game_present = present()
  -- Not while the game boots.
  if not game_present and not warned and game.field() > 600 then
    warn("unexpected code at " .. mem.hex(SITE) .. ": is this Metroid Prime 2: Echoes NTSC-U? "
         .. "Visors unchanged")
    warned = true
  end
  local player, state
  if game_present then player, state = playing() end
  if not player or vr.menu_open() or not near_head(field) then
    take(nil)
    return
  end
  local hand = hand_name()
  local x, y = axis(hand, "thumbstick_x") or 0, axis(hand, "thumbstick_y") or 0
  local magnitude = math.max(math.abs(x), math.abs(y))
  if owned ~= hand then
    -- Not before the thumbstick was centred next to the head.
    if magnitude >= REARM then return end
    take(hand)
    armed = true
  end
  if magnitude < REARM then
    armed = true
    return
  end
  local dir = direction(x, y)
  if dir and armed and select_visor(player, state, dir) then armed = false end
end

function on_state_loaded()
  last_in_zone = nil
  take(nil)
end

function on_setting_changed(name)
  if name == "hand" then take(nil) end
end

function on_unload()
  take(nil)
end
