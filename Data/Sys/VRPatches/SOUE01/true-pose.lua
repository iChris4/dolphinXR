-- The Legend of Zelda: Skyward Sword (NTSC-U 1.00): the Wii Remote's orientation in the game is
-- the VR controller's.
--
-- The game reads the Wii Remote through KPAD, which integrates the MotionPlus samples into the
-- remote's direction (KPADRead's MotionPlus step, 803F5AD0) and copies its status to the game
-- (EGG's controller, [805CB0B8] + 0x18), which takes the sword's, the items' aim and the
-- Loftwing's tilt from it. A cave right after that step, before the copy, replaces the direction
-- with the controller's real orientation: the game follows the controller exactly, without the
-- emulated gyro's lag and drift (the next samples integrate from it). The rates and the
-- accelerometer stay the emulated ones, which the swings also read.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "True Wii Remote pose",
  author = "DolphinXR",
  version = "1.0",
  description = "The game reads the Wii Remote's orientation from the VR controller instead of "
    .. "integrating the emulated MotionPlus: Link's sword, the aim of the items and the "
    .. "Loftwing follow the controller exactly, without drift or recalibration. For Skyward "
    .. "Sword NTSC-U 1.00.",
  games = {"SOUE01"},
  revision = 0,
  scratch = 0x80,
  settings = {
    {name = "hand", label = "Wii Remote hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "true_pose", label = "True pose", type = "bool", default = true,
     description = "The game reads the remote's orientation from the controller."},
    {name = "pitch", label = "Pitch offset", unit = "deg", default = 0, min = -45, max = 45,
     step = 1,
     description = "Tilts the remote up (positive) or down from the controller, on top of the "
       .. "Wii Remote pitch of the VR settings."},
    {name = "yaw", label = "Yaw offset", unit = "deg", default = 0, min = -45, max = 45, step = 1,
     description = "Turns the remote right (positive) or left from the controller."},
    {name = "test_pitch", label = "Test pose", unit = "deg", default = 0, min = -60, max = 60,
     step = 5,
     description = "Not 0: the remote is held at this pitch, pointing at the screen, instead of "
       .. "following the controller, to check the patch without a headset."},
  },
}

-- ---- The game (Skyward Sword NTSC-U 1.00) ----

-- KPADRead: after the MotionPlus step (bl 803F5AD0, r21 the channel's status), "li r0, 30" starts
-- copying the status to the game's buffer.
local HOOK = 0x803F1F70
local HOOK_ORIGINAL = 0x3800001E
local STATUS = 0x805FC888       -- KPAD's status of the first Wii Remote
local DEVICE = 0x5C             -- u8: 5, 6 or 7 with a MotionPlus (alone, Nunchuk, Classic)
local DIRECTION = 0xC8          -- the remote's left, top and pointing axes (x left, y up,
                                -- z toward the screen)
local SIGNATURE = {{0x803F1F6C, 0x48003B65}, {0x803F1F74, 0x38BAFFFC}, {0x803F1F78, 0x3895FFFC}}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local ARMED = S + 0x00          -- STATUS when the cave writes; 0 = not
local DIR = S + 0x04            -- the direction (9 floats)
local CAVE = S + 0x30

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- In place of "li r0, 30" after the MotionPlus step: for the armed status, the direction is
-- written there before it is copied to the game. KPADRead keeps no value in r9, r10, r11, r12,
-- ctr or cr0 across that point (it just called).
local function cave_words()
  local text = load_address("r11", S) ..
               "lwz r12, 0(r11)\ncmplw r12, r21\nbne done\n" ..
               string.format("addi r9, r11, 0x%X\naddi r10, r21, 0x%X\n", DIR - S - 4,
                             DIRECTION - 4) ..
               "li r0, 9\nmtctr r0\ncopy:\nlwzu r0, 4(r9)\nstwu r0, 4(r10)\nbdnz copy\n" ..
               string.format("done:\nli r0, 30\nb `0x%X`\n", HOOK + 4)
  local words, err = asm.assemble(text, CAVE)
  if not words then error("assembling the cave: " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hook ----

local cave = nil
local installed = false
local waiting_logged = false

local function branch_target(from, word)
  if word & 0xFC000003 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- A branch from the hook into the patches' scratch memory is this patch's cave, maybe at another
-- address, kept by a savestate made while it ran: installing replaces it.
local function our_branch(word)
  local target = branch_target(HOOK, word or 0)
  return target ~= nil and target >= 0x80001800 and target < 0x80003000
end

local function present()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then return false end
  end
  local word = mem.u32(HOOK)
  return word == HOOK_ORIGINAL or our_branch(word)
end

local function install()
  if not present() then
    installed = false
    if not waiting_logged then log("waiting for the game's KPAD code") end
    waiting_logged = true
    return
  end
  cave = cave or cave_words()
  local changed = mem.ensure_code(CAVE, cave)
  changed = mem.ensure_code(HOOK, asm.branch(HOOK, CAVE)) or changed
  if changed or not installed then log("hook installed, cave at", mem.hex(CAVE)) end
  installed = true
  waiting_logged = false
end

local function uninstall()
  mem.write_u32(ARMED, 0)
  if our_branch(mem.u32(HOOK)) then mem.write_code(HOOK, HOOK_ORIGINAL) end
  installed = false
end

-- ---- Tracking ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local sin, cos, rad = math.sin, math.cos, math.rad
local raw_pose, write_f32s, u8 = vr.raw_pose, mem.write_f32s, mem.u8

-- The Hamilton product a * b of two quaternions (x, y, z, w).
local function qmul(ax, ay, az, aw, bx, by, bz, bw)
  return aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
         aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz
end

-- The right (+X), up (+Y) and forward (-Z) axes of a rotation of OpenXR's view space (the columns
-- of its matrix), in the game's view axes (+X right, +Y up, +Z forward: z negated).
local function axes(x, y, z, w)
  return 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), -2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), -2 * (y * z + x * w),
         -2 * (x * z + y * w), -2 * (y * z - x * w), 1 - 2 * (x * x + y * y)
