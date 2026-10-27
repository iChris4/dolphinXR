-- Metroid Prime (NTSC-U 1.00 and 1.02, GameCube): move and strafe with the left thumbstick, turn
-- with the right one.
--
-- A DolphinXR VR game patch. The GameCube game moves forward and back and turns with the control
-- stick, and strafes only while locked on (L). While playing in first person the patch holds the
-- control stick at the right thumbstick's X (the game's own turning, its acceleration and speed)
-- and the left thumbstick's Y (forward and back), and the strafe input at the left thumbstick's X.
--
-- Strafing without a lock-on uses PrimeHack's port of the Trilogy's Metroid Prime movement
-- (shiiion/dolphin, Source/Core/Core/PrimeHack/Mods/StrafeControlPatches.h, GPL-2.0-or-later):
-- CPlayer::StrafeInput answers whatever the orbit state, and CPlayer::ComputeMovement applies the
-- force of the forward and side input together (planar_move and compute_input_force_scale, the
-- Trilogy's tweaks per surface) where the GameCube game applies its forward force. Unlike
-- PrimeHack, which turns the player itself, the game's turning stays. While locked on, the left
-- thumbstick circles the target as usual.
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
  author = "DolphinXR, with PrimeHack's strafe movement",
  version = "1.1",
  description = "The left thumbstick moves forward, back and sideways and the right thumbstick "
    .. "turns, as in a first-person shooter. Hold a button to choose beams and visors with the "
    .. "right thumbstick. For Metroid Prime NTSC-U 1.00 and 1.02 (GameCube).",
  games = {"GM8E01"},
  scratch = 0x220,
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

-- ---- The game ----

-- Per disc revision, recognized by its code. The strafe site is the orbit state test of
-- CPlayer::StrafeInput (this in r30); the force site the start of CPlayer::ComputeMovement's
-- forward force (player r29, input r30, dt f27, jump f29, forward input f31, its force vector at
-- sp+0xDC), and resume where the game applies its turn torque. PrimeHack's symbols, rtoc and r13
-- offsets and player offsets (StrafeControlPatches.h); the state manager and the player's camera
-- and ball states as in metroid-state.lua.
local REVISIONS = {
  {
    name = "Metroid Prime 1.00",
    state_manager = 0x8045A1A8, camera = 0x2F4, ball = 0x2F8, orbit = 0x304,
    strafe_input = 0x80286C50, strafe_site = 0x80286C80, strafe_original = 0x801E0304,
    strafe_epilogue = 0x80286CB4,
    force_site = 0x80287590, force_original = 0xC002BDA0, force_resume = 0x802875BC,
    mag2d = 0x803140F4, normalize2d = 0x803141B0, transpose_rotate = 0x80312A24,
    dot2d = 0x80313FA8, apply_force = 0x8011C144, axis_identity = 0x8001B534,
    zero = -0x4260, epsilon = -0x4E00, epsilon_1e5 = -0x47D8, sixty = -0x4D70, minus_one = -0x5740,
    one = -0x378C, tweak = -0x5EE8,
    transform = 0x34, mass = 0xE8, velocity = 0x138, restraint = 0x2AC, out_of_water = 0x2B0,
    damped = 0x802884F0, mat_rotate = 0x80312A80, scale2d = 0x8031414C, seven_halves = -0x54F8,
    damp_epsilon = -0x36E0, move_state = 0x258, grapple_timeout = 0x3D8,
  },
  {
    name = "Metroid Prime 1.02",
    state_manager = 0x8045B208, camera = 0x304, ball = 0x308, orbit = 0x314,
    strafe_input = 0x802875DC, strafe_site = 0x8028760C, strafe_original = 0x801E0314,
    strafe_epilogue = 0x80287640,
    force_site = 0x80287F1C, force_original = 0xC002BDA8, force_resume = 0x80287F48,
    mag2d = 0x80314B44, normalize2d = 0x80314C00, transpose_rotate = 0x80313474,
    dot2d = 0x803149F8, apply_force = 0x8011C854, axis_identity = 0x8001B814,
    zero = -0x34BC, epsilon = -0x3320, epsilon_1e5 = -0x38FC, sixty = -0x4148, minus_one = -0x33A8,
    one = -0x4160, tweak = -0x5EC8,
    transform = 0x34, mass = 0xF8, velocity = 0x148, restraint = 0x2BC, out_of_water = 0x2C0,
    damped = 0x80288EA4, mat_rotate = 0x803134D0, scale2d = 0x80314B9C, seven_halves = -0x54F8,
    damp_epsilon = -0x36D8, move_state = 0x268, grapple_timeout = 0x3E8,
  },
}
-- CPlayer::GetDampedClampedVelocityWR's first instructions in both revisions (0x200 bytes long).
local DAMPED_ORIGINAL = {0x9421FFB0, 0x7C0802A6, 0x90010054}
local DAMPED_WORDS = 0x80
local STRAFE_AFTER = 0x2C000000   -- cmpwi r0, 0
local FORCE_AFTER = 0xEC21002A    -- fadds f1, f1, f0

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local ACTIVE = S + 0x00   -- the player while the patch drives it; 0 = the game's own movement
local STRAFE = S + 0x04   -- the strafe input, -1 (left) to 1 (right)
local CODE = S + 0x10

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

local function signed16(value)
  value = value & 0xFFFF
  return value >= 0x8000 and value - 0x10000 or value
end

-- StrafeInput, after its morph ball test: the patch's input while it drives the player, whatever
-- the orbit state; otherwise the game's test (the replaced instruction) and the rest.
local function strafe_text(r)
  local f = string.format
  return load_address("r11", S) ..
         "lwz r12, 0(r11)\ncmplw r12, r30\nbne strafe_game\nlfs f1, 4(r11)\n" ..
         f("b `0x%X`\n", r.strafe_epilogue) ..
         f("strafe_game:\nlwz r0, 0x%X(r30)\nb `0x%X`\n", r.orbit, r.strafe_site + 4)
end

-- ComputeMovement's forward force while the patch drives the player: the force of the forward
-- and side input, then the game's turn torque; otherwise the game's own (the replaced
-- instruction and the rest). Then PrimeHack's planar_move (r3 player, r4 the force vector to
-- fill, f1 forward, f2 side, f3 dt) and compute_input_force_scale (r3 player, f1 input magnitude,
-- f2 input direction . local velocity, f3 dt; the Trilogy's 0x80193FDC).
local function force_text(r)
  local f = string.format
  return table.concat({
    load_address("r11", S),
    "lwz r12, 0(r11)\ncmplw r12, r29\nbne force_game\n",
    f("mr r3, r29\nmr r4, r30\nbl `0x%X`\nfmr f2, f1\n", r.strafe_input),
    "mr r3, r29\naddi r4, r1, 0xDC\nfmr f1, f31\nfmr f3, f27\nbl planar_move\n",
    f("bl `0x%X`\nstfs f29, 0xE4(r1)\nmr r5, r3\nmr r3, r29\naddi r4, r1, 0xDC\nbl `0x%X`\n",
      r.axis_identity, r.apply_force),
    f("b `0x%X`\n", r.force_resume),
    f("force_game:\nlfs f0, %d(r2)\nb `0x%X`\n", signed16(r.force_original), r.force_site + 4),

    "planar_move:\n",
    "stwu r1, -0x38(r1)\nmflr r0\nstw r0, 0x3C(r1)\nstfd f31, 0x8(r1)\nstfd f30, 0x10(r1)\n",
    "stw r31, 0x18(r1)\nstw r30, 0x1C(r1)\nmr r31, r4\nmr r30, r3\nfmr f31, f3\n",
    f("lfs f0, %d(r2)\nstfs f0, 0(r31)\nstfs f0, 4(r31)\n", r.zero),
    "stfs f2, 0x20(r1)\nstfs f1, 0x24(r1)\naddi r3, r1, 0x20\n", f("bl `0x%X`\n", r.mag2d),
    f("fmr f30, f1\nlfs f0, %d(r2)\nfcmpo cr0, f1, f0\nble planar_end\n", r.epsilon),
    f("addi r3, r1, 0x2C\naddi r4, r30, 0x%X\naddi r5, r30, 0x%X\nbl `0x%X`\n", r.transform,
      r.velocity, r.transpose_rotate),
    f("addi r3, r1, 0x20\nbl `0x%X`\n", r.normalize2d),
    f("addi r3, r1, 0x20\naddi r4, r1, 0x2C\nbl `0x%X`\n", r.dot2d),
    "mr r3, r30\nfmr f3, f31\nfmr f2, f1\nfmr f1, f30\nbl force_scale\nfdivs f0, f1, f30\n",
    "lfs f1, 0x20(r1)\nlfs f2, 0x24(r1)\nfmuls f1, f0, f1\nfmuls f2, f0, f2\n",
    "stfs f1, 0(r31)\nstfs f2, 4(r31)\n",
    "planar_end:\n",
    "lfd f31, 0x8(r1)\nlfd f30, 0x10(r1)\nlwz r31, 0x18(r1)\nlwz r30, 0x1C(r1)\n",
    "lwz r0, 0x3C(r1)\nmtlr r0\naddi r1, r1, 0x38\nblr\n",

    "force_scale:\n",
    f("lwz r0, 0x%X(r3)\ncmpwi r0, 2\nbne scale_water\nlwz r0, 0x%X(r3)\nb scale_index\n",
      r.out_of_water, r.restraint),
    "scale_water:\nli r0, 4\nscale_index:\nslwi r0, r0, 2\n",
    f("lwz r9, %d(r13)\nadd r5, r9, r0\n", r.tweak),
    "lfs f7, 0xA4(r5)\nlfs f4, 0x44(r5)\nlfs f8, 0x4(r5)\n",
    f("lfs f0, %d(r2)\nfmuls f4, f3, f4\nfmuls f6, f0, f4\nfmuls f5, f7, f6\n", r.sixty),
    f("lfs f6, 0x%X(r3)\nlfs f9, %d(r2)\nfmuls f5, f5, f6\nfmuls f3, f3, f8\n", r.mass, r.zero),
    f("lfs f0, %d(r2)\nfsubs f4, f9, f1\nfdivs f6, f5, f3\nfabs f4, f4\nfrsp f3, f4\n",
      r.epsilon_1e5),
    "fcmpo cr0, f3, f0\nmfcr r0\nsrwi. r0, r0, 31\nbne scale_done\n",
    "fcmpo cr0, f1, f9\nble scale_negative\nfmr f5, f6\nb scale_sign\n",
    "scale_negative:\nfneg f5, f6\nscale_sign:\n",
    f("fsubs f0, f7, f6\nlfs f3, %d(r2)\nlfs f4, %d(r2)\n", r.minus_one, r.one),
    "fmuls f0, f1, f0\nfadds f0, f0, f5\nfsubs f0, f0, f2\nfdivs f0, f0, f7\n",
    "fsubs f1, f3, f0\nfsel f1, f1, f3, f0\nfsubs f0, f1, f4\nfsel f0, f0, f4, f1\n",
    "fmuls f9, f8, f0\n",
    "scale_done:\nfmr f1, f9\nblr\n",
  })
end

-- PrimeHack's CPlayer::GetDampedClampedVelocityWR (r3 the result, r4 the player), written over
-- the game's: the GameCube game takes the friction off each local axis on its own and clamps the
-- forward speed only, which turns a diagonal into a forward move; this one, the Trilogy's, damps
-- and clamps the horizontal speed as a whole (and the forward speed while orbiting).
local function damped_text(r)
  local f = string.format
  return table.concat({
    "stwu r1, -0x48(r1)\nmflr r0\nstw r0, 0x4C(r1)\nstfd f31, 0x8(r1)\nstfd f30, 0x10(r1)\n",
    "stfd f29, 0x18(r1)\nstfd f28, 0x20(r1)\nstw r31, 0x28(r1)\nstw r30, 0x2C(r1)\n",
    "stw r29, 0x30(r1)\nmr r31, r4\nmr r30, r3\n",
    f("addi r3, r1, 0x38\naddi r4, r31, 0x%X\naddi r5, r31, 0x%X\nbl `0x%X`\n", r.transform,
      r.velocity, r.transpose_rotate),
    f("addi r3, r1, 0x38\nbl `0x%X`\nfmr f28, f1\n", r.mag2d),
    f("lwz r3, %d(r13)\nlwz r0, 0x%X(r31)\ncmpwi r0, 2\nbne damp_water\n", r.tweak,
      r.out_of_water),
    f("lwz r29, 0x%X(r31)\nb damp_index\ndamp_water:\nli r29, 4\ndamp_index:\n", r.restraint),
    "slwi r0, r29, 2\nadd r3, r0, r3\nlfs f30, 0xA4(r3)\nlfs f29, 0x44(r3)\n",
    f("lwz r0, 0x%X(r31)\ncmpwi r0, 0\nbne damp_orbit\n", r.orbit),
    f("lwz r0, 0x%X(r31)\ncmpwi r0, 0\nbeq damp_ground\n", r.move_state),
    f("lfs f0, 0x%X(r31)\nlfs f1, %d(r2)\nfcmpo cr0, f0, f1\nbgt damp_orbit\n", r.grapple_timeout,
      r.zero),
    "damp_ground:\ncmpwi r29, 1\nbne damp_friction\n",
    f("lfs f29, %d(r2)\nlfs f0, 0x%X(r31)\nfdivs f0, f28, f0\nfmuls f29, f0, f29\n",
      r.seven_halves, r.mass),
    f("damp_friction:\nlfs f0, %d(r2)\nlfs f1, 0x38(r1)\nlfs f2, 0x3C(r1)\n", r.damp_epsilon),
    "fmuls f1, f1, f1\nfmuls f2, f2, f2\nfadds f1, f1, f2\nfcmpo cr0, f1, f0\ncror 2, 1, 2\n",
    "bne damp_z\n",
    f("lfs f0, %d(r2)\nfsubs f2, f28, f29\nfsubs f3, f0, f2\nfsel f2, f3, f0, f2\n", r.zero),
    "fsubs f0, f2, f30\nfsel f0, f0, f30, f2\nfdivs f1, f0, f28\naddi r3, r1, 0x38\n",
    f("bl `0x%X`\nb damp_z\n", r.scale2d),
    "damp_orbit:\nfneg f2, f30\nlfs f0, 0x3C(r1)\nfsubs f1, f2, f0\nfsel f0, f1, f2, f0\n",
    "fsubs f1, f0, f30\nfsel f0, f1, f30, f0\nstfs f0, 0x3C(r1)\n",
    f("damp_z:\nlwz r0, 0x%X(r31)\ncmpwi r0, 0\nbne damp_rotate\n", r.move_state),
    f("lfs f0, %d(r2)\nstfs f0, 0x40(r1)\n", r.zero),
    f("damp_rotate:\nmr r3, r30\naddi r4, r31, 0x%X\naddi r5, r1, 0x38\nbl `0x%X`\n", r.transform,
      r.mat_rotate),
    "lfd f31, 0x8(r1)\nlfd f30, 0x10(r1)\nlfd f29, 0x18(r1)\nlfd f28, 0x20(r1)\n",
    "lwz r31, 0x28(r1)\nlwz r30, 0x2C(r1)\nlwz r29, 0x30(r1)\nlwz r0, 0x4C(r1)\nmtlr r0\n",
    "addi r1, r1, 0x48\nblr\n",
  })
end

-- The code of revision r: the caves' addresses and words, and the damped velocity function.
local function build(r)
  local strafe, err = asm.assemble(strafe_text(r), CODE)
  if not strafe then error("assembling the strafe cave for " .. r.name .. ": " .. err) end
  local force_address = CODE + #strafe * 4
  local force
  force, err = asm.assemble(force_text(r), force_address)
  if not force then error("assembling the force code for " .. r.name .. ": " .. err) end
  assert(force_address + #force * 4 <= S + mem.scratch_size,
         "the code does not fit in the scratch memory")
  local damped
  damped, err = asm.assemble(damped_text(r), r.damped)
  if not damped then error("assembling the damped velocity for " .. r.name .. ": " .. err) end
  assert(#damped <= DAMPED_WORDS, "the damped velocity function is longer than the game's")
  return {strafe = CODE, strafe_words = strafe, force = force_address, force_words = force,
          damped_words = damped}
end

-- ---- Finding the revision, installing and removing the hooks ----

local current = nil       -- the revision whose hooks are installed
local code = {}           -- the built code per revision
local damped_saved = {}   -- per revision, the game's damped velocity function, to put back
local warned = false

-- Whether the first words at address are words' first ones.
local function starts_with(address, words)
  local read = mem.read_words(address, 3)
  if not read then return false end
  for i = 1, 3 do
    if read[i] ~= words[i] then return false end
  end
  return true
end

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- Whether a site holds its original instruction or a branch into the patch's code, followed by
-- the next instruction.
local function site_present(site, original, after)
  if mem.u32(site + 4) ~= after then return false end
  local word = mem.u32(site)
  if word == original then return true end
  local target = branch_target(site, word or 0)
  return target ~= nil and target >= CODE and target < S + mem.scratch_size
end

local function detect()
  for _, r in ipairs(REVISIONS) do
    if site_present(r.strafe_site, r.strafe_original, STRAFE_AFTER) and
       site_present(r.force_site, r.force_original, FORCE_AFTER) then
      code[r] = code[r] or build(r)
      -- The damped velocity function: the game's, or the patch's (a savestate made with it).
      if starts_with(r.damped, DAMPED_ORIGINAL) or starts_with(r.damped, code[r].damped_words) then
        return r
      end
    end
  end
  return nil
end

local function install()
  local r = detect()
  if not r then
    if not warned then
      warn("not installed: no code of Metroid Prime NTSC-U 1.00 or 1.02 at the movement sites")
      warned = true
    end
    current = nil
    return
  end
  local c = code[r]
  if starts_with(r.damped, DAMPED_ORIGINAL) then
    damped_saved[r] = mem.read_words(r.damped, DAMPED_WORDS)
  end
  local changed = mem.ensure_code(c.strafe, c.strafe_words)
  changed = mem.ensure_code(c.force, c.force_words) or changed
  changed = mem.ensure_code(r.damped, c.damped_words) or changed
  changed = mem.ensure_code(r.strafe_site, asm.branch(r.strafe_site, c.strafe)) or changed
  changed = mem.ensure_code(r.force_site, asm.branch(r.force_site, c.force)) or changed
  if changed or r ~= current then log(r.name .. ": hooks installed, code at", mem.hex(CODE)) end
  current = r
  warned = false
end

-- The game's code back. The damped velocity function only when the patch read it: a savestate made
-- with the patch keeps the patch's (the Trilogy's), which works without the rest.
local function uninstall()
  local r = current
  current = nil
  mem.write_u32(ACTIVE, 0)
  if not r then return end
  for _, site in ipairs({{r.strafe_site, r.strafe_original}, {r.force_site, r.force_original}}) do
    if branch_target(site[1], mem.u32(site[1]) or 0) then mem.write_code(site[1], site[2]) end
  end
  if damped_saved[r] and starts_with(r.damped, code[r].damped_words) then
    mem.write_code(r.damped, damped_saved[r])
  end
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
local function playing_player(r)
  local sm = r.state_manager
  local player = mem.ptr(sm + 0x84C)
  if not player or mem.u32(sm + 0xF90) ~= 0 then return nil end
  if mem.u32(player + r.camera) ~= 0 or mem.u32(player + r.ball) ~= 0 then return nil end
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
  current = nil
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not current then install() end
  local player = current and playing_player(current)
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
  -- Last: arms the caves for this player.
  mem.write_u32(ACTIVE, player)
end

function on_unload()
  uninstall()
  release()
end
