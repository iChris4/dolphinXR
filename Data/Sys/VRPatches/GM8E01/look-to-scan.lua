-- Metroid Prime (NTSC-U 1.00 and 1.02, GameCube): look at an object to scan it.
--
-- A DolphinXR VR game patch, after PrimedGun's HMD scan targeting (PrimedGun, created by Nobbie):
-- in the scan visor the object the game locks on with L is the one you look at, not the one
-- nearest the centre of the game's screen, and the lock does not turn Samus or the camera towards
-- it, so it stays where you look while you scan.
--
-- Each field the patch casts a ray from the game camera along your head's direction and picks,
-- among the scannable objects the game lists near the player (CPlayer::x344_nearbyOrbitObjects),
-- the one nearest the ray within a cone that widens with the distance (PrimedGun's cone and score:
-- the object's position or its render bounds). Three hooks act while the scan visor is on, in
-- first person:
--   CPlayer::FindOrbitTargetId          its FindBestOrbitableObject call answers the picked object
--                                       (or none)
--   CPlayer::UpdateOrbitOrientation     returns at once, as in morph ball: Samus does not turn
--                                       towards the target
--   CFirstPersonCamera::UpdateTransform its switch on the orbit state reads 5 or more, which it
--                                       skips: the camera does not turn towards the target either
--                                       (PrimedGun's scan yaw lock), nor dips on a jump or a fall
-- The game scans the locked object as usual: L held, in range. While the patch runs, the scan
-- window's magnified view shows what is in front of your head (GazeRamCopies 1), where you look.
-- The caves take the last 0x50 bytes of scratch the game's other patches leave. On by default
-- (Sys GameSettingsVR/GM8E01.ini).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Look to scan",
  author = "DolphinXR, after PrimedGun (Nobbie)",
  version = "1.0",
  description = "In the scan visor, L locks on the object you look at, and the view does not turn "
    .. "towards it while you scan. For Metroid Prime NTSC-U 1.00 and 1.02 (GameCube).",
  games = {"GM8E01"},
  scratch = 0x50,
  settings = {
    {name = "cone", label = "Look cone", default = 4, min = 1, max = 10, step = 0.5,
     description = "How far from where you look an object can be picked (PrimedGun's targeting "
       .. "radius: wider picks objects further from the centre of your view)."},
    {name = "range", label = "Range", unit = "game units", default = 60, min = 20, max = 120,
     step = 5},
  },
}

-- ---- The game ----

local u32, f32s, ptr = mem.u32, mem.f32s, mem.ptr

-- Per disc revision (recognized by the code at the hooks): the state manager, the player's camera
-- and ball states, orbit state and nearby orbitable objects, an actor's render bounds, and the
-- hooks: CPlayer::FindOrbitTargetId (this r4, the TUniqueId returned through r3, its
-- FindBestOrbitableObject call at +0x20), CPlayer::UpdateOrbitOrientation (its morph ball test
-- at +0x14) and, in CFirstPersonCamera::UpdateTransform, the player's orbit state load (player
-- r30) before the switch turning the camera to the orbit point; a state above 4 branches to the
-- instruction after the switch.
local REVISIONS = {
  {name = "Metroid Prime 1.00", state_manager = 0x8045A1A8,
   camera = 0x2F4, ball = 0x2F8, orbit = 0x304, nearby = 0x344, bounds = 0x9C,
   find = 0x8017CC70, find_check = 0x38A40354,          -- addi r5, r4, 0x354
   orient = 0x8017EACC,
   camera_site = 0x8000E71C, camera_skip = 0x8000E9C4},
  {name = "Metroid Prime 1.02", state_manager = 0x8045B208,
   camera = 0x304, ball = 0x308, orbit = 0x314, nearby = 0x354, bounds = 0x9C,
   find = 0x8017D454, find_check = 0x38A40364,
   orient = 0x8017F2B0,
   camera_site = 0x8000E9D8, camera_skip = 0x8000EC80},
}
local FIND_PROLOGUE = 0x9421FFF0     -- stwu r1, -0x10(r1)
local ORIENT_PROLOGUE = 0x9421FF80   -- stwu r1, -0x80(r1)
local MFLR = 0x7C0802A6              -- mflr r0
local FIND_CALL = 0x48000325         -- bl FindBestOrbitableObject (+0x324, both revisions)
local COMPARE_ZERO = 0x2C000000      -- cmpwi r0, 0
local CAMERA_AFTER = 0x2C000004      -- cmpwi r0, 4
local SKIP_WORD = 0x3861044C         -- addi r3, r1, 0x44C

