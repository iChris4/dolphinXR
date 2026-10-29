-- Metroid Prime Trilogy (NTSC-U, Wii): turn with the right thumbstick, in the three games.
--
-- A DolphinXR VR game patch, the Trilogy's version of RM3E01/stick-turn.lua. The three games turn
-- the view while the Wii pointer is near the left or right edge of the screen and tilt it near the
-- top or bottom. With a VR controller aiming the arm cannon (the arm-cannon patch) that is
-- imprecise, and the head already looks up and down.
--
-- Each game reads the player's controls through one function (the control mapper, a control
-- number) that returns 0..1, and its turning and look code asks it for turn left and right and
-- look up and down, which the mapper ramps up from the pointer's distance to the edges. A cave at
-- its entry answers those for the player's mapper from values this patch writes every field: the
-- right thumbstick's X for turning, 0 for looking up and down when the edges should not tilt. The
-- game's own turning (acceleration, damping, morph ball and lock-on rules) does the rest, and the
-- pointer still aims, scans and drives the menus. Each game is its own program on the disc,
-- recognized by its arm cannon code (as in metroid-state.lua) and by the mapper's code.
--
-- Pointing at the top or bottom of the screen tilts the game's camera, which is how the pointer
-- reaches a grapple point or a scan target above or below the view. The patch leaves that to the
-- game, but names the camera matrix's pitch to the VR view, which removes it from each frame: the
-- world stays where the head looks, and the game's screen, with the pointer and the 2D draws on
-- it, moves up or down instead (the arm-cannon patch then places the cannon in the camera's level
-- frame). While playing in first person the VR pointer stays on the screen
-- (vr.keep_pointer_on_screen): an aim past an edge holds it at that edge, so the camera tilts while
-- the aim is past the edge and stops as soon as it comes back. Echoes also needs the pointer on
-- the screen to turn: two seconds after it leaves the screen, it stops turning altogether.
-- Corruption's camera goes back to level on its map, pause screens and dialogs, so they are shown
-- in front; Prime's and Echoes' pause screens and maps are shown as the game draws them.
--
-- The stock OpenXR Wii Remote profile puts the Wii Remote's - (hold: the visor menu) and + (hold:
-- the beam menu, Corruption's hypermode) on the right thumbstick's X axis. While playing, the
-- patch moves them to other buttons (settings); on the maps and pause screens, the thumbstick has
-- them back. Its up and down (1 and 2: the map and the pause screen) are held released while it
-- turns.
--
-- PrimedGun's jump, fire and missiles (settings, on by default). Jump: while playing in first
-- person or screw attacking (which the games run as a morph ball state), pushing the right
-- thumbstick up holds the Wii Remote's B, the jump, and the Wii Remote's 1 (the map, on the
-- thumbstick's up in the stock profile) is held released: the Wii Remote reads the profile before
-- the patch sees the thumbstick move. The map button (setting) presses 1 instead. Fire: while
-- playing (also in the morph ball: bombs), the right trigger presses the Wii Remote's A, which is
-- otherwise the user's (Echoes' scan descriptions close with it) unless a setting gives the right A
-- to another button; B is held released but for the jump: the stock profiles put B on the trigger.
-- Missiles: the D-pad's down, which the stock profiles leave unbound, follows a button (setting)
-- while playing; a setting above that chose the same button then holds its Wii Remote button
-- released. On the maps, the pause screens and the menus, the buttons are the user's.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

local BUTTONS = {"Keep my bindings", "Right grip", "Right B", "Right thumbstick click", "Left X",
                 "Left Y", "Right A"}

