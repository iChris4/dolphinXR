-- Red Steel (NTSC-U): the gun follows a VR controller.
--
-- Drawing: each mesh is a display list drawn by 80446490 with the render context's modelview
-- (r27 + 0x6FC, the world matrix times the view, in view space), which it loads with
-- GXLoadPosMtxImm and from which 8030E38C makes the normal matrix. The player's arm and weapon
-- are drawn last, with their own projection (near plane 1 game unit; the world's is 10). A cave
-- on that load, for the meshes drawn with that projection, puts the weapon where the controller
-- is (the modelview becomes the controller's pose in the camera's view space: the drawn gun does
-- not lag when the camera turns) and moves the arm out of view (it is skinned on the CPU, its
-- origin at the player's feet, so it would stay where the game holds the gun). A weapon of
-- several meshes keeps them where they are on its first one.
--
-- Aiming: the game shoots from the camera through the Wii pointer. While the gun is drawn and
-- the game is not paused, a second cave, after KPADRead, puts KPAD's pointer where a point along
-- the barrel is on the game's screen, through the projection of the world's last draw (which the
-- first cave keeps).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Gun follows the controller",
  author = "DolphinXR",
  version = "1.0",
  description = "The gun is drawn in your hand, where the VR controller is and pointing where it "
    .. "points, the game's arm is hidden, and the game aims where the gun points. For Red Steel "
    .. "NTSC-U.",
  games = {"REDE41"},
  revision = 0,
  scratch = 0x300,
  settings = {
    {name = "hand", label = "Gun hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "follow", label = "Gun follows the controller", type = "bool", default = true},
    {name = "hide_arm", label = "Hide the arm", type = "bool", default = true,
     description = "While the gun follows the controller, the game's arm is not drawn: it stays "
       .. "where the game holds the gun."},
    {name = "true_aim", label = "True aim", type = "bool", default = true,
     description = "While the gun is out, the Wii pointer is where the gun points: the game "
       .. "shoots along the barrel."},
    {name = "aim_distance", label = "Aim distance", unit = "m", default = 8, min = 1, max = 50,
     step = 0.5,
     description = "True aim: the distance at which the shot crosses the barrel's line (the game "
       .. "shoots from the eyes, not from the barrel)."},
    {name = "scale", label = "Hand movement scale", default = 1, min = 0.5, max = 2, step = 0.05,
     description = "Times the profile's Units per Meter: above 1 the gun moves farther than the "
       .. "hand."},
    {name = "pitch", label = "Pitch offset", unit = "deg", default = 0, min = -90, max = 90,
     step = 1},
    {name = "yaw", label = "Yaw offset", unit = "deg", default = 0, min = -90, max = 90, step = 1},
    {name = "roll", label = "Roll offset", unit = "deg", default = 0, min = -90, max = 90,
     step = 1},
    {name = "offset_x", label = "Right offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "offset_y", label = "Up offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "offset_z", label = "Back offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "test_pitch", label = "Test pose", unit = "deg", default = 0, min = -60, max = 60,
     step = 5,
     description = "Not 0: the gun is held at this pitch in front of the camera instead of "
       .. "following the controller, to check the patch without a headset."},
  },
}

-- ---- The game (Red Steel NTSC-U) ----

-- 80446490 draws a mesh: "addi r3, r27, 0x6FC; li r4, 0; bl GXLoadPosMtxImm", r27 the render
-- context, then 8030E38C loads the inverse transpose of the same matrix as the normal matrix.
local HOOK = 0x80446814
local HOOK_ORIGINAL = 0x4808C80D       -- bl 804D3020
local LOAD_POS_MATRIX = 0x804D3020     -- GXLoadPosMtxImm
local INVERSE = 0x804B73F0             -- PSMTXInverse(src, dst)
local CONCAT = 0x804B72D4              -- PSMTXConcat(a, b, ab): ab = a * b
local CONTEXT_PROJECTION = 0x6BC       -- the context's projection (4x4, GX rows)
local CONTEXT_NEAR = 0x6E8             -- its row 2 column 3: -near
local CONTEXT_W = 0x6F4                -- its row 3 column 2: -1 for a perspective projection
-- KPADRead (80525F60) fills the game's statuses of a channel (r28 + channel * 1360, 0x88 bytes
-- each, the newest first) and returns how many it read; "mulli r4, r23, 1360" after the call
-- starts the game's reading of them (r23 the channel), which smooths the pointer (80463AE8).
local KPAD_HOOK = 0x8040F37C
local KPAD_HOOK_ORIGINAL = 0x1C970550
local KPAD_STATUS_SIZE = 0x88
local KPAD_POS = 0x20                  -- the pointer: x right, y down, -1 to 1 across the screen
local SIGNATURE = {{0x8044680C, 0x387B06FC}, {0x80446810, 0x38800000},
                   {0x80446818, 0x48088215}, {0x8040F378, 0x48116BE9},
                   {0x8040F380, 0x906DC2B8}}
-- The weapon projection's near plane is 1 game unit, the world's 10: a draw whose projection
-- has its near plane closer than NEAR_LIMIT is the arm's or the weapon's.
local NEAR_LIMIT = 5.0
-- The arm is skinned on the CPU: its modelview's origin is the player's feet, more than ARM_Y
-- below the eyes even crouched (the gun is about 20 units below them, in centimeters).
local ARM_Y = -60.0
local HIDDEN_Z = 100000.0              -- behind the camera
-- [SCENES] + 0xC is null while the game is paused (its pause menu still draws the gun) and the
-- running scene otherwise (the level's world time step is at [[SCENES] + 0xC] + 0xFC).
local SCENES = 0x808A7C68
local RUNNING_SCENE = 0xC

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local ARMED = S + 0x00         -- 1 when the cave moves the weapon
local IN_PASS = S + 0x04       -- 1 since the first draw with the weapon projection
local BODY_SET = S + 0x08      -- 1 once this pass's first weapon mesh took the hand pose
local RETURN = S + 0x0C        -- the return address in 80446490
local MATRIX = S + 0x10        -- the modelview being loaded
local DRAWN = S + 0x14         -- counts the weapon meshes moved
local ARMS = S + 0x18          -- counts the arm draws seen
local HIDE = S + 0x1C          -- 1: the arm is moved out of view
local LIMITS = S + 0x20        -- f32: -NEAR_LIMIT, ARM_Y, HIDDEN_Z, -0.5
local LAST = S + 0x30          -- the translation the cave last wrote (3 words)
local AIMING = S + 0x3C        -- 1 when the KPAD cave writes POS
local HAND = S + 0x40          -- 3x4: the weapon's body in the camera's view space
local INV_BODY = S + 0x70      -- 3x4: the inverse of the body's game modelview
local TEMP = S + 0xA0          -- 3x4
local WORLD_SCALE = S + 0xD0   -- f32: the world projection's x and y scales
local POS = S + 0xD8           -- f32: the KPAD pointer of the true aim
local CAVE = S + 0xE0
local KPAD_CAVE = nil          -- after CAVE

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

local function o(address) return address - S end

-- In place of "bl GXLoadPosMtxImm" (r3 the modelview, r4 0, r27 the context): the volatile
-- registers are free, since the game expects that call to change them. The weapon path saves
-- the return address for its calls and comes back with r3 and r4 as they were.
local function cave_words()
  local reload = load_address("r11", S)
  local t = {reload}
  t[#t + 1] = string.format("lwz r12, 0x%X(r11)\ncmpwi r12, 0\nbeq tail\n", o(ARMED))
  -- The weapon projection: a perspective one (its last row 0, 0, -1, 0) with a near plane.
  t[#t + 1] = string.format("lfs f0, 0x%X(r27)\nlfs f1, 0x%X(r11)\nfcmpo cr0, f0, f1\n" ..
                            "bge world\n", CONTEXT_W, o(LIMITS) + 12)
  t[#t + 1] = string.format("lfs f0, 0x%X(r27)\nlfs f1, 0x%X(r11)\nfcmpo cr0, f0, f1\n" ..
                            "bgt pass\n", CONTEXT_NEAR, o(LIMITS))
  -- A world draw in perspective: its projection's scales, for the true aim.
  t[#t + 1] = string.format("lwz r12, 0x%X(r27)\nstw r12, 0x%X(r11)\nlwz r12, 0x%X(r27)\n" ..
                            "stw r12, 0x%X(r11)\n", CONTEXT_PROJECTION, o(WORLD_SCALE),
                            CONTEXT_PROJECTION + 0x14, o(WORLD_SCALE) + 4)
  t[#t + 1] = string.format("world:\nli r12, 0\nstw r12, 0x%X(r11)\nb tail\n", o(IN_PASS))
  -- Its first draw since a world draw starts a pass: the next weapon mesh is the body.
  t[#t + 1] = string.format("pass:\nlwz r12, 0x%X(r11)\ncmpwi r12, 0\nbne known\n" ..
                            "li r12, 1\nstw r12, 0x%X(r11)\nli r12, 0\nstw r12, 0x%X(r11)\n",
                            o(IN_PASS), o(IN_PASS), o(BODY_SET))
  -- A matrix the cave wrote already (the context's modelview drawn again): as it is.
  t[#t + 1] = "known:\n"
  for i, offset in ipairs({0x0C, 0x1C, 0x2C}) do
    t[#t + 1] = string.format("lwz r12, 0x%X(r3)\nlwz r10, 0x%X(r11)\ncmpw r12, r10\n%s\n",
                              offset, o(LAST) + (i - 1) * 4, i < 3 and "bne arm_test" or "beq tail")
  end
  t[#t + 1] = string.format("arm_test:\nlfs f0, 0x1C(r3)\nlfs f1, 0x%X(r11)\n" ..
                            "fcmpo cr0, f0, f1\nblt arm\n", o(LIMITS) + 4)
  t[#t + 1] = string.format("lwz r12, 0x%X(r11)\naddi r12, r12, 1\nstw r12, 0x%X(r11)\n" ..
                            "mflr r0\nstw r0, 0x%X(r11)\nstw r3, 0x%X(r11)\n" ..
                            "lwz r12, 0x%X(r11)\ncmpwi r12, 0\nbne part\n", o(DRAWN), o(DRAWN),
                            o(RETURN), o(MATRIX), o(BODY_SET))
  -- The body: INV_BODY = inverse(modelview), modelview = HAND.
  t[#t + 1] = string.format("li r12, 1\nstw r12, 0x%X(r11)\naddi r4, r11, 0x%X\nbl `0x%X`\n",
                            o(BODY_SET), o(INV_BODY), INVERSE)
  t[#t + 1] = reload .. string.format("addi r9, r11, 0x%X\nlwz r10, 0x%X(r11)\n" ..
                                      "addi r10, r10, -4\nli r0, 12\nmtctr r0\n" ..
                                      "copy:\nlwzu r0, 4(r9)\nstwu r0, 4(r10)\nbdnz copy\n" ..
                                      "b out\n", o(HAND) - 4, o(MATRIX))
  -- A part: modelview = HAND * INV_BODY * modelview.
  t[#t + 1] = string.format("part:\naddi r3, r11, 0x%X\nlwz r4, 0x%X(r11)\naddi r5, r11, 0x%X\n" ..
                            "bl `0x%X`\n", o(INV_BODY), o(MATRIX), o(TEMP), CONCAT)
  t[#t + 1] = reload .. string.format("addi r3, r11, 0x%X\naddi r4, r11, 0x%X\n" ..
                                      "lwz r5, 0x%X(r11)\nbl `0x%X`\n", o(HAND), o(TEMP),
                                      o(MATRIX), CONCAT)
  t[#t + 1] = "out:\n" .. reload .. string.format("lwz r3, 0x%X(r11)\n", o(MATRIX))
  for i, offset in ipairs({0x0C, 0x1C, 0x2C}) do
    t[#t + 1] = string.format("lwz r12, 0x%X(r3)\nstw r12, 0x%X(r11)\n", offset,
                              o(LAST) + (i - 1) * 4)
  end
  t[#t + 1] = string.format("lwz r0, 0x%X(r11)\nmtlr r0\nli r4, 0\n", o(RETURN))
  t[#t + 1] = string.format("tail:\nb `0x%X`\n", LOAD_POS_MATRIX)
  -- The arm: counted, and moved behind the camera when hidden.
  t[#t + 1] = string.format("arm:\nlwz r12, 0x%X(r11)\naddi r12, r12, 1\nstw r12, 0x%X(r11)\n" ..
                            "lwz r12, 0x%X(r11)\ncmpwi r12, 0\nbeq tail\n" ..
                            "lwz r12, 0x%X(r11)\nstw r12, 0x2C(r3)\nb tail\n", o(ARMS), o(ARMS),
                            o(HIDE), o(LIMITS) + 8)
  local words, err = asm.assemble(table.concat(t), CAVE)
  if not words then error("assembling the cave: " .. err) end
  -- In place of "mulli r4, r23, 1360" after KPADRead (r3 the statuses read, kept): for the first
  -- Wii Remote, the true aim's pointer in each of them.
  KPAD_CAVE = CAVE + #words * 4
  local k = reload ..
            string.format("lwz r12, 0x%X(r11)\ncmpwi r12, 0\nbeq kdone\n", o(AIMING)) ..
            "cmpwi r23, 0\nbne kdone\ncmpwi r3, 0\nble kdone\nmtctr r3\nmr r10, r28\n" ..
            string.format("lwz r9, 0x%X(r11)\nlwz r12, 0x%X(r11)\n", o(POS), o(POS) + 4) ..
            string.format("kloop:\nstw r9, 0x%X(r10)\nstw r12, 0x%X(r10)\naddi r10, r10, 0x%X\n" ..
                          "bdnz kloop\n", KPAD_POS, KPAD_POS + 4, KPAD_STATUS_SIZE) ..
            string.format("kdone:\nmulli r4, r23, 1360\nb `0x%X`\n", KPAD_HOOK + 4)
  local kwords, kerr = asm.assemble(k, KPAD_CAVE)
  if not kwords then error("assembling the KPAD cave: " .. kerr) end
  for _, word in ipairs(kwords) do words[#words + 1] = word end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the caves do not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hooks ----

local cave = nil
local installed = false
local waiting_logged = false

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- A branch from a hook into the patches' scratch memory is this patch's cave, maybe at another
-- address, kept by a savestate made while it ran: installing replaces it.
local function our_branch(hook, word)
  local target = branch_target(hook, word or 0)
  return target ~= nil and target >= 0x80001800 and target < 0x80003000
end

local function hook_free(hook, original)
  local word = mem.u32(hook)
  return word == original or our_branch(hook, word)
end

local function present()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then return false end
  end
  return hook_free(HOOK, HOOK_ORIGINAL) and hook_free(KPAD_HOOK, KPAD_HOOK_ORIGINAL)
end

local function install()
  if not present() then
    installed = false
    if not waiting_logged then log("waiting for the game's mesh drawing and KPAD code") end
    waiting_logged = true
    return
  end
  cave = cave or cave_words()
  mem.write_f32s(LIMITS, -NEAR_LIMIT, ARM_Y, HIDDEN_Z, -0.5)
  local changed = mem.ensure_code(CAVE, cave)
  changed = mem.ensure_code(HOOK, asm.branch(HOOK, CAVE, true)) or changed
  changed = mem.ensure_code(KPAD_HOOK, asm.branch(KPAD_HOOK, KPAD_CAVE)) or changed
  if changed or not installed then
    log("hooks installed, caves at", mem.hex(CAVE), "and", mem.hex(KPAD_CAVE))
  end
  installed = true
  waiting_logged = false
end

local function uninstall()
  mem.write_u32(ARMED, 0)
  mem.write_u32(AIMING, 0)
  if our_branch(HOOK, mem.u32(HOOK)) then mem.write_code(HOOK, HOOK_ORIGINAL) end
  if our_branch(KPAD_HOOK, mem.u32(KPAD_HOOK)) then
    mem.write_code(KPAD_HOOK, KPAD_HOOK_ORIGINAL)
  end
  installed = false
end

-- ---- Tracking ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local sin, cos, rad = math.sin, math.cos, math.rad
local raw_pose, f32s, write_f32s, u32 = vr.raw_pose, mem.f32s, mem.write_f32s, mem.u32

-- The Hamilton product a * b of two quaternions (x, y, z, w).
local function qmul(ax, ay, az, aw, bx, by, bz, bw)
  return aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
         aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz
end

-- The controller's pose seen from the game camera (OpenXR view space, which has the axes of the
-- game's view space: +X right, +Y up, -Z forward; meters), or the test pose.
local function hand_pose()
  if settings.test_pitch ~= 0 then
    local h = rad(settings.test_pitch) * 0.5
    return true, 0.2, -0.25, -0.35, sin(h), 0, 0, cos(h)
  end
  return raw_pose(settings.hand, "view")
end

-- HAND: the gun's modelview in the camera's view space. The gun model's barrel is its +X, its
-- right +Y, its top +Z (a mirrored frame, as the game's matrices are): the columns are the
-- controller's forward, right and up axes, then its position in game units.
local function write_hand(px, py, pz, x, y, z, w)
  local h = rad(settings.yaw) * 0.5
  x, y, z, w = qmul(x, y, z, w, 0, sin(h), 0, cos(h))
  h = rad(settings.pitch) * 0.5
  x, y, z, w = qmul(x, y, z, w, sin(h), 0, 0, cos(h))
  h = rad(settings.roll) * 0.5
  x, y, z, w = qmul(x, y, z, w, 0, 0, sin(h), cos(h))
  local rx, ry, rz = 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)
  local ux, uy, uz = 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)
  local fx, fy, fz = -2 * (x * z + y * w), -2 * (y * z - x * w), -(1 - 2 * (x * x + y * y))
  local scale = vr.units_per_meter() * settings.scale
  local tx = (px + settings.offset_x) * scale
  local ty = (py + settings.offset_y) * scale
  local tz = (pz + settings.offset_z) * scale
  return write_f32s(HAND, fx, rx, ux, tx, fy, ry, uy, ty, fz, rz, uz, tz)
end

-- Whether the cave moved a weapon mesh in the last fields (the game draws at 30 fps).
local RECENT_FIELDS = 6
local last_drawn = 0
local drawn_field = -1000

local function gun_drawn()
  local drawn = u32(DRAWN) or 0
  if drawn ~= last_drawn then
    last_drawn = drawn
    drawn_field = game.field()
  end
  return game.field() - drawn_field <= RECENT_FIELDS
end

-- True aim: KPAD's pointer where the point aim_distance along the drawn barrel is on the game's
-- screen, through the world's projection (the game shoots from the camera through the pointer).
-- The emulated pointer is kept on the screen meanwhile, so that KPAD does not lose the sensor
-- bar when the aim leaves the virtual screen.
local aiming = false

local function stop_aiming()
  if aiming then
    mem.write_u32(AIMING, 0)
    vr.keep_pointer_on_screen(false)
    aiming = false
  end
end

local function aim()
  local x_scale, y_scale = f32s(WORLD_SCALE, 2)
  local fx, _, _, tx, fy, _, _, ty, fz, _, _, tz = f32s(HAND, 12)
  if not x_scale or x_scale <= 0 or y_scale <= 0 or not fx then return false end
  local d = settings.aim_distance * vr.units_per_meter() * settings.scale
  local x, y, z = tx + fx * d, ty + fy * d, tz + fz * d
  if z > -1 then return false end
  local u = math.max(-1, math.min(1, x_scale * x / -z))
  local v = math.max(-1, math.min(1, y_scale * y / -z))
  if not write_f32s(POS, u, -v) then return false end
  if not aiming then
    vr.keep_pointer_on_screen(true)
    aiming = true
  end
  -- Last: arms the KPAD cave once POS is written.
  mem.write_u32(AIMING, 1)
  return true
end

local function paused()
  local scenes = mem.ptr(SCENES)
  return not scenes or (u32(scenes + RUNNING_SCENE) or 0) == 0
end

local function track()
  local armed = false
  if settings.follow then
    local ok, px, py, pz, qx, qy, qz, qw = hand_pose()
    armed = ok and write_hand(px, py, pz, qx, qy, qz, qw)
  end
  mem.write_u32(HIDE, (armed and settings.hide_arm) and 1 or 0)
  -- Last: arms the cave once HAND is written.
  mem.write_u32(ARMED, armed and 1 or 0)
  if not (armed and settings.true_aim and gun_drawn() and not paused() and aim()) then
    stop_aiming()
  end
end

-- ---- Hooks ----

local function reset()
  mem.write_u32(ARMED, 0)
  mem.write_u32(AIMING, 0)
  mem.write_u32(IN_PASS, 0)
  mem.write_u32(BODY_SET, 0)
  aiming = false
end

function on_load()
  reset()
  install()
end

function on_state_loaded()
  reset()
  installed = false
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then
    install()
  end
  if installed then
    track()
  end
end

function on_unload()
  stop_aiming()
  uninstall()
end