-- The state manager's player, pending transition (pause screens), player state (behind a
-- reference), object list and camera manager.
local SM_PLAYER, SM_TRANSITION, SM_PLAYER_STATE = 0x84C, 0xF90, 0x8B8
local SM_OBJECTS, SM_CAMERAS = 0x810, 0x86C
local VISOR, SCAN_VISOR = 0x14, 2
local TRANSFORM = 0x34

-- ---- Scratch memory and code ----

local S = mem.scratch
local TARGET = S + 0x00   -- the picked TUniqueId (halfword), 0xFFFF none
-- While the patch drives the scan visor: 5, an orbit state the camera's switch skips (ORed into
-- the player's), and not 0 for the morph ball test; 0 = the game's own targeting and turning.
local ACTIVE = S + 0x04
local ACTIVE_VALUE = 5
local CODE = S + 0x08

-- lis's and the displacement's halves of an address (@ha, @l).
local function high_adjusted(address)
  local hi = ((address + 0x8000) >> 16) & 0xFFFF
  return hi >= 0x8000 and hi - 0x10000 or hi
end

local function low(address)
  local lo = address & 0xFFFF
  return lo >= 0x8000 and lo - 0x10000 or lo
end

-- The three caves, one after the other: their addresses and words.
local function build(r)
  local load = string.format("lis r11, %d\nlwz r12, %d(r11)\n", high_adjusted(S), low(ACTIVE))
  local find_best = r.find + 0x20 + 0x324
  local text = load .. "cmpwi r12, 0\nbeq find_game\n" ..
    string.format("lhz r12, %d(r11)\nsth r12, 0(r3)\nblr\nfind_game:\nb `0x%X`\n", low(TARGET),
                  find_best)
  local find_words, err = asm.assemble(text, CODE)
  if not find_words then error("assembling the target cave: " .. err) end
  local orient = CODE + #find_words * 4
  text = load .. string.format("lwz r0, 0x%X(r3)\nor r0, r0, r12\nb `0x%X`\n", r.ball,
                               r.orient + 0x18)
  local orient_words
  orient_words, err = asm.assemble(text, orient)
  if not orient_words then error("assembling the orientation cave: " .. err) end
  local camera = orient + #orient_words * 4
  text = load .. string.format("lwz r0, 0x%X(r30)\nor r0, r0, r12\nb `0x%X`\n", r.orbit,
                               r.camera_site + 4)
  local camera_words
  camera_words, err = asm.assemble(text, camera)
  if not camera_words then error("assembling the camera cave: " .. err) end
  assert(camera + #camera_words * 4 <= S + mem.scratch_size, "the caves do not fit the scratch")
  return {find = CODE, find_words = find_words, orient = orient, orient_words = orient_words,
          camera = camera, camera_words = camera_words}
end

-- The target of a relative branch (b or bl), or nil.
local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- Whether `site` holds `original` or a branch into this patch's code.
local function site_ours_or(site, original)
  local word = u32(site)
  if word == original then return true end
  local target = word and branch_target(site, word)
  return target ~= nil and target >= CODE and target < S + mem.scratch_size
end

-- The hooks' sites and the game's words there.
local function sites(r)
  return {{r.find + 0x20, FIND_CALL}, {r.orient + 0x14, 0x80030000 | r.ball},
          {r.camera_site, 0x801E0000 | r.orbit}}
end

local function detect()
  for _, r in ipairs(REVISIONS) do
    if u32(r.find) == FIND_PROLOGUE and u32(r.find + 4) == MFLR and
       u32(r.find + 0xC) == r.find_check and u32(r.orient) == ORIENT_PROLOGUE and
       u32(r.orient + 4) == MFLR and u32(r.orient + 0x18) == COMPARE_ZERO and
       u32(r.camera_site + 4) == CAMERA_AFTER and u32(r.camera_skip) == SKIP_WORD then
      local ours = true
      for _, site in ipairs(sites(r)) do ours = ours and site_ours_or(site[1], site[2]) end
      if ours then return r end
    end
  end
  return nil
end

local current = nil   -- the revision whose hooks are installed
local code = {}
local warned = false

local function install()
  local r = detect()
  if not r then
    -- Not while the game boots.
    if not warned and game.field() > 600 then
      warn("not installed: no code of Metroid Prime NTSC-U 1.00 or 1.02 at the hooks")
      warned = true
    end
    current = nil
    return
  end
  code[r] = code[r] or build(r)
  local c = code[r]
  local find_site, orient_site, camera_site = r.find + 0x20, r.orient + 0x14, r.camera_site
  local changed = mem.ensure_code(c.find, c.find_words)
  changed = mem.ensure_code(c.orient, c.orient_words) or changed
  changed = mem.ensure_code(c.camera, c.camera_words) or changed
  changed = mem.ensure_code(find_site, asm.branch(find_site, c.find, true)) or changed
  changed = mem.ensure_code(orient_site, asm.branch(orient_site, c.orient)) or changed
  changed = mem.ensure_code(camera_site, asm.branch(camera_site, c.camera)) or changed
  if changed then log(r.name .. ": hooks installed, code at", mem.hex(CODE)) end
  current = r
  warned = false
end

local function uninstall()
  local r = current
  current = nil
  mem.write_u32(ACTIVE, 0)
  if not r then return end
  for _, site in ipairs(sites(r)) do
    if branch_target(site[1], u32(site[1]) or 0) then mem.write_code(site[1], site[2]) end
  end
end

-- ---- Picking what you look at ----

local sqrt, abs, min, max = math.sqrt, math.abs, math.min, math.max

-- The object of `uid` through the state manager's object list, or nil.
local function object(objects, uid)
  local obj = u32(objects + ((uid & 0x3FF) << 3) + 4)
  if not obj or obj < 0x80000000 or obj >= 0x81800000 then return nil end
  local word = u32(obj + 8)
  if not word or word >> 16 ~= uid then return nil end
  return obj
end

-- The ray: from the game camera along the head's direction (both in the camera's view, OpenXR
-- axes: +X right, +Y up, -Z forward), in game coordinates. Nil without a live camera.
local function gaze_ray(sm, objects)
  local manager = u32(sm + SM_CAMERAS)
  local camera_word = manager and manager >= 0x80000000 and u32(manager)
  local camera = camera_word and object(objects, camera_word >> 16)
  if not camera then return nil end
  local a, b, c, tx, d, e, f, ty, g, h, i, tz = f32s(camera + TRANSFORM, 12)
  if not a or (a - a) + (b - b) + (c - c) + (d - d) + (e - e) + (f - f) + (g - g) + (h - h) +
     (i - i) + (tx - tx) + (ty - ty) + (tz - tz) ~= 0 then
    return nil
  end
  -- Prime's camera: columns right (a, d, g), forward (b, e, h), up (c, f, i).
  local ok, px, py, pz, qx, qy, qz, qw = vr.raw_pose("head", "view")
  if not ok then px, py, pz, qx, qy, qz, qw = 0, 0, 0, 0, 0, 0, 1 end
  -- The head's forward (-Z) turned by its rotation, then in the camera's axes.
  local vx = -2 * (qx * qz + qw * qy)
  local vy = -2 * (qy * qz - qw * qx)
  local vz = -(1 - 2 * (qx * qx + qy * qy))
  local right, forward, up = vx, -vz, vy
  local dx, dy, dz = a * right + b * forward + c * up, d * right + e * forward + f * up,
                     g * right + h * forward + i * up
  local length = sqrt(dx * dx + dy * dy + dz * dz)
  if length < 1e-4 then return nil end
  local upm = vr.units_per_meter()
  right, forward, up = px * upm, -pz * upm, py * upm
  return tx + a * right + b * forward + c * up, ty + d * right + e * forward + f * up,
         tz + g * right + h * forward + i * up, dx / length, dy / length, dz / length
