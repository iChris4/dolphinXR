-- Metroid Prime (NTSC-U 1.00 and 1.02, GameCube): change visors with a controller next to the
-- head.
--
-- A DolphinXR VR game patch, after PrimedGun's visor gesture (PrimedGun, created by Nobbie):
-- bring a controller next to your head and push its thumbstick towards a visor. The directions
-- are the game's D-pad, as the HUD's visor icons show them: up the combat visor, left scan, down
-- thermal, right X-ray.
--
-- The patch changes visors as the game does for a D-pad press (CPlayer::UpdateVisorState in the
-- decompilation): in first person, not while a visor change is in progress, while scanning or
-- grappling, and only to a visor whose powerup the player owns, it sets the visor the game
-- changes to (CPlayerState::x18_transitioningVisor) and lowers the arm cannon for the scan visor
-- (CPlayer::HolsterGun); the game raises it again by itself after the scan visor. It presses no
-- button. A push while the game cannot change visors waits until it can.
--
-- The thumbstick belongs to the gesture once it was centred next to the head (a hand that comes
-- with its thumbstick pushed keeps moving the player): the patch then holds released what the
-- stock profile puts on it (the control stick for the left hand, the C-stick and D-pad for the
-- right one) and raises the flag thumbstick_left_gesture or thumbstick_right_gesture. The
-- dual-stick patch follows the left one and drives the control stick itself (it raises
-- main_stick_follows_gestures): the gesture then leaves the control stick to it, so the right
-- thumbstick still turns.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Change visors next to your head",
  author = "DolphinXR, after PrimedGun (Nobbie)",
  version = "1.1",
  description = "Bring a controller next to your head and push its thumbstick towards a visor, as "
    .. "on the D-pad: up combat, left scan, down thermal, right X-ray. For Metroid Prime NTSC-U "
    .. "1.00 and 1.02 (GameCube).",
  games = {"GM8E01"},
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

-- Per disc revision, recognized by its code at the movement sites dual-stick.lua hooks (the
-- game's instructions or that patch's branches) and the instructions after them. The state
-- manager as in metroid-state.lua; 1.02's player fields are 0x10 further than 1.00's.
local REVISIONS = {
  {name = "Metroid Prime 1.00", state_manager = 0x8045A1A8, player_offset = 0,
   sites = {{0x80286C80, 0x801E0304, 0x2C000000}, {0x80287590, 0xC002BDA0, 0xEC21002A}}},
  {name = "Metroid Prime 1.02", state_manager = 0x8045B208, player_offset = 0x10,
   sites = {{0x8028760C, 0x801E0314, 0x2C000000}, {0x80287F1C, 0xC002BDA8, 0xEC21002A}}},
}

-- The state manager's player, player state (behind a reference) and pending transition (1 on the
-- map, 2 on the pause screens).
local SM_PLAYER, SM_PLAYER_STATE, SM_TRANSITION = 0x84C, 0x8B8, 0xF90
-- CPlayer (1.00): camera state (0 first person), morph ball state, orbit state, scan state,
-- grapple state, arm cannon holster state and its time.
local CAMERA, BALL, ORBIT, SCAN, GRAPPLE, HOLSTER = 0x2F4, 0x2F8, 0x304, 0x3A8, 0x3B8, 0x498
local ORBIT_GRAPPLE = 5
local GUN_DRAWING, GUN_DRAWN, GUN_HOLSTERING = 1, 2, 3
local HOLSTER_TIME = 0.2   -- the game's (CTweakPlayerGun), as measured in NTSC-U
local DRAW_TIME = 0.45
-- CPlayerState: the current visor, the visor it changes to, the change's progress (0.2 when done)
-- and the powerups (41 of them, each its amount and capacity).
local VISOR, NEXT_VISOR, VISOR_FACTOR, POWERUP_COUNT, POWERUPS = 0x14, 0x18, 0x1C, 0x24, 0x28
local VISOR_DONE = 0.2 - 1e-6
local SCAN_VISOR = 2

-- Per thumbstick direction: the visor (0 combat, 1 X-ray, 2 scan, 3 thermal) and its powerup.
local DIRECTIONS = {up = {0, 17}, left = {2, 5}, down = {3, 9}, right = {1, 13}}

