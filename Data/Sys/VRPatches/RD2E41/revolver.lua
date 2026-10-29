-- Red Steel 2 (NTSC-U): the revolver follows a VR controller.
--
-- The renderer draws each object from its render node (802983E8: the node's world rotation and
-- position, then 802982F8 multiplies them by the view and loads the result). The patch hooks the
-- branch from one to the other and, for the revolver's node only, gives the renderer the world
-- matrix that puts the revolver where the controller is seen from the game camera: the controller's
-- pose in the camera's view space times the inverse of the view, both read in the same frame, so it
-- does not lag when the camera turns. Only the drawing changes: the game still aims at the Wii
-- pointer, which DolphinXR moves with the controller's aim.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Revolver follows the controller",
  author = "DolphinXR",
  version = "1.0",
  description = "The revolver is drawn in your hand, where the VR controller is and pointing "
    .. "where it points. For Red Steel 2 NTSC-U.",
  games = {"RD2E41"},
  revision = 0,
  scratch = 0x400,
  settings = {
    {name = "hand", label = "Revolver hand", type = "choice", options = {"Left", "Right"},
     default = 1},
    {name = "scale", label = "Hand movement scale", default = 1, min = 0.5, max = 2, step = 0.05,
     description = "Times the profile's Units per Meter: above 1 the revolver moves farther "
       .. "than the hand."},
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
    {name = "hide_arms", label = "Hide the arms", type = "bool", default = true,
     description = "While the revolver is drawn, the game's arms are hidden: they stay where "
       .. "the game holds them, without the revolver."},
    {name = "smoothing", label = "Smoothing", default = 0, min = 0, max = 0.9, step = 0.01,
     description = "Part of the previous pose kept each frame."},
    {name = "test_pitch", label = "Test pose", unit = "deg", default = 0, min = -60, max = 60,
     step = 5,
     description = "Not 0: the revolver is held at this pitch in front of the camera instead of "
       .. "following the controller, to check the patch without a headset."},
  },
}

-- ---- The game (Red Steel 2 NTSC-U) ----

local RENDERER = 0x8054E178     -- [r13 - 0x6F48]: the renderer, its view (4x4, row vectors:
                                -- clip = p * V * P) at +0x210, the world matrix it draws an
                                -- object with at +0x2D0
local VIEW_OFFSET = 0x210
local NODE_CLASS = 0x804707D0   -- the vtable of a render node object, 0x28 before its matrix
-- The revolver's render nodes: its body, whose pose the controller gives, and its other parts
-- (cylinder, hammer...) before it, NODE_STRIDE apart. A node holds its world rotation rows (+0,
-- +0x10, +0x20) and position (+0x80): the matrix 802983E8 receives in r4. The player's weapons
-- are allocated at the same addresses in every level seen so far.
local REVOLVER_NODE = 0x91F6A4FC
local PARTS = 5
local NODE_STRIDE = 0x1B4
local NODE_POSITION = 0x80
local NODE_MODEL = 0x90
local MODEL_CLASS = 0x8046FF30  -- the vtable of the node's model object
local INVERSE = 0x800ECA1C      -- rigid inverse of a 4x4: r3 = inverse(r4)
local CONCAT = 0x800379C4       -- 4x4 multiplication: r3 = r4 * r5
local DRAW = 0x802982F8         -- multiplies the world matrix by the view and loads it
-- 802983E8 copies the node into the renderer's world matrix and ends with "b DRAW".
local HOOK = 0x80298438
local HOOK_ORIGINAL = 0x4BFFFEC0
local SIGNATURE = {{0x802983E8, 0xE0E40000}, {0x8029840C, 0xE0240080}, {0x80298418, 0xF0E302D0}}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local GUARDS = S + 0x00   -- the revolver's nodes when the pose is for them (PARTS words), the
                          -- body's first; 0 = not tracked
