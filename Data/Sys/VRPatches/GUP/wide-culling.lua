-- Shadow the Hedgehog (NTSC-U) and Shadow the Hedgehog: Reloaded: the game culls for the
-- headset's view, not the game camera's.
--
-- The game draws the world and its objects through RenderWare, which culls them against its
-- camera's view frustum: six planes, eight corners and their bounding box, built from the game
-- camera. In VR the eyes see wider than the game camera and look around it, and in first person
-- they are at Shadow's eyes, in front of the camera: buildings and objects beside you and behind
-- you are missing. The patch hooks the frustum's construction and gives RenderWare a frustum at
-- the headset's eyes, turned with them, at least "Wide view angle" up and down and left and right.
-- The camera itself is unchanged (it still draws what the game shows).
--
-- Memory (Reloaded 1.2, found with the agent server):
-- - 807C6620 (in this run; [8056EE5C] points to it): the game camera's RwCamera. + 4 its RwFrame
--   (its world transform, the LTM, at + 0x50: right, up, at, position rows of 16 bytes; the
--   camera looks along at), + 0x68 the view window (tangents), + 0x80 the near and + 0x84 the far
--   plane (1 and 30000), + 0x94 the six frustum planes (RwFrustumPlane: an outward unit normal and
--   a distance, a sphere is out when normal . centre - distance > radius, then closestX/Y/Z: 1
--   when that component of the normal is not negative; far, near, right, top, left, bottom),
--   + 0x10C the frustum's bounding box (max, min), + 0x124 its eight corners (near then far).
-- - 8048528C updates a perspective camera's frustum: its call at 804852B4 to 804842C8 builds the
--   planes and corners from the frame's LTM (Reloaded widens them twice sideways, a cave at
--   80003F3C), then 8048349C the bounding box from the corners. It runs about 20 times a field
--   for the game camera, from the same transform.
-- - 80461658 (world sectors) and 804856C8 (spheres) test against the planes and the box.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Wide culling",
  author = "DolphinXR",
  version = "1.0",
  description = "The world and objects are culled for what the headset shows, around where you "
    .. "look and from where your eyes are (Shadow's eyes in first person), instead of the game "
    .. "camera's view: no more missing buildings and objects at the edges and behind you. For "
    .. "Shadow the Hedgehog NTSC-U and Reloaded.",
  games = {"GUPE8P", "GUPR8P"},
  scratch = 0x1D0,
  settings = {
    {name = "mode", label = "Culling", type = "choice", options = {"Wide view", "No culling"},
     default = 0,
     description = "Wide view: the game culls what is outside a wide view around where you "
       .. "look. No culling: it draws everything around you (more work for the GPU)."},
    {name = "fov", label = "Wide view angle", unit = "deg", default = 130, min = 90, max = 170,
     step = 5,
     description = "The angle kept up and down and left and right. Headsets show about 100 to "
       .. "125 degrees: a few more keep the edges while you turn your head."},
    {name = "follow_head", label = "Follow the head", type = "bool", default = true,
     description = "The wide view is at your eyes and turns with your head. Off: it stays on "
       .. "the game camera."},
  },
}

-- ---- The game ----

local HOOK = 0x804852B4
local HOOK_ORIGINAL = 0x4BFFF015  -- bl 804842C8
local BUILDER = 0x804842C8
local SIGNATURE = {{0x804852A4, 0x2C000001}, {0x804852AC, 0x4BFFF801}, {0x804852B0, 0x7FE3FB78},
                   {0x804852B8, 0x48000010}, {0x804852D4, 0x4BFFE1C9},
                   {0x804842C8, 0x9421FF70}, {0x8048431C, 0x80A30004}, {0x80484578, 0x907E0094}}
