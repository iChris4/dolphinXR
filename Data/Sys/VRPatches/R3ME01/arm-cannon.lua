-- Metroid Prime Trilogy (NTSC-U, Wii): the arm cannon follows a VR controller in Metroid Prime,
-- Metroid Prime 2: Echoes and Metroid Prime 3: Corruption.
--
-- A DolphinXR VR game patch, after PrimedGun's arm cannon tracking for Metroid Prime (PrimedGun,
-- created by Nobbie, GPL-2.0-or-later). The state managers, the player offsets and the cannon
-- updaters come from PrimeHack's R3ME01 tables (shiiion/dolphin). The hook points, the gun and
-- camera chains and the cave are this patch's own.
--
-- When the VR view removes the game camera's pitch (stick-turn.lua lets the pointer's edges tilt
-- the camera), the cannon is placed in the camera's level frame, so that it stays in the hand
-- whatever that pitch.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Arm cannon follows the controller",
  author = "DolphinXR, after PrimedGun (Nobbie), with PrimeHack's addresses",
  version = "1.2",
  description = "The arm cannon is aimed and placed by your VR controller, and shots and missiles "
    .. "leave it where it points; the scan visors of Metroid Prime and Echoes scan what their "
    .. "lens shows. For the three games of Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
  scratch = 0x440,
  -- The profile's Controller Anchor of the cannon textures would move the cannon twice.
  conflicts = {texture = {"Arm Cannon"}},
  settings = {
    {name = "hand", label = "Cannon hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "scale", label = "Hand movement scale", default = 1, min = 0.5, max = 2, step = 0.05,
     description = "Times the profile's Units per Meter: above 1 the cannon moves farther than "
       .. "the hand."},
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
    {name = "model_x", label = "Model right offset", unit = "m", default = 0, min = -0.5,
     max = 0.5, step = 0.01},
    {name = "model_y", label = "Model forward offset", unit = "m", default = 0, min = -1,
     max = 0.5, step = 0.01},
    {name = "model_z", label = "Model up offset", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "smoothing", label = "Smoothing", default = 0.24, min = 0, max = 0.9, step = 0.01,
     description = "Part of the previous pose kept each frame (PrimedGun: 0.24)."},
    {name = "test_pitch", label = "Test pose", unit = "deg", default = 0, min = -60, max = 60,
     step = 5,
     description = "Not 0: the cannon is held at this pitch in front of the camera instead of "
       .. "following the controller, to check the patch without a headset."},
  },
}

-- ---- The games ----