end

-- PrimedGun's ray metrics of a point: the distance along the ray and from it, or nil.
local function point_metrics(ox, oy, oz, dx, dy, dz, x, y, z, max_along)
  local vx, vy, vz = x - ox, y - oy, z - oz
  local along = vx * dx + vy * dy + vz * dz
  if along < -0.5 or along > max_along then return nil end
  along = max(along, 0)
  local ex, ey, ez = vx - dx * along, vy - dy * along, vz - dz * along
  return along, sqrt(ex * ex + ey * ey + ez * ez), 0
end

-- The part [t0, t1] of the ray inside the slab lo..hi of one axis, or nil when it misses.
local function slab(o, dir, lo, hi, t0, t1)
  if abs(dir) < 1e-5 then
    if o < lo or o > hi then return nil end
    return t0, t1
  end
  local a, b = (lo - o) / dir, (hi - o) / dir
  if a > b then a, b = b, a end
  t0, t1 = max(t0, a), min(t1, b)
  if t0 > t1 then return nil end
  return t0, t1
end

-- PrimedGun's ray metrics of a box: inside the ray 0 from it, else the distance of its centre
-- less its radius across the ray; and that radius.
local function box_metrics(ox, oy, oz, dx, dy, dz, x0, y0, z0, x1, y1, z1, max_along)
  local hx, hy, hz = (x1 - x0) * 0.5, (y1 - y0) * 0.5, (z1 - z0) * 0.5
  local radius = sqrt(hx * hx * max(0, 1 - dx * dx) + hy * hy * max(0, 1 - dy * dy) +
                      hz * hz * max(0, 1 - dz * dz))
  -- Slab test of the ray against the box.
  local t0, t1 = slab(ox, dx, x0, x1, 0, max_along)
  if t0 then t0, t1 = slab(oy, dy, y0, y1, t0, t1) end
  if t0 then t0, t1 = slab(oz, dz, z0, z1, t0, t1) end
  if t0 then return t0, 0, radius end
  local cx, cy, cz = (x0 + x1) * 0.5, (y0 + y1) * 0.5, (z0 + z1) * 0.5
  local vx, vy, vz = cx - ox, cy - oy, cz - oz
  local along = vx * dx + vy * dy + vz * dz
  if along < -radius or along > max_along + radius then return nil end
  along = min(max(along, 0), max_along)
  local ex, ey, ez = vx - dx * along, vy - dy * along, vz - dz * along
  return along, max(0, sqrt(ex * ex + ey * ey + ez * ez) - radius), radius
