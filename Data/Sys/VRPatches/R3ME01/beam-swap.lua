-- Metroid Prime Trilogy (NTSC-U, Wii): change beams with a move of the cannon hand, in Metroid
-- Prime and Echoes.
--
-- A DolphinXR VR game patch, after PrimedGun's beam select for Metroid Prime (PrimedGun, created
-- by Nobbie), the Trilogy's version of G2ME01/beam-swap.lua: hold a button of the cannon hand (B
-- by default), move or point that controller up, down, left or right, and let go: the arm cannon
-- changes to the beam in that direction. The directions follow the games' own beam menu (hold +):
-- up, left and right the beams the menu shows there (Prime: Wave, Ice, Plasma; Echoes:
-- Annihilator, Dark, Light), down the Power Beam (the menu's centre). Back in the centre before
-- letting go changes nothing. Corruption has no beams to change: the patch leaves it alone.
--
-- PrimedGun's gesture: when the button is pressed, a panel is set 26 cm in front of the
-- controller, 5.5 cm above its aim, facing it and level (without the controller's roll). The
-- direction is where the controller's aim ray meets that panel, in steps of 21 cm, from where it
-- met it at first (a controller whose ray misses the panel counts its move instead, in steps of
-- 7.5 cm); a quarter of a step picks a direction, along the larger of the two axes.
--
-- Each game reads its beam controls in its gun's input code, a loop over its beam table that
-- leaves the beam to change to in a register (PrimeHack's beam change hook, shiiion/dolphin). A
-- cave before that loop takes the beam this patch asks for instead, once; the game then changes
-- beams as for its menu: only to a beam the player owns, not to the current one. Each game is its
-- own program on the disc, recognized by the code at its hook. In first person only: not in morph
-- ball, on the map or pause screens, or with the VR menu open.
--
-- The button usually has a game action of its own (Modern controls, stick-turn.lua, puts the Wii
-- Remote's + on right B by default; the stock Quest Touch profile puts 2, the pause screen, there).
-- The Wii Remote reads it before the patch sees the press, so while playing Prime or Echoes that
-- Wii Remote button is held released, and a tap of the button without a direction presses it when
-- the button is let go; elsewhere (the pause screens, the map, Corruption, the Trilogy's menus) it
-- is the user's.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

local BUTTONS = {"B or Y", "Grip", "Thumbstick click"}
local WII_BUTTONS = {"None", "1", "2", "-", "+", "A", "B", "D-Pad/Down"}

patch = {
  title = "Beam quick swap",
  author = "DolphinXR, after PrimedGun (Nobbie), with PrimeHack's hook points",
  version = "1.0",
  description = "Hold B on the cannon hand, move or point the controller up, down, left or right, "
    .. "and let go to change beams, as in the game's beam menu: up, left and right its beams, "
    .. "down Power. A tap of the button still does what it did. For Metroid Prime and Echoes in "
    .. "Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
  scratch = 0x40,
  settings = {
    {name = "hand", label = "Hand", type = "choice", options = {"Left", "Right"}, default = 1,
     description = "The cannon hand (PrimedGun's default: right)."},
    {name = "button", label = "Button", type = "choice", options = BUTTONS, default = 0},
    {name = "wii_button", label = "Its Wii Remote button", type = "choice", options = WII_BUTTONS,
     default = 4,
     description = "What your controller profile or Modern controls put on that button (Modern "
       .. "controls: + on right B; the stock Quest Touch profile: 2 on right B): held "
       .. "released during a gesture, pressed on a tap."},
    {name = "haptics", label = "Vibrate on each direction", type = "bool", default = true},
  },
}

-- ---- The games ----

local u32, ptr = mem.u32, mem.ptr

-- Per game: the hook (the instruction before the beam loop, which the cave runs: lis r27, 0x8047
-- and lis r26, 0x804A, written with the signed immediate), the next instruction, the loop's end
-- and the registers of the beam and its powerup there; the beam per direction (beam = powerup in
-- both games); and:
--   player()          the player in first person and playing (no map, pause screen), or nil
--   owned(player, b)  whether beam b's powerup count is not 0
--   beam(player)      the gun's current beam
local GAMES = {
  {
    name = "Metroid Prime",
    hook = 0x8018E538, original = 0x3F608047, replaced = "lis r27, -32697", after = 0x3B000000,
    done = 0x8018E5AC, beam_reg = "r26", item_reg = "r25",
    beams = {up = 2, left = 1, right = 3, down = 0},
    names = {[0] = "Power", "Ice", "Wave", "Plasma"},
    player = function()
      local player = ptr(0x804BF420 + 0x84C)
      if not player or u32(0x804BF420 + 0x117C) ~= 0 then return nil end
      if u32(player + 0x2F0) ~= 0 or u32(player + 0x2F4) ~= 0 then return nil end
      return player
    end,
    owned = function(_, b)
      local powerups = ptr(0x804BFCD4)
      local count = powerups and u32(powerups + b * 8 + 0x30)
      return count ~= nil and count ~= 0
    end,
    beam = function(player)
      local gun = ptr(player + 0x480)
      return gun and u32(gun + 0x310)
    end,
  },
  {
    name = "Metroid Prime 2: Echoes",
    hook = 0x8018CC7C, original = 0x3F40804A, replaced = "lis r26, -32694", after = 0x3AC00000,
    done = 0x8018CCF4, beam_reg = "r31", item_reg = "r30",
    beams = {up = 3, left = 1, right = 2, down = 0},
    names = {[0] = "Power", "Dark", "Light", "Annihilator"},
    player = function()
      local player = ptr(0x804E72E8 + 0x14F4)
      if not player or u32(0x804E72E8 + 0x2C0C) ~= 0 then return nil end
      if u32(player + 0x374) ~= 0 or u32(player + 0x378) ~= 0 then return nil end
      return player
    end,
    owned = function(player, b)
      local powerups = ptr(player + 0x12EC)
      local count = powerups and u32(powerups + b * 12 + 0x5C)
      return count ~= nil and count ~= 0
    end,
    beam = function(player)
      local gun = ptr(player + 0xEA8)
      return gun and u32(gun + 0x768)
    end,
  },
}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local REQUEST = S + 0x00  -- the beam asked for + 1; 0 = none (the cave clears it)
local CAVE = S + 0x10

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- With a request: its beam in the loop's registers, the request cleared, past the loop. Without:
-- the game's instruction and its loop. r0, r3 and r4 hold nothing there (the loop sets r3 and r4
-- first, the code after it r0 and r3).
local function cave_words(g)
  local text = load_address("r4", REQUEST) .. string.format([[
lwz r3, 0(r4)
cmpwi r3, 0
beq game
li r0, 0
stw r0, 0(r4)
addi %s, r3, -1
mr %s, %s
b `0x%X`
game:
%s
b `0x%X`
]], g.beam_reg, g.item_reg, g.beam_reg, g.done, g.replaced, g.hook + 4)
  local words, err = asm.assemble(text, CAVE)
  if not words then error("assembling the cave for " .. g.name .. ": " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  assert(words[#words - 1] == g.original, "the cave does not run the game's instruction")
  return words
end

-- ---- Finding the game, installing and removing the hook ----

local current = nil       -- the game in GAMES whose hook is installed
local caves = {}          -- the built cave per game

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- Whether g's program is in memory: its hook holds the game's instruction or the branch to the
-- cave, followed by the game's next instruction.
local function present(g)
  if u32(g.hook + 4) ~= g.after then return false end
  local word = u32(g.hook)
  return word == g.original or branch_target(g.hook, word or 0) == CAVE
end

local function install()
  local found = nil
  for _, g in ipairs(GAMES) do
    if present(g) then found = g end
  end
  if found ~= current then mem.write_u32(REQUEST, 0) end
  if not found then
    current = nil
    return
  end
  caves[found] = caves[found] or cave_words(found)
  local changed = mem.ensure_code(CAVE, caves[found])
  changed = mem.ensure_code(found.hook, asm.branch(found.hook, CAVE)) or changed
  if changed or found ~= current then
    log(found.name .. ": hook installed, cave at", mem.hex(CAVE))
  end
  current = found
end

local function uninstall()
  local g = current
  current = nil
  mem.write_u32(REQUEST, 0)
  if g and branch_target(g.hook, u32(g.hook) or 0) == CAVE then
    mem.write_code(g.hook, g.original)
  end
end

-- ---- The gesture ----

local BUTTON_NAMES = {[0] = "secondary", [1] = "squeeze", [2] = "thumbstick"}
-- PrimedGun's panel: its centre from the controller (forward, up), its step, how far from the
-- centre the ray still counts as on it, and the step of a move when the ray misses it.
local PANEL_AHEAD, PANEL_ABOVE, PANEL_STEP, PANEL_EDGE = 0.26, 0.055, 0.21, 1.8
local MOVE_STEP = 0.075
local PICK = 0.25          -- the steps that pick a direction
local REQUEST_FIELDS = 10  -- a request the game has not taken by then is dropped
local TAP_FIELDS = 6       -- how long a tap presses the Wii Remote button

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
local moved = false        -- a direction was picked during this hold
local was_pressed = false  -- the button, last field
local request_field = nil  -- the field of the pending request
local wii_held = false     -- the patch holds the Wii Remote button
local tap_left = 0         -- fields left of a tap

local function hand_name()
  return settings.hand == 0 and "left" or "right"
end

local function wii_button()
  local name = WII_BUTTONS[settings.wii_button + 1]
  return name ~= "None" and name or nil
end

-- The Wii Remote button: held released (0), pressed (1) or the user's again (nil).
local function hold_wii(value)
  local name = wii_button()
  if not name then return end
  if value then
    wiimote.set_value(1, name, value)
    wii_held = true
  elseif wii_held then
    wiimote.release(1, name)
    wii_held = false
  end
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
  moved = false
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

-- Asks the game for the beam of a direction, if the player owns it and it is not the current one.
local function swap(g, player, dir)
  local b = g.beams[dir]
  if not g.owned(player, b) or g.beam(player) == b then return end
  mem.write_u32(REQUEST, b + 1)
  request_field = game.field()
  if settings.haptics then vr.haptic(hand_name(), 0.5, 0.05) end
  log(g.names[b] .. " Beam")
end

local function release_all()
  open = false
  hovered = nil
  tap_left = 0
  hold_wii(nil)
end

function on_load()
  mem.write_u32(REQUEST, 0)
  install()
end

function on_state_loaded()
  current = nil
  release_all()
  install()
end

function on_frame()
  local field = game.field()
  -- Another game of the disc may have started: its program replaced the hooked one.
  if field % 60 == 0 or not current or not present(current) then install() end
  if request_field and (u32(REQUEST) == 0 or field - request_field >= REQUEST_FIELDS) then
    mem.write_u32(REQUEST, 0)
    request_field = nil
  end
  if tap_left > 0 then tap_left = tap_left - 1 end

  local hand = hand_name()
  local pressed = not vr.menu_open() and vr.button(hand, BUTTON_NAMES[settings.button])
  local g = current
  local player = g and g.player()
  -- The Wii Remote button: pressed during a tap, held released while playing, else the user's.
  if tap_left > 0 then
    hold_wii(1)
  elseif player then
    hold_wii(0)
  else
    hold_wii(nil)
  end
  local ok, px, py, pz, qx, qy, qz, qw = raw_pose(hand)
  if not player or not ok then
    open = false
    hovered = nil
    was_pressed = pressed
    return
  end
  if pressed then
    -- Only a press while playing starts a gesture (one held from a pause screen does not).
    if not open and not was_pressed and tap_left == 0 then begin(px, py, pz, qx, qy, qz, qw) end
    if open then
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
    end
  elseif open then
    open = false
    if hovered then
      swap(g, player, hovered)
    elseif not moved and wii_button() then
      -- A tap: the button's own action.
      hold_wii(1)
      tap_left = TAP_FIELDS
    end
    hovered = nil
  end
  was_pressed = pressed
end

function on_setting_changed(name)
  if name == "wii_button" then
    -- The previous choice's hold.
    local index = settings.wii_button
    for i, button in ipairs(WII_BUTTONS) do
      if i - 1 ~= index and button ~= "None" then wiimote.release(1, button) end
    end
    wii_held = false
  end
  release_all()
end

function on_unload()
  release_all()
  uninstall()
end
