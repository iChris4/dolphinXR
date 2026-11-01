-- The Legend of Zelda: Twilight Princess (GameCube, NTSC-U): first-person view from Link's head.
--
-- With Camera Anchor on (VR settings, or its hotkey), the VR camera sits in Link's head: his head
-- joint, read from his model, composed with the game camera's view matrix. The view is level and
-- turns with Link (his model's own facing), so the game's third-person camera, which keeps
-- swinging around him, no longer moves it, and his head (face, hair, eyes) is hidden while the
-- view is inside it. It replaces the profile's "Links Head" texture anchor, which followed a head
-- draw in the game camera's view. The game moves Link relative to its camera: the main stick is
-- turned so that up walks straight ahead in the view, except during events (dialogs) and in the
-- menus, where the stick picks.
--
-- Memory (NTSC-U rev 0, found with the agent server's Workflow 7):
-- - 8040BF6C (and 8040B098) in the game's global info point to Link's actor; actor + 0x524 is his
--   J3DModel (vtable 803CED20). His model data has 35 joints (J3DModelData + 0x10 is the joint
--   tree, its joint count at + 0x1C): the wolf's skeleton gets no anchor. (A list of models at
--   80411C40 starts with Link's only near his house: another model takes its place elsewhere.)
-- - J3DModel + 0x24: the model's base transform, Link's position and facing (its +Z).
-- - J3DModel + 0x84 -> J3DMtxBuffer, + 0xC -> the joints' world matrices, 0x30 bytes each: joint 4
--   is the head (its +X points where Link faces, its +Y down).
-- - 803DCDCC: the camera's world -> view matrix, written once per game frame. J3D's global view
--   (80434AC8) is rewritten by other views several times per field.
-- - 8040B16C and 8040B16D (the game's event control): 1 during a dialog, 0 while Link moves
--   freely; 8040C075: 1 while the pause or the item menu is open.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "First-person view",
  author = "DolphinXR",
  version = "1.2",
  description = "With Camera Anchor on, the VR camera is in Link's head: level, turning with Link "
    .. "whatever the game's camera does, his head hidden. As a wolf the game's camera returns; "
    .. "cutscenes stay in the head (the Camera Anchor hotkey leaves it). For Twilight Princess "
    .. "(GameCube) NTSC-U.",
  games = {"GZ2E01"},
  revision = 0,
  conflicts = {texture = {"Links Head"}},
  -- Face, hair, eyes, mouth.
  hide = {texture = {"d7f168afaf5c673a", "582a10908b80c70e", "e6d8e9279935e654",
                     "1f25545c58e95893"}},
  settings = {
    {name = "rotation", label = "View rotation", type = "choice",
     options = {"Level", "Fixed", "Full", "Camera", "Yaw"}, default = 0,
     description = "Level: turns with Link, the game's camera does not move the view. Fixed: the "
       .. "headset does all the turning. Full: the head's tilt too. Camera and Yaw: the game's "
       .. "camera moves the view, as the texture anchor did."},
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
    {name = "stick", label = "Stick forward", type = "choice",
     options = {"Link's facing", "Where you look", "The game's camera"}, default = 0,
     description = "What up on the stick means. Link's facing: up keeps him going straight, a "
       .. "sideways push turns him. Where you look: the headset's direction; with Level, looking "
       .. "aside while walking keeps turning Link. The game's camera: as the game does."},
  },
}

-- Two of the global info's pointers to Link's actor.
local PLAYERS = {0x8040BF6C, 0x8040B098}
local MODEL_IN_PLAYER = 0x524
local MODEL_VTABLE = 0x803CED20
local LINK_JOINTS = 35
local HEAD = 4 * 0x30
local CAMERA = 0x803DCDCC
-- An event (a dialog), and the menus: the stick is not Link's to walk with.
local EVENT = 0x8040B16C
local MENU = 0x8040C075
local ROTATIONS = {"level", "fixed", "full", "camera", "yaw"}
local STICKS = {"facing", "head", "camera"}

local function apply_options()
  local body = settings.heading == 0
  vr.set_camera_anchor_options{
    rotation = ROTATIONS[settings.rotation + 1], yaw = settings.yaw,
    up = settings.up, forward = settings.forward,
    upm = settings.upm > 0 and settings.upm or nil,
    smoothing = settings.smoothing >= 0 and settings.smoothing or nil,
    -- The game reads the pad, moves its camera, then Link with the new camera.
    stick = STICKS[settings.stick + 1], stick_lead = 1,
    up_axis = "y", forward_axis = body and "z" or "x", bone_up_axis = body and "y" or "-y",
  }
end

function on_load()
  apply_options()
end

function on_setting_changed()
  apply_options()
end

-- Whether `model` is Link's J3DModel.
local function is_link(model)
  if not model or not mem.valid(model, 0x88) or mem.u32(model) ~= MODEL_VTABLE then
    return false
  end
  local data = mem.u32(model + 4)
  return data ~= nil and mem.valid(data, 0x30) and mem.u16(data + 0x2C) == LINK_JOINTS
end

-- Link's J3DModel, or nil (no player, the wolf, a stage loading).
local function link_model()
  for _, at in ipairs(PLAYERS) do
    local player = mem.u32(at)
    local model = player and mem.valid(player, MODEL_IN_PLAYER + 4) and
                  mem.u32(player + MODEL_IN_PLAYER)
    if model and is_link(model) then
      return model
    end
  end
  return nil
end

function on_frame()
  local model = link_model()
  local joints = model and mem.ptr(model + 0x84, 0xC)
  if not joints then
    return
  end
  local heading = settings.heading == 0 and model + 0x24 or nil
  vr.set_camera_anchor_stick(mem.u16(EVENT) == 0 and mem.u8(MENU) == 0)
  vr.set_camera_anchor_matrices(joints + HEAD, CAMERA, "view", heading)
end