end

-- The object's best metrics: its position, or its render bounds when they are a plausible object
-- shape (some scannable actors are large trigger volumes) around it.
local function metrics(obj, r, ox, oy, oz, dx, dy, dz, max_along)
  local _, _, _, x, _, _, _, y, _, _, _, z = f32s(obj + TRANSFORM, 12)
  if not x or (x - x) + (y - y) + (z - z) ~= 0 then return nil end
  local along, perp, radius = point_metrics(ox, oy, oz, dx, dy, dz, x, y, z, max_along)
  local x0, y0, z0, x1, y1, z1 = f32s(obj + r.bounds, 6)
  if x0 and (x0 - x0) + (y0 - y0) + (z0 - z0) + (x1 - x1) + (y1 - y1) + (z1 - z1) == 0 and
     x1 >= x0 and y1 >= y0 and z1 >= z0 and
     max(x1 - x0, y1 - y0, z1 - z0) <= min(max(settings.cone * 2, 8), 22) and
     x >= x0 - 1 and x <= x1 + 1 and y >= y0 - 1 and y <= y1 + 1 and z >= z0 - 1 and z <= z1 + 1 then
    local b_along, b_perp, b_radius = box_metrics(ox, oy, oz, dx, dy, dz, x0, y0, z0, x1, y1, z1,
                                                  max_along)
    if b_along and (not along or b_perp + b_along * 0.001 < perp + along * 0.001) then
      along, perp, radius = b_along, b_perp, b_radius
    end
  end
  return along, perp, radius