-- Whether site {address, original, after} holds the game's code or a branch in its place.
local function site_present(site)
  if u32(site[1] + 4) ~= site[3] then return false end
  local word = u32(site[1])
  return word == site[2] or (word ~= nil and word & 0xFC000003 == 0x48000000)
end

local function detect()
  for _, r in ipairs(REVISIONS) do
    if site_present(r.sites[1]) and site_present(r.sites[2]) then return r end
  end
  return nil
end

-- The player in first person and playing (not morphed, no map or pause screen) and its player
-- state, or nil.
local function playing(r)
  local sm = r.state_manager
  local player = ptr(sm + SM_PLAYER)
  if not player or u32(sm + SM_TRANSITION) ~= 0 then return nil end
  local o = r.player_offset
  if u32(player + CAMERA + o) ~= 0 or u32(player + BALL + o) ~= 0 then return nil end
  local state = ptr(sm + SM_PLAYER_STATE, 0)
  if not state or u32(state + POWERUP_COUNT) ~= 41 then return nil end
  return player, state
end

-- CPlayer::HolsterGun: from drawn, the game's holster time; from drawing, what the draw did.
local function holster(player, o)
  local address = player + HOLSTER + o
  local gun = u32(address)
  if gun == GUN_DRAWN then
    mem.write_f32(address + 4, HOLSTER_TIME)
  elseif gun == GUN_DRAWING then
    mem.write_f32(address + 4, HOLSTER_TIME * (1 - (f32(address + 4) or 0) / DRAW_TIME))
  else
    return
  end
  mem.write_u32(address, GUN_HOLSTERING)
end

-- Changes to the direction's visor. False while the game cannot change visors (try again).
local function select_visor(r, player, state, dir)
  local visor, item = DIRECTIONS[dir][1], DIRECTIONS[dir][2]
  local target = u32(state + NEXT_VISOR)
  local capacity = u32(state + POWERUPS + item * 8 + 4)
  if target == visor or not capacity or capacity == 0 or capacity >= 0x80000000 then return true end
  if u32(state + VISOR) ~= target or (f32(state + VISOR_FACTOR) or 0) < VISOR_DONE then
    return false
  end
  local o = r.player_offset
  if u32(player + SCAN + o) ~= 0 or u32(player + GRAPPLE + o) ~= 0 or
     u32(player + ORBIT + o) == ORBIT_GRAPPLE then
    return false
  end
  mem.write_u32(state + NEXT_VISOR, visor)
  if visor == SCAN_VISOR then holster(player, o) end
  if settings.haptics then vr.haptic(settings.hand == 1 and "right" or "left", 0.4, 0.04) end
  log(r.name .. ": visor", visor)
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
local holding_stick = false   -- the control stick held released (the left hand's)
local armed = false        -- the thumbstick came back to the centre since the last pick
local warned = false

local function hand_name()
  return settings.hand == 1 and "right" or "left"
end

-- The left hand's control stick held released, unless a patch drives it (dual-stick).
local function hold_stick()
  local hold = owned == "left" and not vr.flag("main_stick_follows_gestures")
  if hold == holding_stick then return end
  holding_stick = hold
  if hold then pad.set_stick(1, "Main Stick", 0, 0) else pad.release(1, "Main Stick") end
end

-- The game's use of the hand's thumbstick held released, and the flag raised; or both undone.
local function take(hand)
  if hand == owned then return end
  if owned then
    vr.set_flag("thumbstick_" .. owned .. "_gesture", false)
    if owned == "right" then
      pad.release(1, "C-Stick")
      for _, name in ipairs(DPAD) do pad.release(1, name) end
    end
  end
  owned = hand
  if hand then
    vr.set_flag("thumbstick_" .. hand .. "_gesture", true)
    if hand == "right" then
      pad.set_stick(1, "C-Stick", 0, 0)
      for _, name in ipairs(DPAD) do pad.set_value(1, name, 0) end
    end
  end
  hold_stick()
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
  local r = detect()
  -- Not while the game boots.
  if not r and not warned and game.field() > 600 then
    warn("no code of Metroid Prime NTSC-U 1.00 or 1.02 at the movement sites: visors unchanged")
    warned = true
  end
  local player, state
  if r then player, state = playing(r) end
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
  hold_stick()
  if magnitude < REARM then
    armed = true
    return
  end
  local dir = direction(x, y)
  if dir and armed and select_visor(r, player, state, dir) then armed = false end
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