local DRAWN = S + 0x14    -- counts the revolver's parts drawn
local RETURN = S + 0x18   -- the caller's return address
local RENDERER_SAVE = S + 0x1C
local HAND = S + 0x20     -- 4x4: the body in the camera's view space (row vectors)
local BODY = S + 0x60     -- 4x4: the body's world matrix, as the game placed it
local INV_BODY = S + 0xA0 -- 4x4: its inverse
local INV_VIEW = S + 0xE0 -- 4x4: the inverse of the view
local MOVE = S + 0x120    -- 4x4: inverse(BODY) * HAND * inverse(view)
local RESULT = S + 0x160  -- 4x4: the part's world matrix * MOVE
local CAVE = S + 0x1A0

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- In place of "b DRAW" at the end of 802983E8 (r3 the renderer, r4 the node, already copied to
-- the renderer's world matrix at +0x2D0): for a node of the revolver, that matrix becomes
-- world * inverse(body) * HAND * inverse(view), which puts the body where the controller is and
-- keeps the other parts where they are on it. The return address is kept for DRAW, which returns
-- to 802983E8's caller.
local function cave_words()
  local function o(address) return address - S end
  local t = {load_address("r11", S)}
  for i = 0, PARTS - 1 do
    t[#t + 1] = string.format("lwz r12, 0x%X(r11)\ncmplw r4, r12\nbeq hit\n", o(GUARDS) + i * 4)
  end
  t[#t + 1] = string.format("b `0x%X`\nhit:\n", DRAW)
  t[#t + 1] = string.format("lwz r12, 0x%X(r11)\naddi r12, r12, 1\nstw r12, 0x%X(r11)\n",
                            o(DRAWN), o(DRAWN))
  t[#t + 1] = string.format("mflr r0\nstw r0, 0x%X(r11)\nstw r3, 0x%X(r11)\n", o(RETURN),
                            o(RENDERER_SAVE))
  -- BODY: the body node's rotation rows (+0 to +0x30) and position row (+0x80).
  t[#t + 1] = string.format("lwz r12, 0x%X(r11)\n", o(GUARDS))
  for i = 0, 15 do
    local from = i < 12 and i * 4 or NODE_POSITION + (i - 12) * 4
    t[#t + 1] = string.format("lwz r0, 0x%X(r12)\nstw r0, 0x%X(r11)\n", from, o(BODY) + i * 4)
  end
  local function call(fn, dest, a, b)
    local s = load_address("r11", S) .. string.format("addi r3, r11, 0x%X\n", o(dest))
    if type(a) == "string" then
      s = s .. a
    else
      s = s .. string.format("addi r4, r11, 0x%X\n", o(a))
    end
    if b then s = s .. string.format("addi r5, r11, 0x%X\n", o(b)) end
    return s .. string.format("bl `0x%X`\n", fn)
  end
  local renderer_plus = function(offset)
    return string.format("lwz r4, 0x%X(r11)\naddi r4, r4, 0x%X\n", o(RENDERER_SAVE), offset)
  end
  t[#t + 1] = call(INVERSE, INV_BODY, BODY)
  t[#t + 1] = call(INVERSE, INV_VIEW, renderer_plus(VIEW_OFFSET))
  t[#t + 1] = call(CONCAT, RESULT, INV_BODY, HAND)
  t[#t + 1] = call(CONCAT, MOVE, RESULT, INV_VIEW)
  t[#t + 1] = load_address("r11", S) .. string.format("addi r3, r11, 0x%X\n", o(RESULT)) ..
              renderer_plus(0x2D0) .. string.format("addi r5, r11, 0x%X\nbl `0x%X`\n", o(MOVE),
                                                    CONCAT)
  -- The renderer's world matrix = RESULT.
  t[#t + 1] = load_address("r11", S) .. string.format("lwz r12, 0x%X(r11)\n", o(RENDERER_SAVE))
  for i = 0, 15 do
    t[#t + 1] = string.format("lwz r0, 0x%X(r11)\nstw r0, 0x%X(r12)\n", o(RESULT) + i * 4,
                              0x2D0 + i * 4)
  end
  t[#t + 1] = string.format("lwz r0, 0x%X(r11)\nmtlr r0\nlwz r3, 0x%X(r11)\nb `0x%X`\n",
                            o(RETURN), o(RENDERER_SAVE), DRAW)
  local words, err = asm.assemble(table.concat(t), CAVE)
  if not words then error("assembling the cave: " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hook ----

local cave = nil
local installed = false
local waiting_logged = false

local function present()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then return false end
  end
  local word = mem.u32(HOOK)
  return word == HOOK_ORIGINAL or word == asm.branch(HOOK, CAVE)
end

local function install()
  if not present() then
    installed = false
    if not waiting_logged then log("waiting for the game's render code") end
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

local function disarm()
  for i = 0, PARTS - 1 do mem.write_u32(GUARDS + i * 4, 0) end
end

local function uninstall()
  disarm()
  if mem.u32(HOOK) == asm.branch(HOOK, CAVE) then mem.write_code(HOOK, HOOK_ORIGINAL) end
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

-- The pose kept between fields (rows of the rotation, then the position), smoothed in place:
-- each field keeps settings.smoothing of the previous pose, unless the position jumped by more
-- than SNAP game units.
local SNAP = 1.0
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

-- The game's arms are hidden through a flag of the game's VR profile (a Texture override of
-- their texture with condition=ARMS_FLAG) while the revolver is drawn: the cave counts its draws.
local ARMS_FLAG = "rd2-revolver-hide-arms"
local last_drawn = 0
local drawn_field = -1000
local arms_hidden = false

local function update_arms()
  local drawn = u32(DRAWN) or 0
  local field = game.field()
  if drawn ~= last_drawn then
    last_drawn = drawn
    drawn_field = field
  end
  local hide = tracking and settings.hide_arms and field - drawn_field <= 6
  if hide ~= arms_hidden then
    vr.set_flag(ARMS_FLAG, hide)
    arms_hidden = hide
  end
end

local function stop_tracking()
  if tracking then
    disarm()
    has_smoothed = false
    tracking = false
  end
end

-- The controller's pose seen from the game camera (OpenXR view space: +X right, +Y up,
-- -Z forward, meters), or the test pose.
local function hand_pose()
  if settings.test_pitch ~= 0 then
    local h = rad(settings.test_pitch) * 0.5
    return true, 0.2, -0.25, -0.35, sin(h), 0, 0, cos(h)
  end
  return raw_pose(settings.hand, "view")
end

local function render_node(node)
  if u32(node - 0x28) ~= NODE_CLASS then return false end
  local model = u32(node + NODE_MODEL)
  return model and model >= 0x80000000 and model < 0x81800000 and u32(model) == MODEL_CLASS
end

-- Whether the revolver's nodes are there: render nodes with a model, the body near the camera.
local function revolver_present()
  for i = 0, PARTS - 1 do
    if not render_node(REVOLVER_NODE - i * NODE_STRIDE) then return false end
  end
  local renderer = u32(RENDERER)
  if not renderer or renderer < 0x80000000 or renderer >= 0x81800000 then return false end
  -- The camera's position: -t * R^T of the view (rows r0, r1, r2, translation t).
  local r00, r01, r02, _, r10, r11, r12, _, r20, r21, r22, _, tx, ty, tz =
    f32s(renderer + VIEW_OFFSET, 15)
  local nx, ny, nz = f32s(REVOLVER_NODE + NODE_POSITION, 3)
  if not r00 or not nx then return false end
  local cx = -(tx * r00 + ty * r01 + tz * r02)
  local cy = -(tx * r10 + ty * r11 + tz * r12)
  local cz = -(tx * r20 + ty * r21 + tz * r22)
  local dx, dy, dz = nx - cx, ny - cy, nz - cz
  return dx * dx + dy * dy + dz * dz <= 4
end

local function track()
  local ok, px, py, pz, qx, qy, qz, qw = hand_pose()
  if not ok or not revolver_present() then
    stop_tracking()
    return
  end

  -- The rotation with the offsets (yaw, pitch, roll in the hand's axes).
  local h = rad(settings.yaw) * 0.5
  local x, y, z, w = qmul(qx, qy, qz, qw, 0, sin(h), 0, cos(h))
  h = rad(settings.pitch) * 0.5
  x, y, z, w = qmul(x, y, z, w, sin(h), 0, 0, cos(h))
  h = rad(settings.roll) * 0.5
  x, y, z, w = qmul(x, y, z, w, 0, 0, sin(h), cos(h))
  -- The hand's axes in OpenXR's view axes (the columns of its rotation), then in the game's
  -- view axes (+X right, +Y up, +Z forward: z negated). The revolver's model has its barrel
  -- along +X, its left along +Y and its top along +Z: its rows are the hand's forward (-Z),
  -- left (-X) and up (+Y).
  local rx, ry, rz = 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), -2 * (x * z - y * w)
  local ux, uy, uz = 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), -2 * (y * z + x * w)
  local fx, fy, fz = -2 * (x * z + y * w), -2 * (y * z - x * w), 1 - 2 * (x * x + y * y)
  local t = target
  t[1], t[2], t[3] = fx, fy, fz
  t[4], t[5], t[6] = -rx, -ry, -rz
  t[7], t[8], t[9] = ux, uy, uz
  -- The position in the game's view axes and units, with the calibration offsets (meters:
  -- x right, y up, z back).
  local scale = vr.units_per_meter() * settings.scale
  t[10] = (px + settings.offset_x) * scale
  t[11] = (py + settings.offset_y) * scale
  t[12] = -(pz + settings.offset_z) * scale
  smooth(settings.smoothing)

  local s = smoothed
  if not write_f32s(HAND, s[1], s[2], s[3], 0, s[4], s[5], s[6], 0, s[7], s[8], s[9], 0,
                    s[10], s[11], s[12], 1) then
    stop_tracking()
    return
  end
  -- Last: arms the cave for the revolver's nodes, the body first.
  for i = 0, PARTS - 1 do mem.write_u32(GUARDS + i * 4, REVOLVER_NODE - i * NODE_STRIDE) end
  tracking = true
end

-- ---- Hooks ----

function on_load()
  disarm()
  install()
end

function on_state_loaded()
  disarm()
  tracking = false
  has_smoothed = false
  installed = false
  install()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then install() end
  if not installed then
    stop_tracking()
  else
    track()
  end
  update_arms()
end

function on_unload()
  uninstall()
end
