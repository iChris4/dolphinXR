-- Metroid Prime 2: Echoes (NTSC-U, GameCube): move and strafe with the left thumbstick, turn with
-- the right one.
--
-- A DolphinXR VR game patch. The GameCube game moves forward and back and turns with the control
-- stick, and strafes only while locked on (L). While playing in first person the patch holds the
-- control stick at the right thumbstick's X (the game's own turning, its acceleration and speed)
-- and the left thumbstick's Y (forward and back), and the strafe input at the left thumbstick's X.
--
-- Echoes' CPlayer::ComputeMovement already moves sideways by its strafe input; CPlayer::
-- StrafeInput only answers 0 outside a lock-on. As in PrimeHack (shiiion/dolphin, FpsControls.cpp,
-- "Enable strafing with left/right on L-Stick", GPL-2.0-or-later), a cave at that orbit state test
-- answers it whatever the orbit state; unlike PrimeHack, which turns the player itself, the game's
-- turning stays. While locked on, the left thumbstick circles the target as usual.
--
-- The right thumbstick also selects beams (C-stick) and visors (D-pad) in the stock profile and
-- the patch holds those released while playing; holding a button (setting) gives the right
-- thumbstick back to them, and it does not turn meanwhile. The maps, pause screens and morph ball
-- keep the user's bindings. While the visor gesture (visor-gesture.lua) uses a thumbstick next to
-- the head (the flags thumbstick_left_gesture and thumbstick_right_gesture), that thumbstick does
-- not move or turn the player.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

local BUTTONS = {"Right B", "Right grip", "Right thumbstick click", "Left X", "Left Y"}

patch = {
  title = "Strafe with the left thumbstick, turn with the right",
  author = "DolphinXR, after PrimeHack's strafing",
  version = "1.1",
  description = "The left thumbstick moves forward, back and sideways and the right thumbstick "
    .. "turns, as in a first-person shooter. Hold a button to choose beams and visors with the "
    .. "right thumbstick. For Metroid Prime 2: Echoes NTSC-U (GameCube).",
  games = {"G2ME01"},
  revision = 0,
  scratch = 0x40,
  settings = {
    {name = "turn_speed", label = "Turn speed", default = 1, min = 0.2, max = 1, step = 0.05,
     description = "The game's turn speed at full thumbstick (1: as with the control stick)."},
    {name = "deadzone", label = "Thumbstick dead zone", default = 0.2, min = 0, max = 0.6,
     step = 0.05},
    {name = "beam_button", label = "Beams and visors on the right thumbstick", type = "choice",
     options = BUTTONS, default = 0,
     description = "While held, the right thumbstick selects beams (C-stick) and visors (D-pad) "
       .. "as your profile binds them, and does not turn."},
  },
}

-- ---- The game (Metroid Prime 2: Echoes NTSC-U) ----

-- CPlayer::StrafeInput (this in r30): its orbit state test, the instruction after it and the
-- function's epilogue. The state manager, the player and its camera and ball states as in
-- metroid-state.lua.
local STRAFE_SITE = 0x801880FC
local STRAFE_ORIGINAL = 0x801E03A4   -- lwz r0, 0x3A4(r30)
local STRAFE_AFTER = 0x2C000000      -- cmpwi r0, 0
local STRAFE_EPILOGUE = 0x80188140
local ORBIT = 0x3A4
local STATE_MANAGER = 0x803DB6E0
local MENU = STATE_MANAGER + 0x2470
local PLAYER = STATE_MANAGER + 0x14FC
local PLAYER_CAMERA = 0x388
local PLAYER_BALL = 0x38C

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local ACTIVE = S + 0x00   -- the player while the patch drives it; 0 = the game's own movement
local STRAFE = S + 0x04   -- the strafe input, -1 (left) to 1 (right)
local CAVE = S + 0x10

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- StrafeInput, after its morph ball test: the patch's input while it drives the player, whatever
-- the orbit state; otherwise the game's test (the replaced instruction) and the rest.
local function cave_words()
  local text = load_address("r11", S) ..
               "lwz r12, 0(r11)\ncmplw r12, r30\nbne strafe_game\nlfs f1, 4(r11)\n" ..
               string.format("b `0x%X`\n", STRAFE_EPILOGUE) ..
               string.format("strafe_game:\nlwz r0, 0x%X(r30)\nb `0x%X`\n", ORBIT, STRAFE_SITE + 4)
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
  local word = mem.u32(STRAFE_SITE)
  if mem.u32(STRAFE_SITE + 4) ~= STRAFE_AFTER or
     (word ~= STRAFE_ORIGINAL and branch_target(STRAFE_SITE, word or 0) ~= CAVE) then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(STRAFE_SITE) ..
           ": is this Metroid Prime 2: Echoes NTSC-U?")
      warned = true
    end
    installed = false
    return
  end
  cave = cave or cave_words()
  local changed = mem.ensure_code(CAVE, cave)
  changed = mem.ensure_code(STRAFE_SITE, asm.branch(STRAFE_SITE, CAVE)) or changed
  if changed then log("hook installed, cave at", mem.hex(CAVE)) end
  installed = true
  warned = false
