-- The Legend of Zelda: The Wind Waker (NTSC-U): first-person view from Link's head.
--
-- With Camera Anchor on (VR settings, or its hotkey), the VR camera sits in Link's head: his head
-- joint, read from his model, composed with the game camera's view matrix. The view is level, so
-- the game's camera, which swings around him, no longer moves it. His head is hidden while the
-- view is inside it: its face and hair share their
-- texture with his body, his mouth changes texture with his expression and some of his hair is
-- skinned with weighted matrices, so the draws centred around his head joint are hidden: within
-- 40 units and above 5 units under it (his head's parts are 3 to 33 units away, all above the
-- joint; his body's centres are 12 units under it or lower). The eyes also by their textures.
-- The game moves Link relative to its camera, so the main stick is turned: by default (Stick
-- forward: VR) up and back walk where you look and left and right turn the view, as in VR games;
-- with "Link's facing" the view turns with Link and up walks where he faces. Not in menus and
-- dialogs, where the stick picks.
--
-- Memory (NTSC-U rev 0, found with the agent server's Workflow 7):
-- - 803CA74C (and 803C9DEC) in the game's global info point to Link's actor; actor + 0x32C is his
--   J3DModel (vtable 8039EA50), whose user area (+ 0x14) points back to the actor. His model data
--   has 42 joints (J3DModelData + 0x10 is the joint tree, its joint count at + 0x18).
-- - J3DModel + 0x24: the model's base transform, Link's position and facing (its +Z).
-- - J3DModel + 0x8C: the joints' world matrices, 0x30 bytes each (this J3D has no matrix buffer):
--   joint 15 is the head (its +Y points where Link faces, its +X up).
-- - 803EDA58: the camera's world -> view matrix, written once per field.
-- - 803CA80A (in the game's global info): 0 while Link moves freely, 1 with the pause menu open,
--   7 during a dialog.
-- - Around the head joint (the neck's top): the shoulders 13 units under it, the hair up to 37
--   above.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "First-person view",
  author = "DolphinXR",
  version = "1.2",
  description = "With Camera Anchor on, the VR camera is in Link's head: level, turning with Link "
    .. "whatever the game's camera does. For The Wind Waker NTSC-U.",
  games = {"GZLE01"},
  revision = 0,
  -- The eyes (two layers): stable textures of the head alone.
  hide = {texture = {"0c78cebd654182ad", "4e3fcb6450b45e35"}},
  settings = {
    {name = "rotation", label = "View rotation", type = "choice",
     options = {"Level", "Fixed", "Full", "Camera", "Yaw"}, default = 0,
     description = "Level: turns with Link, the game's camera does not move the view. Fixed: the "
       .. "headset does all the turning. Full: the head's tilt too. Camera and Yaw: the game's "
       .. "camera moves the view."},
    {name = "heading", label = "Turn with", type = "choice",
     options = {"Link's body", "Link's head"}, default = 0,
     description = "The body turns smoothly; the head also turns where Link looks."},
    {name = "yaw", label = "Yaw offset", unit = "deg", default = 0, min = -180, max = 180,
     step = 1},
    {name = "up", label = "Up", unit = "m", default = 0, min = -0.5, max = 0.5, step = 0.01},
    {name = "forward", label = "Forward", unit = "m", default = 0, min = -0.5, max = 0.5,
     step = 0.01},
    {name = "upm", label = "Units per meter (0: the profile's)", default = 0, min = 0, max = 500,
     step = 1},
    {name = "smoothing", label = "Smoothing (-1: the Camera Anchor setting)", default = -1,
     min = -1, max = 0.95, step = 0.05},
    {name = "hide_radius", label = "Hide the head within", unit = "game units", default = 40,
     min = 0, max = 80, step = 1,
     description = "The draws centred this close to the head joint, and not lower than the next "
       .. "setting under it, are hidden: the head's parts sit above the joint."},
    {name = "hide_below", label = "Down to", unit = "game units", default = 5, min = 0,
     max = 20, step = 1,
     description = "How far under the head joint a hidden draw's centre can be (the shoulders "
       .. "are 13 under it)."},
    {name = "stick", label = "Stick forward", type = "choice",
     options = {"Link's facing", "VR: where you look, left and right turn",
                "The game's camera"}, default = 1,
     description = "What the stick does. Link's facing: up keeps him going straight, a sideways "
       .. "push turns him, a push back turns him around. VR: up and back walk forward and backward "
       .. "where you look and left and right turn the view (Turn speed), as in VR games; the view "
       .. "rotation is then Fixed. The game's camera: as the game does."},
    {name = "turn_speed", label = "Turn speed", unit = "deg/s", default = 180, min = 30,
     max = 360, step = 5,
     description = "How fast left and right on the stick turn the view with Stick forward: VR."},
  },
}

-- Two of the global info's pointers to Link's actor.
local PLAYERS = {0x803CA74C, 0x803C9DEC}
local MODEL_IN_PLAYER = 0x32C
local MODEL_VTABLE = 0x8039EA50
local LINK_JOINTS = 42
local HEAD = 15 * 0x30
local CAMERA = 0x803EDA58
-- 0 while Link moves freely: the stick is his to walk with.
local GAME_MODE = 0x803CA80A
local ROTATIONS = {"level", "fixed", "full", "camera", "yaw"}
local STICKS = {"facing", "head", "camera"}

local function apply_options()
  local body = settings.heading == 0
  -- VR controls: the stick's left and right turn a Fixed view, up and back walk where you look.
  local vr_controls = settings.stick == 1
  vr.set_camera_anchor_options{
    rotation = vr_controls and "fixed" or ROTATIONS[settings.rotation + 1], yaw = settings.yaw,
    up = settings.up, forward = settings.forward,
    upm = settings.upm > 0 and settings.upm or nil,
    smoothing = settings.smoothing >= 0 and settings.smoothing or nil,
    hide_radius = settings.hide_radius, hide_below = settings.hide_below,
    -- The game reads the pad, moves its camera, then Link with the new camera.
    stick = vr_controls and "head" or STICKS[settings.stick + 1], stick_lead = 1,
    stick_turn_speed = vr_controls and settings.turn_speed or 0,
    up_axis = "y", forward_axis = body and "z" or "y", bone_up_axis = body and "y" or "x",
  }
end

function on_load()
  apply_options()
end

function on_setting_changed()
  apply_options()
end

-- Link's J3DModel, or nil (no player, a stage loading).
local function link_model()
  for _, at in ipairs(PLAYERS) do
    local player = mem.u32(at)
    local model = player and mem.valid(player, MODEL_IN_PLAYER + 4) and
                  mem.u32(player + MODEL_IN_PLAYER)
    if model and mem.valid(model, 0x90) and mem.u32(model) == MODEL_VTABLE and
       mem.u32(model + 0x14) == player then
      local data = mem.u32(model + 4)
      if data and mem.valid(data, 0x2C) and mem.u16(data + 0x28) == LINK_JOINTS then
        return model
      end
    end
  end
  return nil
end

function on_frame()
  local model = link_model()
  local joints = model and mem.u32(model + 0x8C)
  if not joints then
    return
  end
  local heading = settings.heading == 0 and model + 0x24 or nil
  vr.set_camera_anchor_stick(mem.u8(GAME_MODE) == 0)
  vr.set_camera_anchor_matrices(joints + HEAD, CAMERA, "view", heading)
end