patch = {
  title = "Modern controls",
  author = "DolphinXR",
  version = "1.2",
  description = "Turns left and right with the right thumbstick instead of pointing at the edges "
    .. "of the screen, and keeps the view level (look up and down with your head): pointing at "
    .. "the top or bottom edge tilts the game's camera, which moves the pointer's screen instead "
    .. "of your view. The pointer still aims, scans and drives the menus. Options, as in "
    .. "PrimedGun: jump with the right thumbstick up, fire with the right trigger, missiles on a "
    .. "button such as the right grip. For the three games of Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
  scratch = 0x80,
  settings = {
    {name = "speed", label = "Turn speed", default = 1, min = 0.2, max = 1, step = 0.05,
     description = "The game's turn speed at full thumbstick (1: as when pointing at the edge)."},
    {name = "deadzone", label = "Thumbstick dead zone", default = 0.2, min = 0, max = 0.6,
     step = 0.05},
    {name = "edge_tilt", label = "The pointer's edges tilt the camera", type = "bool",
     default = true,
     description = "Pointing at the top or bottom of the screen tilts the game's camera, as on "
       .. "the Wii, so the pointer reaches what is above or below. Your view stays level: the "
       .. "screen the pointer moves on goes up or down, and aiming past its edge keeps the pointer "
       .. "at that edge. Off: those edges do nothing."},
    {name = "visor_button", label = "Visor menu (Wii Remote -)", type = "choice",
     options = BUTTONS, default = 3,
     description = "Held to choose a visor while playing (the stock profile leaves the thumbstick "
       .. "click free). Keep my bindings: the thumbstick's left still presses - when your "
       .. "profile binds it there."},
    {name = "plus_button", label = "Beam menu, hypermode (Wii Remote +)", type = "choice",
     options = BUTTONS, default = 2,
     description = "Held to choose a beam in Metroid Prime and Echoes, and for hypermode in "
       .. "Corruption."},
    {name = "jump", label = "Jump with the right thumbstick up", type = "bool", default = true,
     description = "In first person and in the screw attack, pushing the right thumbstick up "
       .. "jumps (Wii Remote B), as in PrimedGun. The morph ball, the maps and the menus keep "
       .. "your binding of B."},
    {name = "fire", label = "Fire with the right trigger", type = "bool", default = true,
     description = "While playing, the right trigger also fires and drops bombs (Wii Remote A), as "
       .. "in PrimedGun. B is then only the jump above (the stock profiles put B on the "
       .. "trigger): use both. The maps and the menus keep your bindings."},
    {name = "map_button", label = "Map (Wii Remote 1)", type = "choice", options = BUTTONS,
     default = 5,
     description = "Held to open the map while playing. With the jump above, the thumbstick's "
       .. "up (1 in the stock profile) no longer opens it. Right A: with the fire option, A is "
       .. "then only on the trigger."},
    {name = "missile_button", label = "Missiles (Wii Remote D-pad down)", type = "choice",
     options = BUTTONS, default = 1,
     description = "Held to fire missiles while playing (the stock profiles leave the D-pad "
       .. "unbound). It takes its button from the settings above that chose it."},
    {name = "test_stick", label = "Test thumbstick", default = 0, min = -1, max = 1, step = 0.1,
     description = "Not 0: turns as if the right thumbstick were held at this X, to check the "
       .. "patch without a headset."},
  },
}

-- ---- The games ----

