-- Metroid Prime (NTSC-U 1.00 and 1.02, GameCube): change beams with a move of the cannon hand.
--
-- A DolphinXR VR game patch, after PrimedGun's beam select (PrimedGun, created by Nobbie): hold a
-- button of the cannon hand (B by default), move or point that controller up, down, left or right,
-- and let go: the arm cannon changes to the beam in that direction. The directions are the game's
-- C-stick: up the Power Beam, right Wave, down Ice, left Plasma. Back in the centre before letting
-- go changes nothing. A light vibration marks each direction the hand moves to (setting).
--
-- PrimedGun's gesture: when the button is pressed, a panel is set 26 cm in front of the
-- controller, 5.5 cm above its aim, facing it and level (without the controller's roll). The
-- direction is where the controller's aim ray meets that panel, in steps of 21 cm, from where it
-- met it at first (a controller whose ray misses the panel counts its move instead, in steps of
-- 7.5 cm); a quarter of a step picks a direction, along the larger of the two axes.
--
-- On letting go the patch pushes the C-stick that way for a few fields, as PrimedGun does, and the
-- game changes the beam as for a C-stick push (CPlayerGun::HandleBeamChange): only to a beam the
-- player owns and not during a beam change; the current beam's direction leaves missile mode.
-- Meanwhile it raises the flag c_stick_gesture, which the dual-stick patch follows: it does not
-- hold the C-stick released then. In first person only: not in morph ball, on the map or pause
-- screens, or with the VR menu open. It needs no scratch memory.
--
-- While it can change beams it also raises the flag beam_swap_<hand>_<button> (e.g.
-- beam_swap_right_secondary), and for a few fields after the button is let go without a direction
-- the flag beam_swap_tap: Modern controls (dual-stick.lua) then gives that button's game action (B,
-- the jump, in the stock profile) only on such a tap, never when a gesture starts.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

local BUTTONS = {"B or Y", "Grip", "Thumbstick click"}

patch = {
  title = "Beam quick swap",
  author = "DolphinXR, after PrimedGun (Nobbie)",
  version = "1.1",
  description = "Hold B on the cannon hand, move or point the controller up, down, left or right, "
    .. "and let go to change beams, as on the C-stick: up Power, right Wave, down Ice, left "
    .. "Plasma. For Metroid Prime NTSC-U 1.00 and 1.02 (GameCube).",
  games = {"GM8E01"},
  settings = {
    {name = "hand", label = "Hand", type = "choice", options = {"Left", "Right"}, default = 1,
     description = "The cannon hand (PrimedGun's default: right)."},
    {name = "button", label = "Button", type = "choice", options = BUTTONS, default = 0,
     description = "Held while the hand moves; use one your controller profile leaves free."},
    {name = "haptics", label = "Vibrate on each direction", type = "bool", default = true},
  },
}

-- ---- The game ----

local u32, ptr = mem.u32, mem.ptr

-- Per disc revision, recognized by its code at the movement sites dual-stick.lua hooks (the
-- game's instructions or that patch's branches) and the instructions after them. The state
-- manager as in metroid-state.lua; 1.02's player fields are 0x10 further than 1.00's.
local REVISIONS = {
  {name = "Metroid Prime 1.00", state_manager = 0x8045A1A8, player_offset = 0,
   sites = {{0x80286C80, 0x801E0304, 0x2C000000}, {0x80287590, 0xC002BDA0, 0xEC21002A}}},
  {name = "Metroid Prime 1.02", state_manager = 0x8045B208, player_offset = 0x10,
   sites = {{0x8028760C, 0x801E0314, 0x2C000000}, {0x80287F1C, 0xC002BDA8, 0xEC21002A}}},
}

-- The state manager's player and pending transition (1 on the map, 2 on the pause screens); the
-- player's (1.00) camera state (0 first person) and morph ball state.
local SM_PLAYER, SM_TRANSITION = 0x84C, 0xF90
local CAMERA, BALL = 0x2F4, 0x2F8

-- Per direction: the C-stick's push and its beam (CPlayerGun's mBeamCtrlCmd, the game's controls).
local STICK = {up = {0, 1}, right = {1, 0}, down = {0, -1}, left = {-1, 0}}
local BEAMS = {up = "Power", right = "Wave", down = "Ice", left = "Plasma"}

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

-- The player in first person and playing (not morphed, no map or pause screen).
local function playing(r)
  local sm = r.state_manager
  local player = ptr(sm + SM_PLAYER)
  if not player or u32(sm + SM_TRANSITION) ~= 0 then return false end
  local o = r.player_offset
  return u32(player + CAMERA + o) == 0 and u32(player + BALL + o) == 0
end

-- ---- The gesture ----

local BUTTON_NAMES = {[0] = "secondary", [1] = "squeeze", [2] = "thumbstick"}
-- PrimedGun's panel: its centre from the controller (forward, up), its step, how far from the
-- centre the ray still counts as on it, and the step of a move when the ray misses it.
local PANEL_AHEAD, PANEL_ABOVE, PANEL_STEP, PANEL_EDGE = 0.26, 0.055, 0.21, 1.8
local MOVE_STEP = 0.075
local PICK = 0.25          -- the steps that pick a direction
local PUSH_FIELDS = 8      -- how long the C-stick is pushed after letting go
local TAP_FIELDS = 6       -- how long a tap's flag stays up

local raw_pose, sqrt, abs = vr.raw_pose, math.sqrt, math.abs