local PLANES_IN_CAMERA = 0x94
local CORNERS_IN_CAMERA = 0x124
local LTM_POSITION_IN_FRAME = 0x80
local CAMERAS = 0x8056EE14
local VIEW_IN_CAMERA = 0x3C
-- The farthest the frustum's apex goes behind the eyes: the eyes move between the field this
-- patch reads them and the frames the game culls with, and the near plane is 1 unit ahead.
local MARGIN = 30
local FAR = 30000
-- The game camera moves between fields: a camera this far from where it was is another view
-- (squared, game units).
local SAME_CAMERA = 100 * 100
local HUGE = 1e30

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local ARMED = S + 0x00      -- 1 while the routine replaces the frustum
local RETURN = S + 0x04     -- the caller's return address
local CAMERA = S + 0x08     -- the RwCamera of the call
local TOLERANCE = S + 0x0C  -- f32: SAME_CAMERA
local CAMERA_AT = S + 0x10  -- 3 f32: the game camera's position this field
local PLANES = S + 0x20     -- 6 RwFrustumPlanes of 20 bytes
local CORNERS = S + 0x98    -- 8 corners of 12 bytes
local ROUTINE = S + 0xF8

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- Called instead of the builder (r3 = the camera): calls it, then, while armed and when the
-- camera's frame is where the game camera is, copies this patch's planes and corners over its.
-- The caller only needs r31 afterwards.
local function routine_words()
  local o = function(address) return string.format("0x%X", address - S) end
  local text = load_address("r5", S) ..
    "mflr r0\nstw r0, " .. o(RETURN) .. "(r5)\nstw r3, " .. o(CAMERA) .. "(r5)\n" ..
    load_address("r12", BUILDER) .. "mtctr r12\nbctrl\n" ..
    load_address("r5", S) ..
    "lwz r0, " .. o(ARMED) .. "(r5)\ncmpwi r0, 0\nbeq done\n" ..
    "lwz r3, " .. o(CAMERA) .. "(r5)\nlwz r4, 4(r3)\ncmpwi r4, 0\nbeq done\n"
  for i = 0, 2 do
    text = text .. string.format("lfs f0, 0x%X(r4)\nlfs f1, %s(r5)\nfsubs f0, f0, f1\n",
                                 LTM_POSITION_IN_FRAME + i * 4, o(CAMERA_AT + i * 4)) ..
           (i == 0 and "fmuls f2, f0, f0\n" or "fmadds f2, f0, f0, f2\n")
  end
  text = text .. "lfs f1, " .. o(TOLERANCE) .. "(r5)\nfcmpo cr0, f2, f1\nbge done\n" ..
    string.format("addi r6, r5, %s\naddi r7, r3, 0x%X\nli r0, 30\nmtctr r0\n",
                  o(PLANES - 4), PLANES_IN_CAMERA - 4) ..
    "planes:\nlwzu r0, 4(r6)\nstwu r0, 4(r7)\nbdnz planes\n" ..
    string.format("addi r6, r5, %s\naddi r7, r3, 0x%X\nli r0, 24\nmtctr r0\n",
                  o(CORNERS - 4), CORNERS_IN_CAMERA - 4) ..
    "corners:\nlwzu r0, 4(r6)\nstwu r0, 4(r7)\nbdnz corners\n" ..
    "done:\nlwz r0, " .. o(RETURN) .. "(r5)\nmtlr r0\nblr\n"
  local words, err = asm.assemble(text, ROUTINE)
  if not words then error("assembling the routine: " .. err) end
  assert(ROUTINE + #words * 4 <= S + mem.scratch_size, "the routine does not fit")
  return words
end

local routine = nil
local warned = false
local armed = false

local function is_this_game()
  for _, word in ipairs(SIGNATURE) do
    if mem.u32(word[1]) ~= word[2] then
      return false
    end
  end
  return true
end

-- A bl from the hook into the scratch region (this patch's routine, maybe at another address in
-- an earlier run).
local function our_branch(word)
  if not word or word & 0xFC000003 ~= 0x48000001 then return false end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  local target = (HOOK + offset) & 0xFFFFFFFF
  return target >= 0x80001800 and target < 0x80003000
end

local function install()
  local word = mem.u32(HOOK)
  if word ~= HOOK_ORIGINAL and not our_branch(word) then
    return
  end
  routine = routine or routine_words()
  mem.write_u32(ARMED, 0)
  mem.write_f32(TOLERANCE, SAME_CAMERA)
  mem.ensure_code(ROUTINE, routine)
  if mem.ensure_code(HOOK, asm.branch(HOOK, ROUTINE, true)) then
    log("hooked the camera's frustum, routine at", mem.hex(ROUTINE))
  end
end

local function uninstall()
  mem.write_u32(ARMED, 0)
  if our_branch(mem.u32(HOOK)) then
    mem.write_code(HOOK, HOOK_ORIGINAL)
  end
end

function on_load()
  if is_this_game() then
    install()
  end
end

function on_state_loaded()
  if is_this_game() then
    install()
  end
end

function on_unload()
  uninstall()
end

-- ---- The frustum ----

-- 3x4 row-major matrices as 12 numbers.
local function mul(a, b)
  local m = {}
  for r = 0, 2 do
    local a0, a1, a2, a3 = a[r * 4 + 1], a[r * 4 + 2], a[r * 4 + 3], a[r * 4 + 4]
    for c = 1, 4 do
      m[r * 4 + c] = a0 * b[c] + a1 * b[4 + c] + a2 * b[8 + c] + (c == 4 and a3 or 0)
    end
  end
  return m
end

-- The inverse of a rotation and translation.
local function rigid_inverse(a)
  local i = {a[1], a[5], a[9], 0, a[2], a[6], a[10], 0, a[3], a[7], a[11], 0}
  for r = 0, 2 do
    i[r * 4 + 4] = -(i[r * 4 + 1] * a[4] + i[r * 4 + 2] * a[8] + i[r * 4 + 3] * a[12])
  end
  return i
end

-- The view matrix of the camera the game draws with (GameCube world -> view), or nil.
local function camera_view()
  local count = mem.u32(CAMERAS + 8)
  local list = mem.u32(CAMERAS + 0xC)
  if not count or count < 1 or count > 8 or not list or not mem.valid(list, count * 8) then
    return nil
  end
  for i = 0, count - 1 do
    local camera = mem.u32(list + i * 8)
    if camera and mem.valid(camera, VIEW_IN_CAMERA + 0x30) and mem.u8(camera) ~= 0 then
      local view = {mem.f32s(camera + VIEW_IN_CAMERA, 12)}
      local length = view[1] * view[1] + view[2] * view[2] + view[3] * view[3]
      if math.abs(length - 1) < 0.01 then
        return view
      end
    end
  end
  return nil
end

-- The eyes in the world (columns: right, up, back, position), from the headset's "view" pose (in
-- the game camera's view axes, meters) or the game camera.
local function eyes_in_world(to_world)
  if not settings.follow_head then
    return to_world
  end
  local valid, px, py, pz, qx, qy, qz, qw = vr.raw_pose("head", "view")
  if not valid then
    return to_world
  end
  local upm = vr.view_units_per_meter()
  return mul(to_world, {
    1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw), px * upm,
    2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw), py * upm,
    2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy), pz * upm,
  })