end

-- The pitch between the controller's aim and the emulated Wii Remote (DolphinXR's
-- WiimoteMotionPitch, 20 by default: a held remote points above a Touch controller's aim). The
-- key is not one vr.setting reads in every version: the default then.
local remote_pitch = 20
local function read_remote_pitch()
  local ok, value = pcall(vr.setting, "WiimoteMotionPitch")
  value = ok and tonumber(value or "")
  if value then remote_pitch = value end
end

-- The controller's orientation relative to the game camera as DolphinXR renders it (the
-- screen's frame; the reference space in flat-screen mode), or the test pose.
local function hand_orientation()
  if settings.test_pitch ~= 0 then
    local h = rad(settings.test_pitch - remote_pitch) * 0.5
    return true, sin(h), 0, 0, cos(h)
  end
  local ok, _, _, _, qx, qy, qz, qw = raw_pose(settings.hand, "view")
  if not ok then
    ok, _, _, _, qx, qy, qz, qw = raw_pose(settings.hand)
  end
  return ok, qx, qy, qz, qw
end

-- KPAD's direction: the remote's left, top and pointing axes in KPAD's frame (x left, y up,
-- z toward the screen: the game's view axes with x negated), from the controller's orientation
-- turned by the yaw offset and pitched up by the remote pitch and the pitch offset. Measured
-- against the emulated MotionPlus (Red Steel 2, the same KPAD): pointing right gives -x, rolling
-- the right side up turns the left axis down.
local function write_direction(qx, qy, qz, qw)
  local h = rad(-settings.yaw) * 0.5
  qx, qy, qz, qw = qmul(0, sin(h), 0, cos(h), qx, qy, qz, qw)
  local rx, ry, rz, ux, uy, uz, fx, fy, fz = axes(qx, qy, qz, qw)
  local p = rad(remote_pitch + settings.pitch)
  local cp, sp = cos(p), sin(p)
  local tx, ty, tz = ux * cp - fx * sp, uy * cp - fy * sp, uz * cp - fz * sp
  local ax, ay, az = fx * cp + ux * sp, fy * cp + uy * sp, fz * cp + uz * sp
  return write_f32s(DIR, rx, -ry, -rz, -tx, ty, tz, -ax, ay, az)
end

local function track()
  local armed = false
  local device = u8(STATUS + DEVICE)
  if settings.true_pose and device and device >= 5 and device <= 7 then
    local ok, qx, qy, qz, qw = hand_orientation()
    armed = ok and write_direction(qx, qy, qz, qw)
  end
  -- Last: arms the cave once the direction is written.
  mem.write_u32(ARMED, armed and STATUS or 0)
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(ARMED, 0)
  read_remote_pitch()
  install()
end

function on_state_loaded()
  mem.write_u32(ARMED, 0)
  installed = false
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then
    install()
    read_remote_pitch()
  end
  if installed then
    track()
  end
end

function on_unload()
  uninstall()
end
