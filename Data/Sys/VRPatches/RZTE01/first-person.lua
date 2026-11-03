-- Wii Sports Resort (NTSC-U, Rev 1): first-person view from your Mii's head, in every sport.
--
-- With Camera Anchor on (VR settings, or its hotkey), the VR camera sits in the head of your Mii:
-- the Mii nearest the game camera in front of it, which every sport keeps behind you (Swordplay
-- and Table Tennis: you, not the opponent facing you). The view is level and turns with the Mii's
-- body, so the game camera's moves no longer move it, and the Mii's head is hidden while the view
-- is inside it.
--
-- Memory (NTSC-U Rev 1, found with the agent server's Workflow 7). The scene is NW4R g3d, as in
-- Wii Sports, but its scene objects are 0x30 bytes longer:
-- - [[[806F4DE8]] + 0x98]: the running sport's g3d scene root (vtable 806B3F20). Root + 0x6C: its
--   view matrix (world -> view of the game camera). Root + 0xDC: every object of the scene,
--   nested ones included (+ 0xE4 their number); the root's own group (+ 0xE8) misses Golf's Mii.
-- - A Mii's head (vtable 806B94E8, the Mii's face model): + 0x0C its world transform, at the neck
--   (+X up, +Y where the Mii looks; the face is 1.5 above, the hair up to 5), + 0x3C the same
--   scaled to the face model (0.08 for a Mii at full size; Bowling draws them at 0.7). Bowling's
--   lane reflects each Mii with a head of its own, whose +X points down.
-- - A Mii's body (vtable 806B3FB8, an MDL0 named "WS2_ava" and a capital letter: A1 to A10, C2...;
--   the "WS2_ava_hand..." models are hands): + 0x0C its base transform, at its feet (+Z its
--   facing), + 0xE8 its MDL0 (version 11: name at MDL0 + [MDL0 + 0x48]).
-- - Root + 0xF8: the data of the scene's camera 0, the one every sport draws with; + 0xB4 its
--   near plane, which g3d::Camera::SetPerspective (80188E60) stores every frame. Table Tennis puts
--   it 10 units away (Archery 3, the others 1 or less): 1.4 m from your eyes, which cut the
--   table's near edge away. The game's draws get their projections from several copies of it, so
--   the patch lowers it where GX sends a projection to the GPU (below).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "First-person view",
  author = "DolphinXR",
  version = "1.0",
  description = "With Camera Anchor on, the VR camera is in your Mii's head: level, turning with "
    .. "the Mii, whatever the game's camera does, its head hidden. For Wii Sports Resort NTSC-U "
    .. "(Rev 1).",
  games = {"RZTE01"},
  revision = 1,
  -- The heading frame the patch computes, the near plane cave and its data.
  scratch = 0x100,
  settings = {
    {name = "rotation", label = "View rotation", type = "choice",
     options = {"Level", "Fixed", "Full", "Camera", "Yaw"}, default = 0,
     description = "Level: turns with the Mii, the game's camera does not move the view. Fixed: "
       .. "the headset does all the turning. Full: the head's tilt too. Camera and Yaw: the "
       .. "game's camera moves the view."},
    {name = "heading", label = "Turn with", type = "choice",
     options = {"The Mii's body", "The Mii's head"}, default = 0},
    {name = "yaw", label = "Yaw offset", unit = "deg", default = 0, min = -180, max = 180,
     step = 1},
    {name = "up", label = "Up", unit = "m", default = 0, min = -0.5, max = 0.5, step = 0.01},
    {name = "forward", label = "Forward", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "eye_height", label = "Eye height", unit = "m", default = 1.6, min = 0.5, max = 2.5,
     step = 0.05,
     description = "Sets the world's scale from your Mii's size: its eyes at this height."},
    {name = "smoothing", label = "Smoothing (-1: the Camera Anchor setting)", default = -1,
     min = -1, max = 0.95, step = 0.05},
    {name = "hide_radius", label = "Hide the head within", unit = "game units", default = 5,
     min = 0, max = 10, step = 0.1,
     description = "The draws centred this close to the head (and above the next setting) are "
       .. "hidden. For a Mii at full size."},
    {name = "hide_below", label = "Down to", unit = "game units", default = 1, min = 0,
     max = 5, step = 0.1,
     description = "How far under the head's pivot a hidden draw's centre can be."},
    {name = "near", label = "Near plane", unit = "game units", default = 1, min = 0.1, max = 10,
     step = 0.1,
     description = "While the view is in the head, the game's camera cuts nothing nearer than "
       .. "this (Table Tennis cuts at 10 units: 1.4 m, the table's near edge)."},
  },
}