end

local function closest(v)
  return v >= 0 and 1 or 0
end

local function write_plane(index, nx, ny, nz, distance)
  local at = PLANES + index * 20
  mem.write_f32s(at, nx, ny, nz, distance)
  mem.write_u32(at + 16, closest(nx) << 24 | closest(ny) << 16 | closest(nz) << 8)
end

-- The planes and corners of a frustum at `eyes` (3x4, columns right, up, back, position) with
-- half angles of `half` degrees; with `everything`, planes that keep everything.
local function write_frustum(eyes, half, everything)
  local rx, ry, rz = eyes[1], eyes[5], eyes[9]
  local ux, uy, uz = eyes[2], eyes[6], eyes[10]
  local bx, by, bz = eyes[3], eyes[7], eyes[11]
  -- The apex, behind the eyes.
  local ax, ay, az = eyes[4] + bx * MARGIN, eyes[8] + by * MARGIN, eyes[12] + bz * MARGIN
  local c, s = math.cos(math.rad(half)), math.sin(math.rad(half))
  local planes = {
    {-bx, -by, -bz},                                            -- far
    {bx, by, bz},                                               -- near
    {c * rx + s * bx, c * ry + s * by, c * rz + s * bz},        -- right
    {c * ux + s * bx, c * uy + s * by, c * uz + s * bz},        -- top
    {-c * rx + s * bx, -c * ry + s * by, -c * rz + s * bz},     -- left
    {-c * ux + s * bx, -c * uy + s * by, -c * uz + s * bz},     -- bottom
  }
  for i, n in ipairs(planes) do
    local distance = n[1] * ax + n[2] * ay + n[3] * az
    if i == 1 then
      distance = distance + FAR
    end
    write_plane(i - 1, n[1], n[2], n[3], everything and HUGE or distance)
  end
  -- The corners: four at the apex (the near plane passes there), four on the far plane.
  local t = math.tan(math.rad(half)) * FAR
  local fx, fy, fz = ax - bx * FAR, ay - by * FAR, az - bz * FAR
  local corners = {}
  for _ = 1, 4 do
    corners[#corners + 1] = ax
    corners[#corners + 1] = ay
    corners[#corners + 1] = az
  end
  for _, sign in ipairs({{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}) do
    corners[#corners + 1] = fx + (rx * sign[1] + ux * sign[2]) * t
    corners[#corners + 1] = fy + (ry * sign[1] + uy * sign[2]) * t
    corners[#corners + 1] = fz + (rz * sign[1] + uz * sign[2]) * t
  end
  if everything then
    for i = 1, #corners do
      corners[i] = (i % 2 == 0) and 1e7 or -1e7
    end
  end
  mem.write_f32s(CORNERS, table.unpack(corners))
end

local function arm(on)
  if on ~= armed then
    log(on and "the frustum is at the headset's eyes" or "the game's own frustum")
    armed = on
  end
  mem.write_u32(ARMED, on and 1 or 0)
end

function on_frame()
  if not is_this_game() then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(HOOK) ..
           ": is this Shadow the Hedgehog NTSC-U or Reloaded?")
      warned = true
    end
    return
  end
  if game.field() % 60 == 0 then
    install()
  end
  local view = camera_view()
  if not view then
    arm(false)
    return
  end
  local to_world = rigid_inverse(view)
  mem.write_f32s(CAMERA_AT, to_world[4], to_world[8], to_world[12])
  write_frustum(eyes_in_world(to_world), settings.fov / 2, settings.mode == 1)
  arm(true)
end