-- The camera of Metroid Prime and Echoes: the camera manager holds the camera's object id, and
-- the object list maps it to the object (PrimeHack's camera lookup, as in arm-cannon.lua).
local function object_camera(state_manager, manager, xf)
  local id = manager and mem.u16(manager)
  if not id or id == 0xFFFF then return nil end
  local camera = mem.ptr(state_manager + 0x810, ((id & 0x3FF) << 3) + 4)
  return camera and camera + xf
end

-- Per game: the hook and next instruction of its arm cannon code (its recognition, as in
-- metroid-state.lua), the control mapper's function with a word of its code and the register of
-- the control number (r3 is the mapper), the player's mapper, the controls. state() returns the
-- player (nil when there is none), whether it plays (no map, pause screen or dialog) and whether
-- it is in first person; camera() the camera's transform (3x4, row-major, world space); screw
-- the player's screw attack state (PrimeHack's screw_state: not 0 while screw attacking, a
-- morph ball state for the game).
local GAMES = {
  {
    name = "Metroid Prime",
    hook = 0x8018BC74, original = 0x387904B0, after = 0x38810380,
    mapper = 0x80075F68, check = {0x14, 0xC3E28944}, control = "r4",
    player_mapper = 0xA40, left = 3, right = 4, up = 9, down = 0xA,
    state = function()
      local player = mem.ptr(0x804BF420 + 0x84C)
      if not player then return nil end
      return player, mem.u32(0x804BF420 + 0x117C) == 0,
             mem.u32(player + 0x2F0) == 0 and mem.u32(player + 0x2F4) == 0
    end,
    camera = function()
      return object_camera(0x804BF420, mem.ptr(0x804BF420 + 0x868), 0x2C)
    end,
  },
  {
    name = "Metroid Prime 2: Echoes",
    hook = 0x8018ACB0, original = 0xC00102A0, after = 0x387C03B0,
    mapper = 0x8006FE1C, check = {0x18, 0x48248589}, control = "r5",
    player_mapper = 0x13BC, left = 3, right = 4, up = 9, down = 0xA, screw = 0x37C,
    state = function()
      local player = mem.ptr(0x804E72E8 + 0x14F4)
      if not player then return nil end
      return player, mem.u32(0x804E72E8 + 0x2C0C) == 0,
             mem.u32(player + 0x374) == 0 and mem.u32(player + 0x378) == 0
    end,
    camera = function()
      local manager = mem.ptr(0x804E72E8 + 0x1514)
      return object_camera(0x804E72E8, manager and manager + 0x10, 0x20)
    end,
  },
  {
    name = "Metroid Prime 3: Corruption",
    hook = 0x80183C28, original = 0xC0010208, after = 0x7F83E378,
    mapper = 0x800618B0, check = {0x14, 0xC3E288C4}, control = "r4",
    player_mapper = 0x35D8, left = 2, right = 3, up = 0x37, down = 0x38, screw = 0x35C,
    -- The look pitch (radians, + up): written 0 on the map, the pause screens and the dialogs.
    level_pitch = 0x784,
    state = function()
      local player = mem.ptr(0x805C6C68, 0x2184)
      if not player then return nil end
      local menu = mem.u32(0x805C6C68 + 0x32C)
      return player, menu ~= 1 and menu ~= 3 and mem.u8(0x805C8D77) == 0,
             mem.u32(player + 0x3C3C) == 1 and mem.u32(player + 0x358) == 0
    end,
    camera = function()
      local camera = mem.ptr(0x805C6C78, 0x34, 0x20)
      return camera and camera + 0x3C
    end,
  },
}