local SCENES = 0x806F4DE8
local ROOT_VTABLE = 0x806B3F20
local HEAD_VTABLE = 0x806B94E8
local MODEL_VTABLE = 0x806B3FB8
local MDL0 = 0x4D444C30
-- "WS2_", "ava"
local WS2, AVA = 0x5753325F, 0x617661
local FACE_SCALE = 0.08
-- For a Mii at full size, standing: its eyes above its feet, and above and in front of the head's
-- pivot (at the neck, between the shoulders).
local MII_EYES = 11.2
local EYES_UP, EYES_FORWARD = 2, 1
local ROTATIONS = {"level", "fixed", "full", "camera", "yaw"}

-- The options for a Mii drawn at `scale`; head_axes: the heading matrix is the head's own (+Y
-- forward, +X up), else the patch's heading frame or the body's base (+Z forward, +Y up).
local applied_scale, applied_head_axes
local function apply_options(scale, head_axes)
  applied_scale, applied_head_axes = scale, head_axes
  local upm = MII_EYES * scale / settings.eye_height
  vr.set_camera_anchor_options{
    rotation = ROTATIONS[settings.rotation + 1], yaw = settings.yaw,
    up = settings.up + EYES_UP * scale / upm,
    forward = settings.forward + EYES_FORWARD * scale / upm,
    upm = upm,
    smoothing = settings.smoothing >= 0 and settings.smoothing or nil,
    hide_radius = settings.hide_radius * scale, hide_below = settings.hide_below * scale,
    up_axis = "y", forward_axis = head_axes and "y" or "z", bone_up_axis = head_axes and "x" or "y",
  }
end

-- ---- The near plane: a cave where GX sends the projection to the GPU ----

-- The game sets its projections with GXSetProjection and, from arrays it keeps, GXSetProjectionv
-- (56 per frame in Table Tennis, some with a slightly farther near plane for decals). Both only
-- fill GX's state; __GXSetProjection (80039990) sends it. Hooked: its load of the depth row
-- (psq_l p0, 0x53C(r3)); p1 and p2 hold the rest of the projection there, so the cave keeps
-- f1 and f2. GX's state itself is not changed.
local HOOK = 0x800399B8
local HOOK_ORIGINAL = 0xE003053C
local SIGNATURE = {{0x80039990, 0x3C80CC01}, {0x800399B0, 0xE043052C},
                   {0x800399B4, 0xE0230534}, {0x800399BC, 0xF0450000}}

local S = mem.scratch
local HEADING = S + 0x00          -- the heading frame (12 floats)
local ARMED = S + 0x30            -- not 0: the cave lowers the game camera's near plane
local NEAR = S + 0x34             -- the near plane it lowers it to
local GAME_NEAR = S + 0x38        -- the game camera's near plane
local CONSTANTS = S + 0x3C        -- 1, and the near planes it changes: 0.98 to 1.05 times GAME_NEAR
local ROW = S + 0x48              -- the new depth row
local CAVE = S + 0x50

