-- Metroid Prime 3: Corruption (NTSC-U, Wii): change visors with a controller next to the head.
--
-- A DolphinXR VR game patch, after PrimedGun's visor gesture for Metroid Prime (PrimedGun,
-- created by Nobbie): bring a controller next to your head and push its thumbstick towards a
-- visor: left the scan visor, up the command visor, right the X-ray visor, down back to the combat
-- visor. (The game's own visor menu has the scan visor up, the command visor right and the X-ray
-- visor left; these directions are kept as players learned them.)
--
-- The visor is the player's powerups' word 0x34 (0 combat, 1 scan, 2 command, 3 X-ray) and a
-- visor is owned when its powerup count (powerups + id * 12 + 0x58; ids 0x0B to 0x0E) is not 0:
-- PrimeHack's PRIME_3_STANDALONE tables, which switch visors the same way. While the gesture
-- picks a visor, the thumbstick's usual input is held released (the Nunchuk stick for the left
-- hand; for the right hand the Wii Remote buttons the stock OpenXR profile puts on its stick).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Change visors next to your head",
  author = "DolphinXR, after PrimedGun (Nobbie), with PrimeHack's addresses",
  version = "1.0",
  description = "Bring a controller next to your head and push its thumbstick towards a visor: "
    .. "left scan, up command, right X-ray, down combat. For Metroid Prime 3: Corruption NTSC-U "
    .. "(Wii).",
  games = {"RM3E01"},
  revision = 0,
  settings = {
    {name = "hand", label = "Hand", type = "choice", options = {"Left", "Right"}, default = 0,
     description = "The hand without the arm cannon (PrimedGun's default)."},
    {name = "radius", label = "Head zone radius", unit = "m", default = 0.24, min = 0.12,
     max = 0.4, step = 0.01, description = "How close to the headset the controller must be."},
    {name = "haptics", label = "Vibrate on a visor change", type = "bool", default = true},
  },
}

-- ---- The game (Metroid Prime 3: Corruption NTSC-U) ----

local STATE_MANAGER = 0x805C4F98     -- -> the state manager
local MENU_STATE = STATE_MANAGER + 0x32C   -- 1: the map, 3: the solar system map
local POINTER_DIALOG = 0x805C70C7    -- byte: the pointer drives a dialog or a menu
local PLAYER_BALL = 0x358            -- 0: not morphed
local PLAYER_CAMERA = 0x3C2C         -- 1: first person
local PLAYER_POWERUPS = 0x35A0       -- -> the powerups
local POWERUPS_VISOR = 0x34
local COMBAT, SCAN, COMMAND, XRAY = 0, 1, 2, 3
local VISOR_POWERUP = {[COMBAT] = 0x0B, [SCAN] = 0x0C, [COMMAND] = 0x0D, [XRAY] = 0x0E}

-- The thumbstick direction -> the visor, as in the game's visor menu.
local DIRECTION_VISOR = {left = SCAN, up = COMMAND, right = XRAY, down = COMBAT}

-- PrimedGun's head zone: within the radius of the headset, from a little below it to above it.
local ZONE_BELOW, ZONE_ABOVE = 0.18, 0.28
local ZONE_GRACE = 16    -- fields the zone lasts after the hand leaves it
local PICK, REARM = 0.5, 0.3   -- thumbstick magnitude that picks a visor, and that re-arms

local u32, u8, raw_pose, axis = mem.u32, mem.u8, vr.raw_pose, vr.axis

local last_in_zone = nil   -- the field the hand was last next to the head
local armed = true         -- the thumbstick came back to the centre since the last pick
local suppressing = false

local function hand_name()
  return settings.hand == 1 and "right" or "left"
end

-- The game's own use of the thumbstick, held released while the gesture uses it.
local function suppress(on)
  if on == suppressing then return end
  suppressing = on
  if hand_name() == "left" then
    if on then nunchuk.set_stick(1, "", 0, 0) else nunchuk.release(1, "") end
  else
    for _, button in ipairs({"1", "2", "-", "+"}) do
      if on then wiimote.set_value(1, button, 0) else wiimote.release(1, button) end
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

-- The player in first person and playing, or nil.
local function playing_player()
  local player = mem.ptr(STATE_MANAGER, 0x2184)
  if not player then return nil end
  if u32(MENU_STATE) == 1 or u32(MENU_STATE) == 3 or u8(POINTER_DIALOG) ~= 0 then return nil end
  if u32(player + PLAYER_BALL) ~= 0 or u32(player + PLAYER_CAMERA) ~= 1 then return nil end
  return player
end

local function select_visor(player, visor)
  local powerups = mem.ptr(player + PLAYER_POWERUPS)
  if not powerups then return end
  local owned = u32(powerups + VISOR_POWERUP[visor] * 12 + 0x58)
  if not owned or owned == 0 or u32(powerups + POWERUPS_VISOR) == visor then return end
  mem.write_u32(powerups + POWERUPS_VISOR, visor)
  if settings.haptics then vr.haptic(hand_name(), 0.4, 0.04) end
  log("visor", visor)
end

function on_frame()
  local field = game.field()
  local player = playing_player()
  if not player or vr.menu_open() or not near_head(field) then
    suppress(false)
    armed = true
    return
  end
  local x, y = axis(hand_name(), "thumbstick_x") or 0, axis(hand_name(), "thumbstick_y") or 0
  local magnitude = math.max(math.abs(x), math.abs(y))
  suppress(magnitude >= REARM or not armed)
  if magnitude < REARM then
    armed = true
    return
  end
  local dir = direction(x, y)
  if dir and armed then
    armed = false
    select_visor(player, DIRECTION_VISOR[dir])
  end
end

function on_state_loaded()
  last_in_zone = nil
  armed = true
  suppress(false)
end

-- Releases what either hand's thumbstick held.
local function release_all()
  nunchuk.release(1, "")
  for _, button in ipairs({"1", "2", "-", "+"}) do wiimote.release(1, button) end
  suppressing = false
end

function on_setting_changed(name)
  if name == "hand" then release_all() end
end

function on_unload()
  release_all()
end
