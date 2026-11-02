-- Shadow the Hedgehog (NTSC-U) and Shadow the Hedgehog: Reloaded: first-person view.
--
-- With Camera Anchor on (VR settings, or its hotkey), the VR camera is at Shadow's eyes: level,
-- turning with him, so the game's camera, which swings around him, no longer moves the view. By
-- default it follows his root (the point at his feet that the game moves and turns with him),
-- raised to his eyes: his head spins with him in a jump or a homing attack, and a view following
-- it would spin too. "Follow: his head" uses his head joint instead (the view then bobs with his
-- head). The game moves Shadow relative to its camera: the main stick is turned so that up runs
-- where he faces, except while the game holds the controls (pause menu, events). During events the
-- view goes back to the game's camera, unless the "In events" setting keeps it.
--
-- His head is hidden while the view is in it, and his whole body and the weapon he holds while he
-- is in the air, where he somersaults and curls up around the view. The head is drawn in the same
-- draw as his body (his
-- model is split by material, not by part), so no draw can be skipped for it: instead a code cave
-- collapses the bones to a point where the game hands them to RenderWare to draw him. The game's
-- own bones, which the view and the game's logic read, are left as they are.
--
-- Memory (Reloaded 1.2, found with the agent server's Workflow 7):
-- - [[807D71A0]] (also [[805EF324] + 0x60]): Shadow's player object, his name at + 0x3C.
-- - [player + 0xA0]: his animation (0x2EA standing, 0xEA running, 0x4C jumping: upright, then a
--   somersault, then 0xCC curled); [player + 0xAC]: his status, 0x40000000 while on the ground.
-- - [player + 0x230]: his motion controller; [that + 0x3E8]: his skeleton: 30 bones (+ 0x0), their
--   matrices at + 0x4 (= skeleton + 0x160), 0x70 bytes each: the world transform (a GameCube Mtx)
--   at + 0, rebuilt every frame (the local matrix at + 0x30 and the scale at + 0x60 are not used).
--   Bone 0 is the root at his feet, 12 the head joint at his neck, 13 to 21 the head's quills and
--   top. He faces their -Z, their +Y is up; the root stays level in jumps.
-- - [motion + 0xD0], then every 0x38 bytes, 6 records: the bones he aims and holds with (his head
--   joint, his hands, and one of the weapon he holds, whose own skeleton has the same layout: 7
--   bones for the pistol).
-- - The bones reach RenderWare through four calls, hooked: 80015740 syncs a frame of a model (his,
--   or his weapon's) from a game bone (its call at 80015774 to glxCopyMatrix, 80016390: r3 = the
--   frame's RwMatrix, r4 = the bone's world transform); 802A5538 and 802A57AC place a thing at a
--   node, which 8041D31C resolves to a bone (their calls to glxCopyMatrix at 802A5590 and
--   802A5804); 800B6090 places the frames attached to a bone (his eyes: its call at 800B60CC to
--   800591F0, r3 = an RwMatrix on the stack, all 16 words written, r4 = the bone). All four calls
--   may clobber r0, r5, r6, ctr, f0 and f1.
-- - 8056EE14: the camera manager: a count at + 0x8, 8-byte entries from [+ 0xC], each pointing to a
--   camera, active while its first byte is not 0, its world -> view matrix at + 0x3C (written by
--   the camera update at 80270618).
-- - 8057D768: not 0 while the game holds the controls (pause menu, events); 8057D724: the pause
--   menu is open.
-- - The game moves Shadow with a camera two updates newer than the one a patch reads: holding the
--   stick toward his facing while the camera swung 66 to 85 degrees, his path turned 80 degrees
--   with no lead, 47 with a lead of 1 and 5 with a lead of 2.
-- - His eyes are 6.7 units above the root and 1.8 in front of it, 0.9 above the head joint and 1.6
--   in front of it. He is 10 units tall and a soldier about 19: 10 units per meter is true to scale.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "First-person view",
  author = "DolphinXR",
  version = "1.0",
  description = "With Camera Anchor on, the VR camera is at Shadow's eyes: level, turning with "
    .. "him whatever the game's camera does, his head hidden, and up on the stick runs where he "
    .. "faces. For Shadow the Hedgehog NTSC-U and Reloaded.",
  games = {"GUPE8P", "GUPR8P"},
  scratch = 0xE0,
  settings = {
    {name = "follow", label = "Follow", type = "choice",
     options = {"His body (steady)", "His head"}, default = 0,
     description = "His body: the view stays at his eye height above his feet, steady in jumps "
       .. "and homing attacks. His head: the view moves with his head, which bobs when he runs "
       .. "and turns over in a spin."},
    {name = "hide_head", label = "Hide his head", type = "bool", default = true,
     description = "His head, quills and eyes shrink to a point while the view is in first "
       .. "person."},
    {name = "hide_body", label = "Hide his body", type = "choice",
     options = {"In the air", "Never", "Always"}, default = 0,
     description = "In the air he somersaults and curls up around the view."},
    {name = "rotation", label = "View rotation", type = "choice",
     options = {"Level", "Fixed", "Camera"}, default = 0,
     description = "Level: turns with Shadow, the game's camera does not move the view. Fixed: "
       .. "the headset does all the turning. Camera: the game's camera turns the view."},
    {name = "yaw", label = "Yaw offset", unit = "deg", default = 0, min = -180, max = 180,
     step = 1},
    {name = "up", label = "Up", unit = "m", default = 0, min = -1, max = 1, step = 0.01,
     description = "Added to his eye height."},
    {name = "forward", label = "Forward", unit = "m", default = 0, min = -1, max = 1,
     step = 0.01},
    {name = "upm", label = "Units per meter (0: the profile's)", default = 10, min = 0,
     max = 100, step = 0.5,
     description = "10: true to scale (Shadow 1 m tall, his eyes 0.7 m above the ground). Lower "
       .. "values raise the view and make the world look smaller."},
    {name = "smoothing", label = "Position smoothing", default = 0, min = 0, max = 0.95,
     step = 0.05,
     description = "Fraction of the last position kept per field. Shadow runs fast: smoothing "
       .. "makes the view trail behind him."},
    {name = "heading_smoothing", label = "Turn smoothing", default = 0.5, min = 0, max = 0.95,
     step = 0.05},
    {name = "stick", label = "Stick forward", type = "choice",
     options = {"Shadow's facing", "Where you look", "The game's camera"}, default = 0,
     description = "What up on the stick means. Shadow's facing: up keeps him going straight, a "
       .. "sideways push turns him. Where you look: the headset's direction. The game's camera: "
       .. "as the game does."},
    {name = "events", label = "In events", type = "choice",
     options = {"The game's camera", "First person"}, default = 0,
     description = "While the game holds the controls (outside the pause menu)."},
  },
}