-- The depth row of a GX perspective projection is (-n / (f - n), -f n / (f - n)): n = P23 /
-- (P22 - 1) and f = P23 / P22. A projection whose near plane is the game camera's (r times it)
-- gets r times NEAR, the same far plane.
local function cave_words()
  local hi = (S >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  local text = "lwz r0, 0x528(r3)\ncmpwi r0, 0\nbne original" ..
               string.format("\nlis r12, %d\nori r12, r12, 0x%X", hi, S & 0xFFFF) ..
               string.format("\nlwz r0, 0x%X(r12)\ncmpwi r0, 0\nbeq original", ARMED - S) ..
               "\nlfs f3, 0x53C(r3)\nlfs f4, 0x540(r3)" ..
               string.format("\nlfs f5, 0x%X(r12)\nfsubs f6, f3, f5\nfdivs f6, f4, f6",
                             CONSTANTS - S) ..
               string.format("\nlfs f7, 0x%X(r12)\nfdivs f8, f6, f7", GAME_NEAR - S) ..
               string.format("\nlfs f9, 0x%X(r12)\nfcmpo cr0, f8, f9\nblt original",
                             CONSTANTS + 4 - S) ..
               string.format("\nlfs f9, 0x%X(r12)\nfcmpo cr0, f8, f9\nbgt original",
                             CONSTANTS + 8 - S) ..
               string.format("\nlfs f9, 0x%X(r12)\nfmuls f8, f8, f9", NEAR - S) ..
               "\nfdivs f10, f4, f3\nfsubs f11, f10, f8\nfdivs f12, f8, f11\nfneg f12, f12" ..
               "\nfmuls f11, f10, f12" ..
               string.format("\nstfs f12, 0x%X(r12)\nstfs f11, 0x%X(r12)", ROW - S, ROW + 4 - S) ..
               string.format("\npsq_l f0, 0x%X(r12), 0, 0\nb `0x%X`", ROW - S, HOOK + 4) ..
               string.format("\noriginal:\npsq_l f0, 0x53C(r3), 0, 0\nb `0x%X`\n", HOOK + 4)
  local words, err = asm.assemble(text, CAVE)
  if not words then error("assembling the cave: " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

local cave, installed = nil, false

local function our_branch(word)
  if not word or word & 0xFC000003 ~= 0x48000000 then
    return false
  end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (HOOK + offset) & 0xFFFFFFFF == CAVE
end

local function install()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then
      installed = false
      return
    end
  end
  local word = mem.u32(HOOK)
  if word ~= HOOK_ORIGINAL and not our_branch(word) then
    installed = false
    return
  end
  cave = cave or cave_words()
  local changed = mem.ensure_code(CAVE, cave)
  changed = mem.ensure_code(HOOK, asm.branch(HOOK, CAVE)) or changed
  if changed or not installed then log("near plane hook installed, cave at", mem.hex(CAVE)) end
  installed = true
end

function on_load()
  mem.write_u32(ARMED, 0)
  mem.write_f32s(CONSTANTS, 1, 0.98, 1.05)
  apply_options(1, false)
  install()
end

function on_state_loaded()
  mem.write_u32(ARMED, 0)
  mem.write_f32s(CONSTANTS, 1, 0.98, 1.05)
  installed = false
  install()
end

function on_unload()
  mem.write_u32(ARMED, 0)
  if our_branch(mem.u32(HOOK)) then mem.write_code(HOOK, HOOK_ORIGINAL) end
end

function on_setting_changed() apply_options(applied_scale or 1, applied_head_axes or false) end

-- Whether `m` is a Mii's body: "WS2_ava" and a capital letter.
local function is_body(m)
  if not mem.valid(m, 0x100) or mem.u32(m) ~= MODEL_VTABLE then
    return false
  end
  local mdl = mem.u32(m + 0xE8)
  if not mdl or not mem.valid(mdl, 0x4C) or mem.u32(mdl) ~= MDL0 then
    return false
  end
  local name = mdl + (mem.s32(mdl + 0x48) or 0)
  if not mem.valid(name, 8) or mem.u32(name) ~= WS2 then
    return false
  end
  local word = mem.u32(name + 4)
  local letter = word & 0xFF
  return word >> 8 == AVA and letter >= 0x41 and letter <= 0x5A
end

-- The scene's Mii heads and bodies, found again when the scene changes and every 5 s.
local heads, bodies, root, scan_field = {}, {}, nil, -1000

local function scan(r)
  heads, bodies = {}, {}
  if not r or not mem.valid(r, 0xF0) or mem.u32(r) ~= ROOT_VTABLE then
    return
  end
  local list, count = mem.u32(r + 0xDC), mem.u32(r + 0xE4)
  if not list or not count or count == 0 or count > 2048 or not mem.valid(list, count * 4) then
    return
  end
  for _, m in ipairs(mem.read_words(list, count)) do
    if mem.valid(m, 0x6C) then
      local vtable = mem.u32(m)
      if vtable == HEAD_VTABLE then
        heads[#heads + 1] = m
      elseif vtable == MODEL_VTABLE and is_body(m) then
        bodies[#bodies + 1] = m
      end
    end
  end
end

-- A head's pivot, or nil for a reflection or an unset transform.
local function head_position(m)
  local _, _, _, x, d, _, _, y, _, _, _, z = mem.f32s(m + 0x0C, 12)
  if not z or d <= 0 then
    return nil
  end
  return x, y, z
end

-- The size a head is drawn at (1 for a Mii at full size), rounded so that no pose changes it.
local function head_scale(m)
  local a, b, c, _, d, e, f, _, g, h, i = mem.f32s(m + 0x3C, 11)
  if not i then
    return 1
  end
  local det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
  local scale = math.abs(det) ^ (1 / 3) / FACE_SCALE
  return math.max(0.05, math.floor(scale * 20 + 0.5) / 20)
end

-- The body standing under the head at (x, y, z), or nil (the base sits a head's height below).
local function body_under(x, y, z, scale)
  local best, best_d
  for _, b in ipairs(bodies) do
    local bx = mem.f32(b + 0x18)
    local by = mem.f32(b + 0x28)
    local bz = mem.f32(b + 0x38)
    if bz then
      local d = (bx - x) ^ 2 + (by - y) ^ 2 + (bz - z) ^ 2
      if d < (15 * scale) ^ 2 and (not best_d or d < best_d) then
        best, best_d = b, d
      end
    end
  end
  return best
end

-- A level frame facing where the transform at `m` faces, from its sideways axis (column `side`:
-- X of a body's base, Z of a head), which stays level when the Mii leans, looks down or falls face
-- down: its forward axis then points at the ground, and its shadow on the ground turns wildly with
-- the slightest tilt (skydiving). Written to the patch's scratch memory; its address.
local heading_x, heading_z = 0, 1
local function level_heading(m, side)
  local r00, _, r02, x, _, _, _, y, r20, _, r22, z = mem.f32s(m, 12)
  if not z then
    return nil
  end
  local lx = side == 0 and r00 or r02
  local lz = side == 0 and r20 or r22
  -- Forward = side x up: (-lz, 0, lx). A side axis standing up (the Mii on its side) keeps the
  -- last heading.
  local n = math.sqrt(lx * lx + lz * lz)
  if n > 0.3 then
    heading_x, heading_z = -lz / n, lx / n
  end
  -- Columns: side (up x forward), up, forward.
  mem.write_f32s(HEADING, heading_z, 0, heading_x, x, 0, 1, 0, y, -heading_x, 0, heading_z, z)
  return HEADING
end

local current

-- This field's Mii: its head, its body and its size, or nil.
local function find_mii()
  local field = game.field()
  local r = mem.ptr(SCENES, 0, 0x98)
  if r ~= root or field - scan_field > 300 or (current and mem.u32(current) ~= HEAD_VTABLE) then
    scan(r)
    root, scan_field, current = r, field, nil
  end
  if #heads == 0 then
    return nil
  end
  -- The camera's position in the world: -R^T t of its view matrix.
  local r00, r01, r02, tx, r10, r11, r12, ty, r20, r21, r22, tz = mem.f32s(root + 0x6C, 12)
  if not tz then
    return nil
  end
  local cx = -(r00 * tx + r10 * ty + r20 * tz)
  local cy = -(r01 * tx + r11 * ty + r21 * tz)
  local cz = -(r02 * tx + r12 * ty + r22 * tz)
  -- The Mii nearest the game camera in front of it; the current one keeps it until another is
  -- clearly nearer.
  local best, best_d, bx, by, bz
  for _, m in ipairs(heads) do
    local x, y, z = head_position(m)
    if x and r20 * x + r21 * y + r22 * z + tz < 0 then
      local d = (x - cx) ^ 2 + (y - cy) ^ 2 + (z - cz) ^ 2
      if m == current then
        d = d * 0.6
      end
      if not best_d or d < best_d then
        best, best_d, bx, by, bz = m, d, x, y, z
      end
    end
  end
  current = best
  if not best then
    return nil
  end
  local scale = head_scale(best)
  return best, settings.heading == 0 and body_under(bx, by, bz, scale) or nil, scale
end

function on_frame()
  if game.field() % 60 == 0 or not installed then
    install()
  end
  local head, body, scale = find_mii()
  local heading
  if head then
    -- Full keeps the whole orientation of the body or the head; the others turn with the level
    -- frame.
    local head_axes = settings.rotation == 2 and not body
    if settings.rotation == 2 then
      heading = body and body + 0x0C or head + 0x0C
    else
      heading = body and level_heading(body + 0x0C, 0) or level_heading(head + 0x0C, 2)
    end
    if scale ~= applied_scale or head_axes ~= applied_head_axes then
      apply_options(scale, head_axes)
    end
  end
  local anchored = heading and
                   vr.set_camera_anchor_matrices(head + 0x0C, root + 0x6C, "view", heading)
  -- The near plane of the game camera, while the view is in the head and the game's is farther.
  -- Last: the cave reads the near planes once it is armed.
  local near = anchored and vr.camera_anchor_active() and settings.near * scale
  local game_near = near and mem.f32(root + 0xF8 + 0xB4)
  if game_near and near < game_near then
    mem.write_f32s(NEAR, near, game_near)
    mem.write_u32(ARMED, 1)
  else
    mem.write_u32(ARMED, 0)
  end
end
