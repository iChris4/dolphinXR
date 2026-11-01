-- Wii Sports (NTSC-U): first-person view from your Mii's head, in every sport.
--
-- With Camera Anchor on (VR settings, or its hotkey), the VR camera sits in the head of your Mii:
-- the Mii nearest the game camera in front of it, which every sport keeps behind you (Tennis: your
-- Mii at the back of the court; Baseball: the batter, not the catcher behind the camera). The
-- view is level and turns with the Mii's body, so the game camera's moves no longer move it, and
-- the Mii's head is hidden while the view is inside it.
--
-- Memory (NTSC-U, found with the agent server's Workflow 7):
-- - [[804DFB90]]: the running sport's scene; its g3d scene root (vtable 80361260) lies in its
--   first 0x800 bytes (0xCC to 0xDC). Root + 0xB8: the group of its models (vtable 80361238):
--   + 4 the array of models, + 0x10 their number.
-- - A model (vtable 80361310): + 0x0C its base transform (the Mii's position and facing, +Z),
--   + 0xB8 its MDL0 resource (name at MDL0 + [MDL0 + 0x3C]), + 0x160 its joints' world matrices
--   (+ 0xBC points there), 0x30 bytes each. The Mii bodies are named "ava_body..." (A1 and B1
--   with 28 matrices, Boxing's C0 with 9; A0/B0 have none set). Joint 1 is the head (+Y where
--   the Mii looks, +X up).
-- - Scene root + 0x6C: the scene's view matrix (the root's own; world -> view of its camera).
--   803B5D74 is the current one, which Golf turns to a top-down view for its map last.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "First-person view",
  author = "DolphinXR",
  version = "1.1",
  description = "With Camera Anchor on, the VR camera is in your Mii's head: level, turning with "
    .. "the Mii, whatever the game's camera does, its head hidden. For Wii Sports NTSC-U.",
  games = {"RSPE01"},
  revision = 0,
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
     description = "The draws centred this close to the head joint (and above the next setting) "
       .. "are hidden: the face reaches 4 units in front of it. For a Mii at full size."},
    {name = "hide_below", label = "Down to", unit = "game units", default = 1, min = 0,
     max = 5, step = 0.1,
     description = "How far under the head joint a hidden draw's centre can be (the top of the "
       .. "body is 1.7 under it)."},
  },
}

local SCENE = 0x804DFB90
local ROOT_VTABLE = 0x80361260
local GROUP_VTABLE = 0x80361238
local MODEL_VTABLE = 0x80361310
local MDL0 = 0x4D444C30
-- "ava_" "body"
local AVA, BODY = 0x6176615F, 0x626F6479
local MATRICES = 0x160
local HEAD = MATRICES + 1 * 0x30
-- The head joint's height above the feet of a Mii at full size, standing.
local MII_HEIGHT = 8.7
local ROTATIONS = {"level", "fixed", "full", "camera", "yaw"}

-- The options for a Mii drawn at `scale` (Bowling draws them at 0.7).
local applied_scale
local function apply_options(scale)
  local body = settings.heading == 0
  applied_scale = scale
  vr.set_camera_anchor_options{
    rotation = ROTATIONS[settings.rotation + 1], yaw = settings.yaw,
    up = settings.up, forward = settings.forward,
    upm = MII_HEIGHT * scale / settings.eye_height,
    smoothing = settings.smoothing >= 0 and settings.smoothing or nil,
    hide_radius = settings.hide_radius * scale, hide_below = settings.hide_below * scale,
    up_axis = "y", forward_axis = body and "z" or "y", bone_up_axis = body and "y" or "x",
  }
end

function on_load() apply_options(1) end
function on_setting_changed() apply_options(applied_scale or 1) end

-- Whether `m` is a Mii body model.
local function is_mii(m)
  if not m or not mem.valid(m, HEAD + 0x30) or mem.u32(m) ~= MODEL_VTABLE or
     mem.u32(m + 0xBC) ~= m + MATRICES then
    return false
  end
  local mdl = mem.u32(m + 0xB8)
  if not mdl or not mem.valid(mdl, 0x40) or mem.u32(mdl) ~= MDL0 then
    return false
  end
  local name = mdl + (mem.s32(mdl + 0x3C) or 0)
  return mem.valid(name, 8) and mem.u32(name) == AVA and mem.u32(name + 4) == BODY
end

-- The scene's Mii bodies, found again when the scene changes and every 5 s.
local miis, scene, camera, scan_field = {}, nil, nil, -1000

local function scan(y)
  miis, camera = {}, nil
  if not y or not mem.valid(y, 0x800) then
    return
  end
  local root
  for offset = 0, 0x7FC, 4 do
    if mem.u32(y + offset) == ROOT_VTABLE then
      root = y + offset
      break
    end
  end
  camera = root and root + 0x6C
  local group = root and mem.u32(root + 0xB8)
  if not group or not mem.valid(group, 0x14) or mem.u32(group) ~= GROUP_VTABLE then
    return
  end
  local array, count = mem.u32(group + 4), mem.u32(group + 0x10)
  if not array or not count or count == 0 or count > 1024 or not mem.valid(array, count * 4) then
    return
  end
  for i = 0, count - 1 do
    local m = mem.u32(array + i * 4)
    if is_mii(m) then
      miis[#miis + 1] = m
    end
  end
end

-- The head joint's position and scale when it is a proper rotation (not unset, not a mirrored
-- reflection like Bowling's lane draws), or nil.
local function head_position(m)
  local a, b, c, x, d, e, f, y, g, h, i, z = mem.f32s(m + HEAD, 12)
  if not z then
    return nil
  end
  local det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
  if det < 0.01 then
    return nil
  end
  return x, y, z, det ^ (1 / 3)
end

local current
function on_frame()
  local field = game.field()
  local y = mem.ptr(SCENE, 0)
  if y ~= scene or field - scan_field > 300 or (current and not is_mii(current)) then
    scan(y)
    scene, scan_field, current = y, field, nil
  end
  if #miis == 0 or not camera then
    return
  end
  -- The camera's position in the world: -R^T t of its view matrix.
  local r00, r01, r02, tx, r10, r11, r12, ty, r20, r21, r22, tz = mem.f32s(camera, 12)
  if not tz then
    return
  end
  local cx = -(r00 * tx + r10 * ty + r20 * tz)
  local cy = -(r01 * tx + r11 * ty + r21 * tz)
  local cz = -(r02 * tx + r12 * ty + r22 * tz)
  -- The Mii nearest the game camera in front of it (Baseball's catcher crouches behind it); the
  -- current one keeps it until another is clearly nearer.
  local best, best_d
  for _, m in ipairs(miis) do
    local hx, hy, hz = head_position(m)
    if hx and r20 * hx + r21 * hy + r22 * hz + tz < 0 then
      local d = (hx - cx) ^ 2 + (hy - cy) ^ 2 + (hz - cz) ^ 2
      if m == current then
        d = d * 0.6
      end
      if not best_d or d < best_d then
        best, best_d = m, d
      end
    end
  end
  if best ~= current and best then
    -- A new Mii: the world's scale and the hidden region follow its size (rounded, so that a
    -- posture does not change it).
    local _, _, _, scale = head_position(best)
    scale = math.floor(scale * 20 + 0.5) / 20
    if scale ~= applied_scale then
      apply_options(scale)
    end
  end
  current = best
  if best then
    local heading = settings.heading == 0 and best + 0x0C or nil
    vr.set_camera_anchor_matrices(best + HEAD, camera, "view", heading)
  end
end