local PLAYERS = {{0x807D71A0, 0}, {0x805EF324, 0x60}}
local NAME = {0x53686164, 0x6F770000}  -- "Shadow"
local STATUS_IN_PLAYER = 0xAC
local ON_THE_GROUND = 0x40000000
local CONTROLLER_IN_PLAYER = 0x230
local SKELETON_IN_CONTROLLER = 0x3E8
local BONES = 30
local BONE_SIZE = 0x70
local HEAD = 12
local HEAD_END = 22  -- the head joint and its children: 12 to 21
local CAMERAS = 0x8056EE14
local VIEW_IN_CAMERA = 0x3C
local CONTROLS_HELD = 0x8057D768
local PAUSE_MENU = 0x8057D724
-- The functions the hooked calls go to, one cave entry each: glxCopyMatrix and 800591F0.
local TARGETS = {0x80016390, 0x800591F0}
-- The calls that hand a bone's matrix to RenderWare: {address, original word, target}.
local HOOKS = {{0x80015774, 0x48000C1D, 1},   -- the frame sync
               {0x802A5590, 0x4BD70E01, 1},   -- things held at a node: his weapons
               {0x802A5804, 0x4BD70B8D, 1},
               {0x800B60CC, 0x4BFA3125, 2}}   -- frames attached to a bone: his eyes
-- The camera update's copy of the view matrix and the code around the hooks: this game's
-- executable.
local SIGNATURE = {{0x80270840, 0x387F003C}, {0x80270848, 0x4819B901},
                   {0x80015768, 0x80860004}, {0x80015778, 0x4800001C},
                   {0x802A558C, 0x38650010}, {0x802A5800, 0x38650010},
                   {0x800B60C8, 0x80840000}, {0x800B60D0, 0x80810008}}
-- Eyes from each anchor, game units: {up, forward}.
local EYES = {{6.7, 2.2}, {0.9, 1.6}}
local ROTATIONS = {"level", "fixed", "camera"}
local STICKS = {"facing", "head", "camera"}

-- The motion controller's records of the bones he aims and holds with (his head, his hands, the
-- weapon he holds): a bone pointer every 0x38 bytes.
local HOLD_RECORDS = 0xD0
local HOLD_RECORD_COUNT = 6
local HOLD_RECORD_SIZE = 0x38