end

local function uninstall()
  mem.write_u32(ACTIVE, 0)
  if branch_target(STRAFE_SITE, mem.u32(STRAFE_SITE) or 0) == CAVE then
    mem.write_code(STRAFE_SITE, STRAFE_ORIGINAL)
  end
  installed = false
end

-- ---- The controllers ----

-- The VR button of a setting's choice.
local CHOICE_BUTTONS = {
  [0] = {"right", "secondary"}, [1] = {"right", "squeeze"}, [2] = {"right", "thumbstick"},
  [3] = {"left", "primary"}, [4] = {"left", "secondary"},
}
local DPAD = {"D-Pad/Up", "D-Pad/Down", "D-Pad/Left", "D-Pad/Right"}

local holding = false     -- the patch holds the control stick
local holding_right = false   -- and the C-stick and D-pad

local function axis(hand, name)
  return vr.axis(hand, name) or 0
end

local function dead(value)
  local size = math.abs(value)
  local zone = settings.deadzone
  if size <= zone then return 0 end
  return (value > 0 and 1 or -1) * math.min((size - zone) / (1 - zone), 1)
end

local function release_right()
  if holding_right then
    pad.release(1, "C-Stick")
    for _, name in ipairs(DPAD) do pad.release(1, name) end
    holding_right = false
  end
end

local function release()
  if holding then
    pad.release(1, "Main Stick")
    holding = false
  end
  release_right()
end

-- The player in first person and playing (not morphed, no map or pause screen), or nil.
local function playing_player()
  local player = mem.ptr(PLAYER)
  if not player or mem.u32(MENU) ~= 0 then return nil end
  if mem.u32(player + PLAYER_CAMERA) ~= 0 or mem.u32(player + PLAYER_BALL) ~= 0 then return nil end
  return player
end

local function stop()
  mem.write_u32(ACTIVE, 0)
  release()
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(ACTIVE, 0)
  install()
end

function on_state_loaded()
  mem.write_u32(ACTIVE, 0)
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then install() end
  local player = installed and playing_player()
  if not player or vr.menu_open() or not vr.axis("left", "thumbstick_x") then
    stop()
    return
  end
  local button = CHOICE_BUTTONS[settings.beam_button]
  local beams = button and vr.button(button[1], button[2])
  local left = not vr.flag("thumbstick_left_gesture")
  local turn = (beams or vr.flag("thumbstick_right_gesture")) and 0 or
               dead(axis("right", "thumbstick_x")) * settings.turn_speed
  pad.set_stick(1, "Main Stick", turn, left and axis("left", "thumbstick_y") or 0)
  holding = true
  if beams then
    release_right()
  else
    pad.set_stick(1, "C-Stick", 0, 0)
    for _, name in ipairs(DPAD) do pad.set_value(1, name, 0) end
    holding_right = true
  end
  mem.write_f32(STRAFE, left and dead(axis("left", "thumbstick_x")) or 0)
  -- Last: arms the cave for this player.
  mem.write_u32(ACTIVE, player)
end

function on_unload()
  uninstall()
  release()
end