end

-- PrimedGun's scan cone (the largest distance from the ray, at that distance along it) and score
-- (lower is better).
local function cone(along, radius)
  local base = min(max(settings.cone * 0.45, 0.75), 2)
  local widest = min(max(settings.cone * 2.75, 3), 14)
  return min(base + min(max(along - 3, 0), 60) * 0.18 + min(radius, 3) * 0.35, widest)
end

local function score(along, perp, radius, limit)
  return perp / max(limit, 0.001) * 2 + perp * 0.2 + along * 0.003 + min(radius, 1) * 0.02
end

-- A picked object stays picked while it is in the cone and nearly as good as the best one.
local KEEP_MARGIN = 0.35
local picked = 0xFFFF
local picking = false   -- picked is this scan visor's

local function pick(r, sm, player)
  local objects = u32(sm + SM_OBJECTS)
  if not objects or objects < 0x80000000 then return 0xFFFF end
  local ox, oy, oz, dx, dy, dz = gaze_ray(sm, objects)
  if not ox then return 0xFFFF end
  local list = player + r.nearby
  local count, items = u32(list + 4), u32(list + 0xC)
  if not count or count == 0 or count > 128 or not items or items < 0x80000000 then return 0xFFFF end
  local best, best_score, kept_score = 0xFFFF, math.huge, nil
  for n = 0, count - 1 do
    local uid = mem.u16(items + n * 2)
    local obj = uid and uid ~= 0xFFFF and object(objects, uid)
    if obj and obj ~= player then
      local along, perp, radius = metrics(obj, r, ox, oy, oz, dx, dy, dz, settings.range)
      if along then
        local limit = cone(along, radius)
        if perp <= limit then
          local s = score(along, perp, radius, limit)
          if s < best_score then best, best_score = uid, s end
          if uid == picked then kept_score = s end
        end
      end
    end
  end
  if kept_score and kept_score <= best_score + KEEP_MARGIN then return picked end
  return best
end

-- The player in first person with the scan visor on (not morphed, no map or pause screen).
local function scanning_player(r)
  local sm = r.state_manager
  local player = ptr(sm + SM_PLAYER)
  if not player or u32(sm + SM_TRANSITION) ~= 0 then return nil end
  if u32(player + r.camera) ~= 0 or u32(player + r.ball) ~= 0 then return nil end
  local state = ptr(sm + SM_PLAYER_STATE, 0)
  if not state or u32(state + VISOR) ~= SCAN_VISOR then return nil end
  return player
end

-- ---- GazeRamCopies ----

-- The scan window's magnified view is a copy of the game screen's centre; with the camera no
-- longer turned to the target, the head's direction is where the target is.
local GAZE_HEAD = "1"
local saved_gaze = nil

local function set_gaze()
  if saved_gaze == nil then
    saved_gaze = vr.setting("GazeRamCopies") or "0"
    vr.set_setting("GazeRamCopies", GAZE_HEAD)
  end
end

local function restore_gaze()
  if saved_gaze ~= nil then
    vr.set_setting("GazeRamCopies", saved_gaze)
    saved_gaze = nil
  end
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(ACTIVE, 0)
  install()
  set_gaze()
end

function on_state_loaded()
  mem.write_u32(ACTIVE, 0)
  picked = 0xFFFF
  picking = false
  current = nil
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not current then install() end
  local player = current and scanning_player(current)
  if not player then
    mem.write_u32(ACTIVE, 0)
    picked = 0xFFFF
    picking = false
    return
  end
  -- Every other field: the game reads the target when L is pressed (half the cost on the Quest).
  if not picking or game.field() % 2 == 0 then
    picked = pick(current, current.state_manager, player)
    picking = true
  end
  mem.write_u16(TARGET, picked)
  -- Last: arms the caves.
  mem.write_u32(ACTIVE, ACTIVE_VALUE)
end

function on_unload()
  uninstall()
  restore_gaze()
end