local MAPPER_ORIGINAL = 0x9421FFC0   -- stwu sp, -0x40(sp): the first instruction of all three
local CAMERA_PITCH_SINE = 0x24       -- the camera transform's forward up component (row 2, col 1)

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local GUARD = S + 0x00    -- the player's control mapper while the patch drives it; 0 = off
local LEVEL = S + 0x04    -- 1: look up and down read 0
local LEFT = S + 0x08     -- turn left, 0..1
local RIGHT = S + 0x0C    -- turn right, 0..1
local ZERO = S + 0x10     -- 0.0
local CAVE = S + 0x20

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- For the player's mapper: turn left and right from LEFT and RIGHT, look up and down 0 when LEVEL
-- is set; anything else runs the game's function (its first instruction, then the rest).
local function cave_words(g)
  local c = g.control
  local text = load_address("r11", S) .. string.format([[
lwz r12, 0(r11)
cmplw r12, r3
bne game
cmplwi %s, 0x%X
beq left
cmplwi %s, 0x%X
beq right
lwz r12, 4(r11)
cmpwi r12, 0
beq game
cmplwi %s, 0x%X
beq zero
cmplwi %s, 0x%X
bne game
zero:
lfs f1, 0x10(r11)
blr
left:
lfs f1, 8(r11)
blr
right:
lfs f1, 0xC(r11)
blr
game:
stwu r1, -0x40(r1)
b `0x%X`
]], c, g.left, c, g.right, c, g.up, c, g.down, g.mapper + 4)
  local words, err = asm.assemble(text, CAVE)
  if not words then error("assembling the cave for " .. g.name .. ": " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

-- ---- Finding the game, installing and removing the hook ----

local current = nil       -- the game in GAMES whose hook is installed
local caves = {}          -- the built cave per game
local waiting_logged = false
local warned = {}

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- Whether g's program is in memory: its arm cannon code, whatever the arm-cannon patch made of
-- the hook.
local function present(g)
  if mem.u32(g.hook + 4) ~= g.after then return false end
  local word = mem.u32(g.hook)
  return word == g.original or (word ~= nil and word & 0xFC000003 == 0x48000000)
end

local function detect()
  for _, g in ipairs(GAMES) do
    if present(g) then return g end
  end
  return nil
end

-- Whether the mapper's code is g's: its first instruction or the branch to the cave, and the
-- word that tells it apart.
local function mapper_present(g)
  local word = mem.u32(g.mapper)
  if word ~= MAPPER_ORIGINAL and branch_target(g.mapper, word or 0) ~= CAVE then return false end
  return mem.u32(g.mapper + g.check[1]) == g.check[2]
end

local function install()
  local g = detect()
  if g ~= current then mem.write_u32(GUARD, 0) end
  if not g then
    if current then log("no game of the trilogy is running") end
    current = nil
    if not waiting_logged then log("waiting for one of the three games") end
    waiting_logged = true
    return
  end
  waiting_logged = false
  if not mapper_present(g) then
    if not warned[g] then
      warn(g.name .. ": not installed: unexpected code at " .. mem.hex(g.mapper))
      warned[g] = true
    end
    current = nil
    return
  end
  caves[g] = caves[g] or cave_words(g)
  local changed = mem.ensure_code(CAVE, caves[g])
  changed = mem.ensure_code(g.mapper, asm.branch(g.mapper, CAVE)) or changed
  if changed or g ~= current then log(g.name .. ": hook installed, cave at", mem.hex(CAVE)) end
  current = g
end

local function uninstall()
  local g = current
  current = nil
  mem.write_u32(GUARD, 0)
  if g and branch_target(g.mapper, mem.u32(g.mapper) or 0) == CAVE then
    mem.write_code(g.mapper, MAPPER_ORIGINAL)
  end
end

-- ---- The Wii Remote's - and + ----

-- The VR button of a setting's choice, or nil for "Keep my bindings".
local CHOICE_BUTTONS = {
  [1] = {"right", "squeeze"}, [2] = {"right", "secondary"}, [3] = {"right", "thumbstick"},
  [4] = {"left", "primary"}, [5] = {"left", "secondary"}, [6] = {"right", "primary"},
}
local RIGHT_A = 6
local MISSILE = "D-Pad/Down"
local CHOICE_SETTINGS = {"visor_button", "plus_button", "map_button", "missile_button"}

local held = {["-"] = false, ["+"] = false, ["1"] = false, ["2"] = false, ["A"] = false,
              ["B"] = false, [MISSILE] = false}

-- While playing: the Wii Remote button follows the chosen VR button (and nothing else presses
-- it), unless the missile button took that VR button; otherwise the user's own bindings.
local function drive_button(name, choice, playing)
  local button = CHOICE_BUTTONS[choice]
  if not button or not playing then
    if held[name] then
      wiimote.release(1, name)
      held[name] = false
    end
    return
  end
  local taken = name ~= MISSILE and choice == settings.missile_button
  wiimote.set_value(1, name, not taken and vr.button(button[1], button[2]) and 1 or 0)
  held[name] = true
end

-- A Wii Remote button held released, or the user's again.
local function guard(name, on)
  if on then
    wiimote.set_value(1, name, 0)
    held[name] = true
  elseif held[name] then
    wiimote.release(1, name)
    held[name] = false
  end
end

-- The Wii Remote's 1 and 2 (the map and the pause screen: the thumbstick's up and down in the
-- stock profile). While playing, 1 follows the map button (setting); otherwise it is held released
-- while the thumbstick turns (pushed more sideways than up or down) or jumps, and 2 while it turns,
-- so a push of the thumbstick does not open them.
local function drive_map_pause(playing, turning, jump)
  if playing and CHOICE_BUTTONS[settings.map_button] then
    drive_button("1", settings.map_button, playing)
  else
    guard("1", turning or jump)
  end
  guard("2", turning)
end

-- PrimedGun's jump and fire: B held while the thumbstick is up (from 0.6 until it is back under
-- 0.4); A pressed by the right trigger, otherwise the user's (held released when a setting gives
-- the right A to another button), with B released but for the jump.
local JUMP_PRESS, JUMP_RELEASE, TRIGGER_PRESS = 0.6, 0.4, 0.5
local jumping = false