-- While the button is held: the controller's position and level frame when it was pressed (right,
-- up, forward), the panel's centre, the first reading and the direction picked so far.
local open = false
local bx, by, bz = 0, 0, 0
local rx, rz = 1, 0
local ux, uy, uz = 0, 1, 0
local fx, fy, fz = 0, 0, -1
local cx, cy, cz = 0, 0, 0
local first_x, first_y = nil, nil
local hovered = nil
local pushing = false      -- the C-stick pushed after letting go
local push_left = 0        -- for that many more fields
local moved = false        -- a direction was picked during this hold
local tap_left = 0         -- fields left of a tap's flag
local owned = nil          -- the flag naming the button while the patch uses it
local warned = false

local function hand_name()
  return settings.hand == 0 and "left" or "right"
end

-- The aim's forward (its -Z axis) from its orientation.
local function forward(qx, qy, qz, qw)
  return -2 * (qx * qz + qw * qy), -2 * (qy * qz - qw * qx), 2 * (qx * qx + qy * qy) - 1
end

local function begin(px, py, pz, qx, qy, qz, qw)
  fx, fy, fz = forward(qx, qy, qz, qw)
  -- Level: the right axis horizontal, up = right x forward.
  local x, z = -fz, fx
  local length = sqrt(x * x + z * z)
  if length < 1e-4 then x, z, length = 1, 0, 1 end
  rx, rz = x / length, z / length
  ux, uy, uz = -rz * fy, rz * fx - rx * fz, rx * fy
  bx, by, bz = px, py, pz
  cx, cy, cz = px + ux * PANEL_ABOVE + fx * PANEL_AHEAD, py + uy * PANEL_ABOVE + fy * PANEL_AHEAD,
               pz + uz * PANEL_ABOVE + fz * PANEL_AHEAD
  first_x, first_y = nil, nil
  hovered = nil
  open = true
end

-- Where the aim points on the panel, or how far the controller moved, in steps.
local function reading(px, py, pz, qx, qy, qz, qw)
  local ax, ay, az = forward(qx, qy, qz, qw)
  local facing = ax * fx + ay * fy + az * fz
  if abs(facing) > 0.025 then
    local t = ((cx - px) * fx + (cy - py) * fy + (cz - pz) * fz) / facing
    if t > 0.02 and t < 2 then
      local hx, hy, hz = px + ax * t - cx, py + ay * t - cy, pz + az * t - cz
      local x = (hx * rx + hz * rz) / PANEL_STEP
      local y = (hx * ux + hy * uy + hz * uz) / PANEL_STEP
      if abs(x) <= PANEL_EDGE and abs(y) <= PANEL_EDGE then return x, y end
    end
  end
  local dx, dy, dz = px - bx, py - by, pz - bz
  return (dx * rx + dz * rz) / MOVE_STEP, (dx * ux + dy * uy + dz * uz) / MOVE_STEP
end

local function direction(x, y)
  if abs(x) < PICK and abs(y) < PICK then return nil end
  if abs(x) >= abs(y) then return x < 0 and "left" or "right" end
  return y < 0 and "down" or "up"
end

local function stop_push()
  if pushing then
    pad.release(1, "C-Stick")
    vr.set_flag("c_stick_gesture", false)
    pushing = false
  end
end

local function push(dir)
  local stick = STICK[dir]
  pad.set_stick(1, "C-Stick", stick[1], stick[2])
  vr.set_flag("c_stick_gesture", true)
  pushing = true
  push_left = PUSH_FIELDS
  if settings.haptics then vr.haptic(hand_name(), 0.5, 0.05) end
  log(BEAMS[dir] .. " Beam")
end

-- The flag naming the button (beam_swap_<hand>_<button>) while the patch uses it.
local function own(on)
  local name = on and ("beam_swap_" .. hand_name() .. "_" .. BUTTON_NAMES[settings.button]) or nil
  if name == owned then return end
  if owned then vr.set_flag(owned, false) end
  if name then vr.set_flag(name, true) end
  owned = name
end

local function cancel()
  open = false
  hovered = nil
  stop_push()
end

function on_frame()
  if pushing then
    push_left = push_left - 1
    if push_left <= 0 then stop_push() end
  end
  local r = detect()
  -- Not while the game boots.
  if not r and not warned and game.field() > 600 then
    warn("no code of Metroid Prime NTSC-U 1.00 or 1.02 at the movement sites: beams unchanged")
    warned = true
  end
  if tap_left > 0 then
    tap_left = tap_left - 1
    if tap_left == 0 then vr.set_flag("beam_swap_tap", false) end
  end
  local hand = hand_name()
  local ok, px, py, pz, qx, qy, qz, qw = raw_pose(hand)
  if not r or not playing(r) or vr.menu_open() or not ok then
    cancel()
    own(false)
    return
  end
  own(true)
  if vr.button(hand, BUTTON_NAMES[settings.button]) then
    if not open then
      begin(px, py, pz, qx, qy, qz, qw)
      moved = false
    end
    local x, y = reading(px, py, pz, qx, qy, qz, qw)
    if not first_x then first_x, first_y = x, y end
    x = math.max(-1, math.min(1, x - first_x))
    y = math.max(-1, math.min(1, y - first_y))
    local dir = direction(x, y)
    if dir ~= hovered then
      hovered = dir
      if dir then
        moved = true
        if settings.haptics then vr.haptic(hand, 0.2, 0.02) end
      end
    end
  elseif open then
    open = false
    if hovered then
      push(hovered)
    elseif not moved then
      -- A tap: the button's own action (Modern controls).
      vr.set_flag("beam_swap_tap", true)
      tap_left = TAP_FIELDS
    end
    hovered = nil
  end
end

function on_state_loaded()
  cancel()
end

function on_setting_changed()
  cancel()
end

function on_unload()
  cancel()
end
