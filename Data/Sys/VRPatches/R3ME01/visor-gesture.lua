-- Metroid Prime Trilogy (NTSC-U, Wii): change visors with a controller next to the head, in the
-- three games.
--
-- A DolphinXR VR game patch, after PrimedGun's visor gesture for Metroid Prime (PrimedGun,
-- created by Nobbie): bring a controller next to your head and push its thumbstick towards a
-- visor. The directions follow the games' own visor menu (hold -): up the scan visor, left and
-- right the visors the menu shows there (Prime: thermal and X-ray; Echoes: dark and echo;
-- Corruption: X-ray and command), down back to the combat visor (the menu's centre).
--
-- The visors, the powerups that own them and the scan visor's holster come from PrimeHack's
-- tables (shiiion/dolphin, PRIME_1, PRIME_2, PRIME_3), which switch visors the same way: the
-- patch writes the visor the game switches to, if its powerup count is not 0. Prime and Echoes
-- lower the arm cannon for the scan visor themselves only through their menu, so the patch starts
-- that holster too. Each game is its own program on the disc, recognized by its arm cannon code
-- (as in metroid-state.lua). While the gesture picks a visor, the thumbstick's usual input is held
-- released (the Nunchuk stick for the left hand; for the right hand the Wii Remote buttons the
-- stock OpenXR profile puts on its stick).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Change visors next to your head",
  author = "DolphinXR, after PrimedGun (Nobbie), with PrimeHack's addresses",
  version = "1.0",
  description = "Bring a controller next to your head and push its thumbstick towards a visor, as "
    .. "in the game's visor menu: up scan, left and right the game's other two visors, down "
    .. "combat. For the three games of Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
  settings = {
    {name = "hand", label = "Hand", type = "choice", options = {"Left", "Right"}, default = 0,
     description = "The hand without the arm cannon (PrimedGun's default)."},
    {name = "radius", label = "Head zone radius", unit = "m", default = 0.24, min = 0.12,
     max = 0.4, step = 0.01, description = "How close to the headset the controller must be."},
    {name = "haptics", label = "Vibrate on a visor change", type = "bool", default = true},
  },
}

-- ---- The games ----

local u32, u8, ptr = mem.u32, mem.u8, mem.ptr

-- Per game: the hook and next instruction of its arm cannon code (its recognition), the visors
-- (the value the game takes and the powerup that owns it) per thumbstick direction, and:
--   player()          the player in first person and playing (no map, pause screen or dialog), or
--                     nil
--   visor(player)     the address of the visor the game switches to
--   owned(player, v)  whether the visor's powerup count is not 0
--   holster(player)   lowers the arm cannon (the scan visor of Prime and Echoes), or nil
local GAMES = {
  {
    name = "Metroid Prime",
    hook = 0x8018BC74, original = 0x387904B0, after = 0x38810380,
    -- 0 combat, 1 X-ray, 2 scan, 3 thermal.
    directions = {up = {2, 0x05}, left = {3, 0x09}, right = {1, 0x0D}, down = {0, 0x11}},
    player = function()
      local player = ptr(0x804BF420 + 0x84C)
      if not player or u32(0x804BF420 + 0x117C) ~= 0 then return nil end
      if u32(player + 0x2F0) ~= 0 or u32(player + 0x2F4) ~= 0 then return nil end
      return player
    end,
    visor = function()
      local powerups = ptr(0x804BFCD4)
      return powerups and powerups + 0x1C
    end,
    owned = function(_, item)
      local powerups = ptr(0x804BFCD4)
      local count = powerups and u32(powerups + item * 8 + 0x30)
      return count ~= nil and count ~= 0
    end,
    holster = function(player)
      mem.write_u32(player + 0x488, 3)
      mem.write_f32(player + 0x488 + 4, 0.2)
    end,
  },
  {
    name = "Metroid Prime 2: Echoes",
    hook = 0x8018ACB0, original = 0xC00102A0, after = 0x387C03B0,
    -- 0 combat, 1 echo, 2 scan, 3 dark.
    directions = {up = {2, 0x09}, left = {3, 0x0A}, right = {1, 0x0B}, down = {0, 0x08}},
    player = function()
      local player = ptr(0x804E72E8 + 0x14F4)
      if not player or u32(0x804E72E8 + 0x2C0C) ~= 0 then return nil end
      if u32(player + 0x374) ~= 0 or u32(player + 0x378) ~= 0 then return nil end
      return player
    end,
    visor = function(player)
      local powerups = ptr(player + 0x12EC)
      return powerups and powerups + 0x34
    end,
    owned = function(player, item)
      local powerups = ptr(player + 0x12EC)
      local count = powerups and u32(powerups + item * 12 + 0x5C)
      return count ~= nil and count ~= 0
    end,
    holster = function(player)
      local gun = ptr(player + 0xEA8)
      if not gun then return end
      mem.write_u32(gun + 0x3A4, 3)
      mem.write_f32(gun + 0x3A4 - 0x20, 0.2)
    end,
  },
  {
    name = "Metroid Prime 3: Corruption",
    hook = 0x80183C28, original = 0xC0010208, after = 0x7F83E378,
    -- 0 combat, 1 scan, 2 command, 3 X-ray.
    directions = {up = {1, 0x0C}, left = {3, 0x0E}, right = {2, 0x0D}, down = {0, 0x0B}},
    player = function()
      local player = ptr(0x805C6C68, 0x2184)
      if not player then return nil end
      local menu = u32(0x805C6C68 + 0x32C)
      if menu == 1 or menu == 3 or u8(0x805C8D77) ~= 0 then return nil end
      if u32(player + 0x3C3C) ~= 1 or u32(player + 0x358) ~= 0 then return nil end
      return player
    end,
    visor = function(player)
      local powerups = ptr(player + 0x35A8)
      return powerups and powerups + 0x34
    end,
    owned = function(player, item)
      local powerups = ptr(player + 0x35A8)
      local count = powerups and u32(powerups + item * 12 + 0x58)
      return count ~= nil and count ~= 0
    end,
  },
}

-- Whether g's program is in memory: its arm cannon code, whatever the arm-cannon patch made of
-- the hook.
local function present(g)
  if u32(g.hook + 4) ~= g.after then return false end
  local word = u32(g.hook)
  return word == g.original or (word ~= nil and word & 0xFC000003 == 0x48000000)
end

-- ---- The gesture ----

-- PrimedGun's head zone: within the radius of the headset, from a little below it to above it.
local ZONE_BELOW, ZONE_ABOVE = 0.18, 0.28
local ZONE_GRACE = 16    -- fields the zone lasts after the hand leaves it
local PICK, REARM = 0.5, 0.3   -- thumbstick magnitude that picks a visor, and that re-arms

local raw_pose, axis = vr.raw_pose, vr.axis

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

local function select_visor(g, player, dir)
  local visor, item = g.directions[dir][1], g.directions[dir][2]
  local address = g.visor(player)
  if not address or not g.owned(player, item) or u32(address) == visor then return end
  mem.write_u32(address, visor)
  -- Up is the scan visor in the three games.
  if g.holster and dir == "up" then g.holster(player) end
  if settings.haptics then vr.haptic(hand_name(), 0.4, 0.04) end
  log(g.name .. ": visor", visor)
end

-- The running game of the disc, or nil.
local function detect()
  for _, g in ipairs(GAMES) do
    if present(g) then return g end
  end
  return nil
end

function on_frame()
  local field = game.field()
  local g = detect()
  local player = g and g.player()
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
    select_visor(g, player, dir)
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