local function right_a_taken()
  for _, name in ipairs(CHOICE_SETTINGS) do
    if settings[name] == RIGHT_A then return true end
  end
  return false
end

local function drive_fire_jump(jump, fire, y)
  jumping = jump and (y >= JUMP_PRESS or (jumping and y >= JUMP_RELEASE))
  local trigger = fire and (vr.axis("right", "trigger") or 0) >= TRIGGER_PRESS
  if trigger or (fire and right_a_taken()) then
    wiimote.set_value(1, "A", trigger and 1 or 0)
    held["A"] = true
  elseif held["A"] then
    wiimote.release(1, "A")
    held["A"] = false
  end
  if jumping or fire then
    wiimote.set_value(1, "B", jumping and 1 or 0)
    held["B"] = true
  elseif held["B"] then
    wiimote.release(1, "B")
    held["B"] = false
  end
end

local function release_buttons()
  for name, is_held in pairs(held) do
    if is_held then wiimote.release(1, name) end
    held[name] = false
  end
end

-- ---- Every field ----

local write_f32s, write_u32 = mem.write_f32s, mem.write_u32

local function stop()
  write_u32(GUARD, 0)
  vr.set_camera_pitch_source(nil)
  vr.keep_pointer_on_screen(false)
  release_buttons()
end

-- The flat screen mode shows the game camera as it is.
local flat_screen = false

-- While playing in first person the VR view removes the camera's pitch (also while a lock-on or a
-- grapple swing aims it), and the pointer stays on the screen. Corruption's camera goes back to
-- level on its map, pause screens and dialogs (the game eases it), so they are in front; Prime and
-- Echoes freeze their camera there, so their pause screens and maps are shown as drawn.
local function level_view(g, player, playing, first_person)
  if game.field() % 60 == 0 then flat_screen = vr.setting("FlatScreen") == "True" end
  vr.keep_pointer_on_screen(first_person and playing)
  local camera = first_person and (playing or g.level_pitch) and not flat_screen and g.camera()
  vr.set_camera_pitch_source(camera and camera + CAMERA_PITCH_SINE or nil)
  if camera and not playing then mem.write_f32(player + g.level_pitch, 0) end
end

local function stick_x()
  if settings.test_stick ~= 0 then return settings.test_stick end
  if vr.menu_open() then return 0 end
  local x = vr.axis("right", "thumbstick_x") or 0
  local dead = settings.deadzone
  local size = math.abs(x)
  if size <= dead then return 0 end
  return (x > 0 and 1 or -1) * math.min((size - dead) / (1 - dead), 1)
end

function on_load()
  write_u32(GUARD, 0)
  flat_screen = vr.setting("FlatScreen") == "True"
  install()
end

function on_state_loaded()
  write_u32(GUARD, 0)
  current = nil
  install()
end

function on_frame()
  -- Another game of the disc may have started: its program replaced the hooked one.
  if game.field() % 60 == 0 or not current or not present(current) then install() end
  local g = current
  local player, playing, first_person
  if g then player, playing, first_person = g.state() end
  if not player then
    stop()
    return
  end

  local x = stick_x() * settings.speed
  write_f32s(LEFT, math.max(-x, 0), math.max(x, 0), 0)
  write_u32(LEVEL, settings.edge_tilt and 0 or 1)
  -- Last: arms the cave for this player.
  write_u32(GUARD, player + g.player_mapper)

  drive_button("-", settings.visor_button, playing)
  drive_button("+", settings.plus_button, playing)
  drive_button(MISSILE, settings.missile_button, playing)
  local raw_x, raw_y = 0, 0
  if not vr.menu_open() then
    raw_x, raw_y = vr.axis("right", "thumbstick_x") or 0, vr.axis("right", "thumbstick_y") or 0
  end
  local screw = g.screw and mem.u32(player + g.screw)
  local jump = settings.jump and playing and (first_person or (screw or 0) ~= 0) and
               not vr.menu_open()
  drive_map_pause(playing, playing and x ~= 0 and math.abs(raw_x) >= math.abs(raw_y), jump)
  drive_fire_jump(jump, settings.fire and playing and not vr.menu_open(), raw_y)
  level_view(g, player, playing, first_person)
end

function on_unload()
  uninstall()
  release_buttons()
end