-- The camera of Metroid Prime and Echoes: the camera manager holds the camera's object id, and
-- the object list maps it to the object (PrimeHack's camera lookup).
local function object_camera(state_manager, camera_manager, xf)
  local manager = camera_manager()
  local id = manager and mem.u16(manager)
  if not id or id == 0xFFFF then return nil end
  local camera = mem.ptr(state_manager + 0x810, ((id & 0x3FF) << 3) + 4)
  return camera and camera + xf
end

-- Each game of the disc is its own executable, recognized by the code at its first site.
--
-- A site is a hook where the game is about to take a finished transform: its rotation rows at
-- base + matrix (row stride 0x10) and its position at base + position[1..3]. The cave writes the
-- hand's transform there when guard_reg holds the value guard(player, gun) returns. The first site
-- is the end of the cannon updater (PrimeHack's "XF updater"), which every aim mode reaches: the
-- cannon.
--
-- Corruption fires along the cannon's transform. The Wii versions of Metroid Prime and Echoes fire
-- at the point the aim ray hits, cast from the camera through the pointer. Their second site is a
-- ray: the cave puts the ray's origin at base + position[1..3] (where the camera's position was)
-- and its direction at base + direction[1..3], before the game casts it. The ray is the cannon's
-- (its position, its forward axis times AIM_LENGTH): shots and the lock-on aim where the cannon
-- points. While the scan visor is on, it goes from the eyes through the scan lens instead, which
-- the game draws at the pointer, on the virtual screen: the scan picks what the lens shows,
-- however far it is (along the cannon, the zone sat above the lens, the hand being below the
-- eyes).
--
-- player() returns the player when it is in first person and not morphed, gun(player) the gun,
-- camera() the camera's transform in world space, scanning(player) whether the scan visor is on;
-- nil when there is none.
local GAMES = {
  {
    name = "Metroid Prime",
    sites = {
      -- 8018BC74: every mode has composed the transform at sp+0x380, which goes to gun+0x4B0.
      {hook = 0x8018BC74, original = 0x387904B0, replaced = "addi r3, r25, 0x4B0\n",
       after = 0x38810380,                     -- addi r4, sp, 0x380
       guard_reg = "r25", guard = function(player, gun) return gun end,
       base = "r1", matrix = 0x380, position = {0x38C, 0x39C, 0x3AC}},
      -- 80015618: the aim ray of the player (r31): the camera's position at sp+0x18C (column 3
      -- of its transform at sp+0x180) and the direction through the pointer at sp+0xB0.
      {hook = 0x80015618, original = 0xC00101AC, replaced = "lfs f0, 0x1AC(r1)\n",
       after = 0xC021019C,                     -- lfs f1, 0x19C(sp)
       guard_reg = "r31", guard = function(player, gun) return player end,
       base = "r1", position = {0x18C, 0x19C, 0x1AC}, direction = {0xB0, 0xB4, 0xB8}},
    },
    player = function()
      local player = mem.ptr(0x804BF420 + 0x84C)
      -- Camera state 0 = first person; ball state 0 = not morphed.
      if not player or mem.u32(player + 0x2F0) ~= 0 or mem.u32(player + 0x2F4) ~= 0 then
        return nil
      end
      return player
    end,
    gun = function(player) return mem.ptr(player + 0x480) end,
    camera = function()
      return object_camera(0x804BF420, function() return mem.ptr(0x804BF420 + 0x868) end, 0x2C)
    end,
    -- The visor on screen and the one it switches to (2 = scan), as in metroid-state.lua.
    scanning = function()
      local powerups = mem.ptr(0x804BFCD4)
      return powerups ~= nil and
             (mem.u32(powerups + 0x18) == 2 or mem.u32(powerups + 0x1C) == 2)
    end,
  },
  {
    name = "Metroid Prime 2: Echoes",
    sites = {
      -- 8018ACB0: the position (sp+0x2A0) goes into the matrix (sp+0x368), copied to gun+0x3B0.
      {hook = 0x8018ACB0, original = 0xC00102A0, replaced = "lfs f0, 0x2A0(r1)\n",
       after = 0x387C03B0,                     -- addi r3, r28, 0x3B0
       guard_reg = "r28", guard = function(player, gun) return gun end,
       base = "r1", matrix = 0x368, position = {0x2A0, 0x2A4, 0x2A8}},
      -- 80018AA0: the aim ray of the player (r31): the camera's position at sp+0x1CC and the
      -- direction through the pointer at sp+0xBC.
      {hook = 0x80018AA0, original = 0xC00101EC, replaced = "lfs f0, 0x1EC(r1)\n",
       after = 0xC02101DC,                     -- lfs f1, 0x1DC(sp)
       guard_reg = "r31", guard = function(player, gun) return player end,
       base = "r1", position = {0x1CC, 0x1DC, 0x1EC}, direction = {0xBC, 0xC0, 0xC4}},
    },
    player = function()
      local player = mem.ptr(0x804E72E8 + 0x14F4)
      if not player or mem.u32(player + 0x374) ~= 0 or mem.u32(player + 0x378) ~= 0 then
        return nil
      end
      return player
    end,
    gun = function(player) return mem.ptr(player + 0xEA8) end,
    camera = function()
      return object_camera(0x804E72E8, function()
        local manager = mem.ptr(0x804E72E8 + 0x1514)
        return manager and manager + 0x10
      end, 0x20)
    end,
    scanning = function(player)
      local visors = mem.ptr(player + 0x12EC)
      return visors ~= nil and mem.u32(visors + 0x34) == 2
    end,
  },
  {
    name = "Metroid Prime 3: Corruption",
    sites = {
      -- 80183C28: the position (sp+0x208) goes into the matrix (sp+0x2D8), copied to r28+0x4C;
      -- r28 = gun + 0x4D8.
      {hook = 0x80183C28, original = 0xC0010208, replaced = "lfs f0, 0x208(r1)\n",
       after = 0x7F83E378,                     -- mr r3, r28
       guard_reg = "r28", guard = function(player, gun) return gun + 0x4D8 end,
       base = "r1", matrix = 0x2D8, position = {0x208, 0x20C, 0x210}},
    },
    player = function()
      local player = mem.ptr(0x805C6C68, 0x2184)
      if not player or mem.u32(player + 0x358) ~= 0 then return nil end
      return player
    end,
    gun = function(player) return mem.ptr(player + 0x3AEC) end,
    -- [[[0x805C6C78] + 0x34] + 0x20]: the chain the game follows to copy the camera into the gun.
    camera = function()
      local camera = mem.ptr(0x805C6C78, 0x34, 0x20)
      return camera and camera + 0x3C
    end,
  },
}

