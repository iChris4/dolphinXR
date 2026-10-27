-- Metroid Prime 3: Corruption (NTSC-U, Wii): turn with the right thumbstick.
--
-- A DolphinXR VR game patch. Prime 3 turns the view while the Wii pointer is near the left or
-- right edge of the screen and tilts it near the top or bottom. With a VR controller aiming the
-- arm cannon (the arm-cannon patch) that is imprecise, and the head already looks up and down.
--
-- The player's controls are read through one function (zz_80061ba8: the control mapper at
-- player + 0x35C4, a control number) that returns 0..1. Controls 2 and 3 are turn left and turn
-- right, which the game ramps up from the pointer's distance to the edge; 0x37 and 0x38 are look
-- up and look down. A cave at its entry answers those four for the player's mapper from values
-- this patch writes every field: the right thumbstick's X for turning, 0 for looking up and down.
-- The game's own turning (acceleration, damping, morph ball and lock-on rules) does the rest, and
-- the pointer still aims, scans and drives the menus.
--
-- Pointing at the top or bottom of the screen tilts Prime 3's camera, which is how the pointer
-- reaches a grapple point or a scan target above or below the view. The patch leaves that to the
-- game, but names the camera matrix's pitch to the VR view, which removes it from each frame: the
-- world stays where the head looks, and the game's screen, with the pointer and the 2D draws on
-- it, moves up or down instead. On the map, the pause screens and dialogs the camera goes back to
-- level, so they are shown in front.
--
-- Prime 3 stops reading the pointer while the Wii Remote points more than about 45 degrees up,
-- and keeps tilting the camera the way the cursor last was; it also turns on its own once the
-- remote loses the sensor bar. Aiming at the top of the virtual screen, all the more once it has
-- moved up with the tilt, reached both, and the camera went up on its own while the pointer did
-- nothing. While the edges tilt the camera, the patch keeps the VR pointer on the screen
-- (vr.keep_pointer_on_screen): an aim past an edge holds it at that edge, and the remote's tilt
-- stays within what aiming at a TV gives, so the game tilts while the aim is past the edge and
-- stops as soon as it comes back.
--
-- The stock OpenXR Wii Remote profile puts the Wii Remote's - (hold: the visor menu) and + (hold:
-- hypermode) on the right thumbstick's X axis. While playing, the patch moves them to other
-- buttons (settings); on the map and pause screens, the thumbstick has them back (zoom). Its up
-- and down (1 and 2: the map and the pause screen) are held released while it turns.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

local BUTTONS = {"Keep my bindings", "Right grip", "Right B", "Right thumbstick click", "Left X",
                 "Left Y"}

patch = {
  title = "Turn with the right thumbstick",
  author = "DolphinXR",
  version = "1.2",
  description = "Turns left and right with the right thumbstick instead of pointing at the edges "
    .. "of the screen, and keeps the view level (look up and down with your head): pointing at "
    .. "the top or bottom edge tilts the game's camera, which moves the pointer's screen instead "
    .. "of your view. The pointer still aims, scans and drives the menus. For Metroid Prime 3: "
    .. "Corruption NTSC-U (Wii).",
  games = {"RM3E01"},
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
    {name = "hide_crosshair", label = "Hide the crosshair", type = "bool", default = true,
     description = "The green crosshair drawn under the pointer (the profile's \"Crosshair\" "
       .. "entry)."},
    {name = "visor_button", label = "Visor menu (Wii Remote -)", type = "choice",
     options = BUTTONS, default = 3,
     description = "Held to choose a visor while playing (the stock profile leaves the thumbstick "
       .. "click free). Keep my bindings: the thumbstick's left still presses - when your "
       .. "profile binds it there."},
    {name = "hyper_button", label = "Hypermode (Wii Remote +)", type = "choice",
     options = BUTTONS, default = 2},
    {name = "test_stick", label = "Test thumbstick", default = 0, min = -1, max = 1, step = 0.1,
     description = "Not 0: turns as if the right thumbstick were held at this X, to check the "
       .. "patch without a headset."},
  },
}

-- ---- The game (Metroid Prime 3: Corruption NTSC-U) ----

local STATE_MANAGER = 0x805C4F98     -- -> the state manager (PrimeHack: state_manager)
local MENU_STATE = STATE_MANAGER + 0x32C   -- 1: the map, 3: the solar system map
local POINTER_DIALOG = 0x805C70C7    -- byte: the pointer drives a dialog or a menu
local PLAYER_CONTROLS = 0x35C4       -- player -> its control mapper
local PLAYER_BALL = 0x358            -- 0: not morphed
local PLAYER_PITCH = 0x77C           -- the look pitch, radians, + up
local PLAYER_CAMERA = 0x3C2C         -- 1: first person
local CAMERA_CHAIN = {0x805C4FA8, 0x34, 0x20}  -- -> the camera actor (as in arm-cannon.lua)
local CAMERA_PITCH_SINE = 0x3C + 0x24          -- its transform's forward up component (row 2, col 1)
local CONTROL = 0x80061BA8           -- zz_80061ba8: a control's value (r3 mapper, r4 control)
local CONTROL_ORIGINAL = 0x9421FFC0  -- stwu sp, -0x40(sp)

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
local function cave_words()
  local text = load_address("r11", S) .. [[
lwz r12, 0(r11)
cmplw r12, r3
bne game
cmplwi r4, 2
beq left
cmplwi r4, 3
beq right
lwz r12, 4(r11)
cmpwi r12, 0
beq game
cmplwi r4, 0x37
beq zero
cmplwi r4, 0x38
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
]] .. string.format("b `0x%X`\n", CONTROL + 4)
  local words, err = asm.assemble(text, CAVE)
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
  local current = mem.u32(CONTROL)
  if current ~= CONTROL_ORIGINAL and not branch_target(CONTROL, current or 0) then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(CONTROL) .. " (" .. mem.hex(current) ..
           "): is this Metroid Prime 3 NTSC-U?")
    end
    warned = true
    installed = false
    return
  end
  cave = cave or cave_words()
  local changed = mem.ensure_code(CAVE, cave)
  changed = mem.ensure_code(CONTROL, asm.branch(CONTROL, CAVE)) or changed
  if changed then log("hook installed, cave at", mem.hex(CAVE)) end
  installed = true