local S = mem.scratch
local HIDE_FIRST = S + 0x00  -- the first hidden bone's matrix (where they shrink to), else 0
local HIDE_END = S + 0x04    -- the end of the hidden bones' matrices, else 0
local ITEM_FIRST = S + 0x08  -- the bones of what he holds, hidden with his, else 0
local ITEM_END = S + 0x0C
local TINY = S + 0x10        -- the hidden bones' scale
local ZERO = S + 0x14
local CAVE = S + 0x18        -- one entry per hooked function, then the code they share

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- Called instead of the hooked functions (r3 = an RwMatrix: right, up, at and pos rows, each
-- followed by a flags or pad word; r4 = the bone): a hidden bone (his, or of what he holds)
-- becomes a point at his first hidden one, any other goes to the function (r6) that the entry
-- names.
local function cave_words()
  local text = ""
  for i, target in ipairs(TARGETS) do
    text = text .. string.format("entry%d:\n", i) .. load_address("r6", target) .. "b hide\n"
  end
  text = text .. "hide:\n" .. load_address("r5", S) ..
         "lwz r0, 0(r5)\ncmplw r4, r0\nblt item\n" ..
         "lwz r0, 4(r5)\ncmplw r4, r0\nblt collapse\n" ..
         "item:\nlwz r0, 8(r5)\ncmplw r4, r0\nblt copy\n" ..
         "lwz r0, 12(r5)\ncmplw r4, r0\nbge copy\n" ..
         "collapse:\nlwz r6, 0(r5)\nlfs f0, 0x10(r5)\nlfs f1, 0x14(r5)\nli r0, 0\n" ..
         "stfs f0, 0(r3)\nstfs f1, 4(r3)\nstfs f1, 8(r3)\nstw r0, 0xC(r3)\n" ..
         "stfs f1, 0x10(r3)\nstfs f0, 0x14(r3)\nstfs f1, 0x18(r3)\nstw r0, 0x1C(r3)\n" ..
         "stfs f1, 0x20(r3)\nstfs f1, 0x24(r3)\nstfs f0, 0x28(r3)\nstw r0, 0x2C(r3)\n" ..
         "lfs f0, 0xC(r6)\nstfs f0, 0x30(r3)\n" ..
         "lfs f0, 0x1C(r6)\nstfs f0, 0x34(r3)\n" ..
         "lfs f0, 0x2C(r6)\nstfs f0, 0x38(r3)\nstw r0, 0x3C(r3)\n" ..
         "blr\n" ..
         "copy:\nmtctr r6\nbctr\n"
  local words, err = asm.assemble(text, CAVE)
  if not words then error("assembling the cave: " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

-- Each entry is three words: lis, ori, b.
local function entry(i)
  return CAVE + (i - 1) * 12
end

local warned = false
local anchored = nil  -- the bone matrices of the last field, for the log
local cave = nil

local function apply_options()
  local upm = settings.upm > 0 and settings.upm or vr.units_per_meter()
  local eyes = EYES[settings.follow + 1]
  vr.set_camera_anchor_options{
    rotation = ROTATIONS[settings.rotation + 1], yaw = settings.yaw,
    up = eyes[1] / upm + settings.up, forward = eyes[2] / upm + settings.forward,
    upm = upm, smoothing = settings.smoothing, heading_smoothing = settings.heading_smoothing,
    stick = STICKS[settings.stick + 1], stick_lead = 2,
    up_axis = "y", forward_axis = "-z", bone_up_axis = "y",
  }
end

local function is_this_game()
  for _, word in ipairs(SIGNATURE) do
    if mem.u32(word[1]) ~= word[2] then
      return false
    end
  end
  return true
end

-- A bl from a hook into the scratch region (this patch's cave, maybe at another address in an
-- earlier run).
local function our_branch(hook, word)
  if not word or word & 0xFC000003 ~= 0x48000001 then return false end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  local target = (hook + offset) & 0xFFFFFFFF
  return target >= 0x80001800 and target < 0x80003000
end

local function hide_bones(first, last, item_first, item_end)
  mem.write_u32(HIDE_FIRST, first)
  mem.write_u32(HIDE_END, last)
  mem.write_u32(ITEM_FIRST, item_first or 0)
  mem.write_u32(ITEM_END, item_end or 0)
end

local function install()
  for _, hook in ipairs(HOOKS) do
    local word = mem.u32(hook[1])
    if word ~= hook[2] and not our_branch(hook[1], word) then
      return
    end
  end
  cave = cave or cave_words()
  hide_bones(0, 0)
  mem.write_f32(TINY, 0.001)
  mem.write_f32(ZERO, 0)
  mem.ensure_code(CAVE, cave)
  local changed = false
  for _, hook in ipairs(HOOKS) do
    changed = mem.ensure_code(hook[1], asm.branch(hook[1], entry(hook[3]), true)) or changed
  end
  if changed then
    log("hooks installed to hide his head and body, cave at", mem.hex(CAVE))
  end
end

local function uninstall()
  hide_bones(0, 0)
  for _, hook in ipairs(HOOKS) do
    if our_branch(hook[1], mem.u32(hook[1])) then
      mem.write_code(hook[1], hook[2])
    end
  end
end

function on_load()
  apply_options()
  if is_this_game() then
    install()
  end
end

function on_setting_changed()
  apply_options()
end

function on_state_loaded()
  if is_this_game() then
    install()
  end
end

function on_unload()
  uninstall()
end

-- Shadow's player object and bone matrices, or nil (no player, a stage loading, another
-- character's model).
local function shadow()
  for _, at in ipairs(PLAYERS) do
    local player = mem.ptr(at[1], at[2])
    if player and mem.valid(player, CONTROLLER_IN_PLAYER + 4) and
       mem.u32(player + 0x3C) == NAME[1] and mem.u32(player + 0x40) == NAME[2] then
      local skeleton = mem.ptr(player + CONTROLLER_IN_PLAYER, SKELETON_IN_CONTROLLER)
      if skeleton and mem.valid(skeleton, 8) and mem.u32(skeleton) == BONES and
         mem.u32(skeleton + 4) == skeleton + 0x160 and
         mem.valid(skeleton + 0x160, BONES * BONE_SIZE) then
        return player, skeleton + 0x160
      end
    end
  end
  return nil
end

-- The bones of what he holds (his weapon), from the motion controller's records: the first bone
-- matrix and the end of them, or nil. A model's bones follow a header 0x160 bytes before them: the
-- bone count, then the address of the first bone.
local function held_item(player, bones)
  local motion = mem.u32(player + CONTROLLER_IN_PLAYER)
  if not motion or not mem.valid(motion, HOLD_RECORDS + HOLD_RECORD_COUNT * HOLD_RECORD_SIZE) then
    return nil
  end
  for i = 0, HOLD_RECORD_COUNT - 1 do
    local bone = mem.u32(motion + HOLD_RECORDS + i * HOLD_RECORD_SIZE)
    if bone and (bone < bones or bone >= bones + BONES * BONE_SIZE) and mem.valid(bone, 0x30) then
      for k = 0, 31 do
        local first = bone - k * BONE_SIZE
        local count = mem.u32(first - 0x160)
        if count and count > k and count <= 64 and mem.u32(first - 0x15C) == first then
          return first, first + count * BONE_SIZE
        end
      end
    end
  end
  return nil
end

-- The view matrix of the camera the game draws with, or nil.
local function camera_view()
  local count = mem.u32(CAMERAS + 8)
  local list = mem.u32(CAMERAS + 0xC)
  if not count or count > 8 or not list or not mem.valid(list, count * 8) then
    return nil
  end
  for i = 0, count - 1 do
    local camera = mem.u32(list + i * 8)
    if camera and mem.valid(camera, VIEW_IN_CAMERA + 0x30) and mem.u8(camera) ~= 0 then
      return camera + VIEW_IN_CAMERA
    end
  end
  return nil
end

-- The view of this field: in Shadow's head (true), or the game's camera (false).
local function first_person(bones, view)
  local held = mem.u8(CONTROLS_HELD) ~= 0
  vr.set_camera_anchor_stick(view ~= nil and not held)
  if not view or (held and mem.u8(PAUSE_MENU) == 0 and settings.events == 0) then
    return false
  end
  local anchor = settings.follow == 0 and bones or bones + HEAD * BONE_SIZE
  return vr.set_camera_anchor_matrices(anchor, view, "view", bones)
end

function on_frame()
  if not is_this_game() then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(SIGNATURE[1][1]) ..
           ": is this Shadow the Hedgehog NTSC-U or Reloaded?")
      warned = true
    end
    return
  end
  if game.field() % 60 == 0 then
    install()
  end
  local player, bones = shadow()
  local view = bones and camera_view()
  if (view and bones) ~= anchored then
    log(view and ("Shadow's bones at " .. mem.hex(bones) .. ", view matrix at " .. mem.hex(view))
        or "no Shadow model or no active camera: the game's camera")
    anchored = view and bones
  end
  if not first_person(bones, view) or not vr.camera_anchor_active() then
    hide_bones(0, 0)
    return
  end
  local in_the_air = (mem.u32(player + STATUS_IN_PLAYER) or 0) & ON_THE_GROUND == 0
  if settings.hide_body == 2 or (settings.hide_body == 0 and in_the_air) then
    hide_bones(bones, bones + BONES * BONE_SIZE, held_item(player, bones))
  elseif settings.hide_head then
    hide_bones(bones + HEAD * BONE_SIZE, bones + HEAD_END * BONE_SIZE)
  else
    hide_bones(0, 0)
  end
end