local BASE_MODEL_FORWARD = -0.080      -- PrimedGun's model offset, meters

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local BASIS = S + 0x00    -- 3x3 row-major: the hand's rotation in the camera's axes
local OFFSET = S + 0x24   -- 3 floats: the hand's position from the camera, its axes, game units
local CAMERA = S + 0x30   -- the camera's transform (3x4)
local GUARDS = S + 0x34   -- per site, the guard_reg value the pose is for; 0 = not tracked
local CAVES = S + 0x50    -- the caves, one per site
-- 6 floats: the aim ray's origin and direction in the camera's axes, game units. At the end, so
-- that the caves of a savestate from version 1.0 (at CAVES too) stay where they were.
local RAY = S + 0x3E8
local LEVEL = S + 0x400     -- 1: from the camera's level frame (the VR view removes its pitch)
local ONE = S + 0x404       -- 1.0
local LEVEL_XF = S + 0x408  -- the camera's level frame (3x4), written by the caves
local MAX_SITES = 3
local AIM_LENGTH = 1.25   -- the length of the aim rays' direction (the games')

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- When the gun is tracked: rotation = the camera's rotation * BASIS, position = the camera's
-- position + its rotation * OFFSET (a ray: direction = the camera's rotation * RAY's direction,
-- origin = the camera's position + its rotation * RAY's origin), written where the game is about
-- to take them; then the replaced instruction.
--
-- With LEVEL set, the camera's level frame takes the camera's place: its right axis (horizontal,
-- as a pitch turns about it), the horizontal forward (up x right) and world up, at the camera's
-- position. The pose is then given in that frame. Only the registers the rest of the cave writes.
local function cave_words(g, site, index, address)
  local guard = GUARDS - S + (index - 1) * 4
  local base = site.base
  local origin = (site.direction and RAY or OFFSET) - S
  local t = {load_address("r11", S),
             string.format("lwz r12, 0x%X(r11)\ncmplw r12, %s\nbne done\n", guard, site.guard_reg),
             string.format("lwz r12, 0x%X(r11)\ncmpwi r12, 0\nlwz r12, 0x30(r11)\nbeq camera\n",
                           LEVEL - S),
             string.format("lfs f1, 0(r12)\nlfs f2, 0x10(r12)\nfneg f3, f2\nlfs f11, 0x%X(r11)\n",
                           ONE - S),
             "fsubs f10, f11, f11\n"}
  -- Rows of the level frame: (rx, -ry, 0), (ry, rx, 0), (0, 0, 1), then the camera's position.
  local rows = {{"f1", "f3", "f10"}, {"f2", "f1", "f10"}, {"f10", "f10", "f11"}}
  for row = 0, 2 do
    local o = row * 0x10
    for col = 0, 2 do
      t[#t + 1] = string.format("stfs %s, 0x%X(r11)\n", rows[row + 1][col + 1],
                                LEVEL_XF - S + o + col * 4)
    end
    t[#t + 1] = string.format("lfs f12, 0x%X(r12)\nstfs f12, 0x%X(r11)\n", o + 0xC,
                              LEVEL_XF - S + o + 0xC)
  end
  t[#t + 1] = string.format("addi r12, r11, 0x%X\ncamera:\n", LEVEL_XF - S)
  if site.direction then
    t[#t + 1] = string.format("lfs f1, 0x%X(r11)\nlfs f2, 0x%X(r11)\nlfs f3, 0x%X(r11)\n",
                              origin + 0xC, origin + 0x10, origin + 0x14)
    for row = 0, 2 do
      local o = row * 0x10
      t[#t + 1] = string.format(
        "lfs f10, 0x%X(r12)\nlfs f11, 0x%X(r12)\nlfs f12, 0x%X(r12)\n" ..
        "fmuls f13, f10, f1\nfmadds f13, f11, f2, f13\nfmadds f13, f12, f3, f13\n" ..
        "stfs f13, 0x%X(%s)\n", o, o + 4, o + 8, site.direction[row + 1], base)
    end
  else
    for i = 0, 8 do
      t[#t + 1] = string.format("lfs f%d, 0x%X(r11)\n", i + 1, i * 4)
    end
    for row = 0, 2 do
      local o = row * 0x10
      t[#t + 1] = string.format("lfs f10, 0x%X(r12)\nlfs f11, 0x%X(r12)\nlfs f12, 0x%X(r12)\n",
                                o, o + 4, o + 8)
      for col = 0, 2 do
        t[#t + 1] = string.format(
          "fmuls f13, f10, f%d\nfmadds f13, f11, f%d, f13\nfmadds f13, f12, f%d, f13\n" ..
          "stfs f13, 0x%X(%s)\n", col + 1, col + 4, col + 7, site.matrix + o + col * 4, base)
      end
    end
  end
  t[#t + 1] = string.format("lfs f1, 0x%X(r11)\nlfs f2, 0x%X(r11)\nlfs f3, 0x%X(r11)\n", origin,
                            origin + 4, origin + 8)
  for row = 0, 2 do
    local o = row * 0x10
    t[#t + 1] = string.format(
      "lfs f10, 0x%X(r12)\nlfs f11, 0x%X(r12)\nlfs f12, 0x%X(r12)\nlfs f13, 0x%X(r12)\n" ..
      "fmadds f13, f10, f1, f13\nfmadds f13, f11, f2, f13\nfmadds f13, f12, f3, f13\n" ..
      "stfs f13, 0x%X(%s)\n", o, o + 4, o + 8, o + 0xC, site.position[row + 1], base)
  end
  t[#t + 1] = "done:\n" .. site.replaced .. string.format("b `0x%X`\n", site.hook + 4)
  local words, err = asm.assemble(table.concat(t), address)
  if not words then error("assembling a cave for " .. g.name .. ": " .. err) end
  assert(address + #words * 4 <= RAY, "the caves do not fit in the scratch memory")
  return words
end

-- The caves of g, one after the other from CAVES: {{address, words}, ...} per site.
local function build_caves(g)
  assert(#g.sites <= MAX_SITES)
  local caves, address = {}, CAVES
  for i, site in ipairs(g.sites) do
    local words = cave_words(g, site, i, address)
    caves[i] = {address, words}
    address = address + #words * 4
  end
  return caves
end

-- ---- Finding the game, installing and removing the hooks ----

local current = nil       -- the game in GAMES whose hooks are installed
local caves = {}          -- the built caves per game
local waiting_logged = false
local warned = {}

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- Whether a site's code is in memory: the hook holds the original word or a branch to the
-- scratch memory, followed by the site's next instruction.
local function site_present(site)
  if mem.u32(site.hook + 4) ~= site.after then return false end
  local word = mem.u32(site.hook)
  if word == site.original then return true end
  local target = branch_target(site.hook, word or 0)
  return target ~= nil and target >= CAVES and target < S + mem.scratch_size
end

-- Whether g's executable is in memory (its first site).
local function present(g)
  return site_present(g.sites[1])
end

local function detect()
  for _, g in ipairs(GAMES) do
    if present(g) then return g end
  end
  return nil
end

local function clear_guards()
  for i = 1, MAX_SITES do mem.write_u32(GUARDS + (i - 1) * 4, 0) end
end

local function install()
  local g = detect()
  if g ~= current then clear_guards() end
  if not g then
    if current then log("no game of the trilogy is running") end
    current = nil
    if not waiting_logged then log("waiting for one of the three games") end
    waiting_logged = true
    return
  end
  caves[g] = caves[g] or build_caves(g)
  local changed = false
  for i, site in ipairs(g.sites) do
    local address, words = caves[g][i][1], caves[g][i][2]
    if i == 1 or site_present(site) then
      changed = mem.ensure_code(address, words) or changed
      changed = mem.ensure_code(site.hook, asm.branch(site.hook, address)) or changed
    elseif not warned[site] then
      warn(g.name .. ": unexpected code at " .. mem.hex(site.hook) .. ", site not installed")
      warned[site] = true
    end
  end
  if changed or g ~= current then
    log(g.name .. ": hooks installed, caves at", mem.hex(CAVES))
  end
  current = g
  waiting_logged = false
end

local function uninstall()
  clear_guards()
  local g = current
  current = nil
  if not g then return end
  for i, site in ipairs(g.sites) do
    if branch_target(site.hook, mem.u32(site.hook) or 0) == caves[g][i][1] then
      mem.write_code(site.hook, site.original)
    end
  end
end

-- ---- Tracking ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local sin, cos, rad, sqrt = math.sin, math.cos, math.rad, math.sqrt
local raw_pose, screen_hit, write_f32s = vr.raw_pose, vr.screen_hit, mem.write_f32s

-- The Hamilton product a * b of two quaternions (x, y, z, w).
local function qmul(ax, ay, az, aw, bx, by, bz, bw)
  return aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
         aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz
end

-- PrimedGun's smoothing of the hand pose: the basis (row-major) then the offset, in place. Each
-- field keeps settings.smoothing of the previous pose, unless the offset jumped by more than SNAP
-- game units.
local SNAP = 2.0
local target = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
local smoothed = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
local has_smoothed = false

local function smooth(keep)
  local t, s = target, smoothed
  local dx, dy, dz = t[10] - s[10], t[11] - s[11], t[12] - s[12]
  if not has_smoothed or keep <= 0 or dx * dx + dy * dy + dz * dz > SNAP * SNAP then
    for i = 1, 12 do s[i] = t[i] end
  else
    for i = 1, 12 do s[i] = s[i] * keep + t[i] * (1 - keep) end
  end
  has_smoothed = true
end

local tracking = false

local function stop_tracking()
  if tracking then
    clear_guards()
    has_smoothed = false
    tracking = false
  end
end

-- The controller's pose seen from the game camera, or from its level frame when the VR view
-- removes the camera's pitch (OpenXR view space: +X right, +Y up, -Z forward, meters), or the
-- test pose.
local function hand_pose(level)
  if settings.test_pitch ~= 0 then
    local h = rad(settings.test_pitch) * 0.5
    return true, 0.2, -0.25, -0.35, sin(h), 0, 0, cos(h)
  end
  return raw_pose(settings.hand, level and "level_view" or "view")
end

-- The virtual screen, in the game camera's view space (meters): 16:9, centred ScreenDistance
-- ahead, ScreenSize high. Read again every second (the VR menu changes them live).
local screen_half_height, screen_distance = 0.75, 2.0

local function read_screen()
  screen_half_height = (tonumber(vr.setting("ScreenSize")) or 1.5) * 0.5
  screen_distance = tonumber(vr.setting("ScreenDistance")) or 2.0
end

-- The ray from the eyes through the point where the hand's aim meets the virtual screen, where the
-- Wii pointer is and the game draws the scan lens (the pointer maps the screen 1:1 and stops at
-- its edges): origin then direction (AIM_LENGTH long), the camera's axes, game units. Nil when
-- the aim misses the screen.
local function lens_ray(upm)
  local u, v = screen_hit(settings.hand)
  local ok, ex, ey, ez = raw_pose("head", "view")
  if not u or not ok then return nil end
  u = u < -1 and -1 or (u > 1 and 1 or u)
  v = v < -1 and -1 or (v > 1 and 1 or v)
  local x = u * screen_half_height * 16 / 9 - ex
  local y = v * screen_half_height - ey
  local z = -screen_distance - ez
  local length = AIM_LENGTH / sqrt(x * x + y * y + z * z)
  -- From OpenXR's view axes (+X right, +Y up, -Z forward) to the camera's (+Y forward, +Z up).
  return ex * upm, -ez * upm, ey * upm, x * length, -z * length, y * length
end

-- A vector of the camera's axes in its level frame's: turned about the right axis by the camera's
-- pitch (the sine is its forward up component).
local function to_level(camera, y, z)
  local s = mem.f32(camera + 0x24) or 0
  local c = sqrt(math.max(0, 1 - s * s))
  return c * y - s * z, s * y + c * z
end

local function track(g)
  local level = vr.camera_pitch_source() ~= nil
  local ok, px, py, pz, qx, qy, qz, qw = hand_pose(level)
  local player = ok and g.player()
  local gun = player and g.gun(player)
  local camera = gun and g.camera()
  if not camera then
    stop_tracking()
    return
  end

  -- The rotation with the offsets (yaw, pitch, roll in the hand's axes), from OpenXR's view axes
  -- (+X right, +Y up, -Z forward) to the game camera's (+X right, +Y forward, +Z up).
  local h = rad(settings.yaw) * 0.5
  local x, y, z, w = qmul(qx, qy, qz, qw, 0, sin(h), 0, cos(h))
  h = rad(settings.pitch) * 0.5
  x, y, z, w = qmul(x, y, z, w, sin(h), 0, 0, cos(h))
  h = rad(settings.roll) * 0.5
  x, y, z, w = qmul(x, y, z, w, 0, 0, sin(h), cos(h))
  local t = target
  t[1], t[2], t[3] = 1 - 2 * (y * y + z * z), -2 * (x * z + y * w), 2 * (x * y - z * w)
  t[4], t[5], t[6] = -2 * (x * z - y * w), 1 - 2 * (x * x + y * y), -2 * (y * z + x * w)
  t[7], t[8], t[9] = 2 * (x * y + z * w), -2 * (y * z - x * w), 1 - 2 * (x * x + z * z)
  -- The position in the camera's axes, game units, with the calibration offsets (meters: x right,
  -- y up, z back).
  local scale = vr.units_per_meter() * settings.scale
  t[10] = (px + settings.offset_x) * scale
  t[11] = -(pz + settings.offset_z) * scale
  t[12] = (py + settings.offset_y) * scale
  smooth(settings.smoothing)

  local s = smoothed
  local b00, b01, b02, b10, b11, b12, b20, b21, b22 = s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                                                      s[8], s[9]
  -- The left hand mirrors the cannon.
  if settings.hand == 0 then b00, b10, b20 = -b00, -b10, -b20 end
  -- The cannon's origin: the hand, plus the model offset along the cannon.
  local mx, my, mz = settings.model_x * scale, (BASE_MODEL_FORWARD + settings.model_y) * scale,
                     settings.model_z * scale
  local ox, oy, oz = s[10] + b00 * mx + b01 * my + b02 * mz, s[11] + b10 * mx + b11 * my + b12 * mz,
                     s[12] + b20 * mx + b21 * my + b22 * mz
  -- The aim ray: along the cannon, or while scanning through the lens.
  local rx, ry, rz, dx, dy, dz
  if g.scanning and settings.test_pitch == 0 and g.scanning(player) then
    rx, ry, rz, dx, dy, dz = lens_ray(vr.units_per_meter())
    -- The lens is on the virtual screen, which follows the camera's pitch.
    if rx and level then
      ry, rz = to_level(camera, ry, rz)
      dy, dz = to_level(camera, dy, dz)
    end
  end
  if not rx then
    rx, ry, rz = ox, oy, oz
    dx, dy, dz = b01 * AIM_LENGTH, b11 * AIM_LENGTH, b21 * AIM_LENGTH
  end
  if not write_f32s(BASIS, b00, b01, b02, b10, b11, b12, b20, b21, b22) or
     not write_f32s(OFFSET, ox, oy, oz) or not write_f32s(RAY, rx, ry, rz, dx, dy, dz) or
     not mem.write_u32(CAMERA, camera) or not write_f32s(ONE, 1) or
     not mem.write_u32(LEVEL, level and 1 or 0) then
    stop_tracking()
    return
  end
  -- Last: arms the caves for this player and gun.
  for i, site in ipairs(g.sites) do
    mem.write_u32(GUARDS + (i - 1) * 4, site.guard(player, gun))
  end
  tracking = true
end

-- ---- Hooks ----

function on_load()
  clear_guards()
  read_screen()
  install()
end

function on_state_loaded()
  clear_guards()
  tracking = false
  has_smoothed = false
  current = nil
  install()
end

function on_frame()
  -- Another game of the disc may have started: its executable replaced the hooked one.
  if game.field() % 60 == 0 then read_screen() end
  if game.field() % 60 == 0 or not current or not present(current) then install() end
  if not current then
    stop_tracking()
    return
  end
  track(current)
end

function on_unload()
  uninstall()
end