end

-- ---- The Wii Remote's - and + ----

-- The VR button of a setting's choice, or nil for "Keep my bindings".
local CHOICE_BUTTONS = {
  [1] = {"right", "squeeze"}, [2] = {"right", "secondary"}, [3] = {"right", "thumbstick"},
  [4] = {"left", "primary"}, [5] = {"left", "secondary"},
}

local held = {["-"] = false, ["+"] = false, ["1"] = false, ["2"] = false}

-- While playing: the Wii Remote button follows the chosen VR button (and nothing else presses
-- it); otherwise the user's own bindings.
local function drive_button(name, choice, playing)
  local button = CHOICE_BUTTONS[choice]
  if not button or not playing then
    if held[name] then
      wiimote.release(1, name)
      held[name] = false
    end
    return
  end
  wiimote.set_value(1, name, vr.button(button[1], button[2]) and 1 or 0)
  held[name] = true
end

-- While the thumbstick turns (pushed more sideways than up or down), its up and down (the Wii
-- Remote's 1 and 2 in the stock profile: the map and the pause screen) are held released, so a
-- diagonal push does not open them.
local function guard_up_down(turning)
  for _, name in ipairs({"1", "2"}) do
    if turning then
      wiimote.set_value(1, name, 0)
      held[name] = true
    elseif held[name] then
      wiimote.release(1, name)
      held[name] = false
    end
  end
end

local function release_buttons()
  for name, is_held in pairs(held) do
    if is_held then wiimote.release(1, name) end
    held[name] = false
  end
end

-- ---- Every field ----

local write_f32s, write_u32, u32, u8 = mem.write_f32s, mem.write_u32, mem.u32, mem.u8

local function stop()
  write_u32(GUARD, 0)
  vr.set_flag("rm3_hide_crosshair", false)
  vr.set_camera_pitch_source(nil)
  vr.keep_pointer_on_screen(false)
  release_buttons()
end

-- The flat screen mode shows the game camera as it is.
local flat_screen = false

-- While the camera is the first-person one, the VR view removes its pitch (also while a lock-on
-- or a grapple swing aims it). On the map, the pause screens and dialogs the camera goes back to
-- level (the game eases it), so they are in front. While playing, the edges' tilt keeps the
-- pointer on the screen.
local function level_view(player, playing)
  if game.field() % 60 == 0 then flat_screen = vr.setting("FlatScreen") == "True" end
  local first_person = u32(player + PLAYER_CAMERA) == 1 and u32(player + PLAYER_BALL) == 0
  vr.keep_pointer_on_screen(settings.edge_tilt and first_person and playing)
  local camera = first_person and not flat_screen and
                 mem.ptr(CAMERA_CHAIN[1], CAMERA_CHAIN[2], CAMERA_CHAIN[3])
  vr.set_camera_pitch_source(camera and camera + CAMERA_PITCH_SINE or nil)
  if camera and not playing then mem.write_f32(player + PLAYER_PITCH, 0) end
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
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then install() end
  local player = mem.ptr(STATE_MANAGER, 0x2184)
  if not installed or not player then
    stop()
    return
  end
  local menu = u32(MENU_STATE)
  local dialog = u8(POINTER_DIALOG)
  local playing = menu ~= 1 and menu ~= 3 and dialog == 0

  local x = stick_x() * settings.speed
  write_f32s(LEFT, math.max(-x, 0), math.max(x, 0), 0)
  write_u32(LEVEL, settings.edge_tilt and 0 or 1)
  -- Last: arms the cave for this player.
  write_u32(GUARD, player + PLAYER_CONTROLS)

  vr.set_flag("rm3_hide_crosshair", settings.hide_crosshair)
  drive_button("-", settings.visor_button, playing)
  drive_button("+", settings.hyper_button, playing)
  local raw_x, raw_y = 0, 0
  if not vr.menu_open() then
    raw_x, raw_y = vr.axis("right", "thumbstick_x") or 0, vr.axis("right", "thumbstick_y") or 0
  end
  guard_up_down(playing and x ~= 0 and math.abs(raw_x) >= math.abs(raw_y))
  level_view(player, playing)
end

function on_unload()
  if mem.u32(CONTROL) and branch_target(CONTROL, mem.u32(CONTROL)) then
    mem.write_code(CONTROL, CONTROL_ORIGINAL)
  end
  write_u32(GUARD, 0)
  release_buttons()
  installed = false
end
