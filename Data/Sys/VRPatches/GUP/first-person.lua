-- Shadow the Hedgehog (NTSC-U) and Shadow the Hedgehog: Reloaded: first-person view.
--
-- With Camera Anchor on (VR settings, or its hotkey), the VR camera is at Shadow's eyes, level,
-- so the game's camera, which swings around him, no longer moves the view. By default it follows
-- his root (the point at his feet that the game moves and turns with him), raised to his eyes: his
-- head spins with him in a jump or a homing attack, and a view following it would spin too.
-- "Follow: his head" uses his head joint instead (the view then bobs with his head). The game
-- moves Shadow relative to its camera, so the main stick is turned: by default (Stick forward: VR)
-- up and back walk where you look and left and right turn the view, as in VR games (his body is
-- hidden while he runs away from the view); with "Shadow's facing" the view turns with him and up
-- runs where he faces. Not while the game holds the controls (pause menu, events). During events
-- the view goes back to the game's camera, unless the "In events" setting keeps it.
--
-- His head is hidden while the view is in it, and his whole body while he is in the air, where he
-- somersaults and curls up around the view. With "Gun in your hand", his left hand and the weapon
-- it holds follow a VR controller, and his left forearm is hidden. The head is drawn in the same
-- draw as his body (his model is split by material, not by part), so no draw can be skipped for
-- it: instead a code cave works where the game hands his bones to RenderWare to draw him: a hidden
-- bone becomes a point, a moved one the matrix this patch computed. The game's own bones, which
-- the view and the game's logic read, are left as they are. The weapon is moved in the game
-- itself: the cave hands it the controller's pose where it asks where his left hand is, so the
-- game places it, and its shots, there.
--
-- Memory (Reloaded 1.2, found with the agent server's Workflows 6 and 7):
-- - [[807D71A0]] (also [[805EF324] + 0x60]): Shadow's player object, his name at + 0x3C.
-- - [player + 0xA0]: his animation (0x2EA standing, 0xEA running, 0x4C jumping: upright, then a
--   somersault, then 0xCC curled); [player + 0xAC]: his status, 0x40000000 while on the ground.
-- - [player + 0x230]: his motion controller; [that + 0x3E8]: his skeleton: 30 bones (+ 0x0), their
--   matrices at + 0x4 (= skeleton + 0x160), 0x70 bytes each: the world transform (a GameCube Mtx)
--   at + 0, rebuilt every frame (the local matrix at + 0x30 and the scale at + 0x60 are not used).
--   Bone 0 is the root at his feet, 12 the head joint at his neck, 13 to 21 the head's quills and
--   top, 22 to 25 his left arm (25 the hand), 26 to 29 the right one. He faces their -Z, their +Y
--   is up; the root stays level in jumps.
-- - The motion controller's records, at + 0xD0, every 0x38 bytes: the points he aims and holds
--   with, computed once a frame by 800AA760 (r3 = the motion controller, r4 = the record): + 0 the
--   bone, + 4 its position, + 0x10 its -Z axis, + 0x1C its +Y axis. Record 3 is his right hand
--   (bone 29), 5 his left hand (bone 25).
-- - His right glove is a model of its own, a 7-bone skeleton laid out like his, rooted at bone 29
--   (one of its bones is in the records). His left hand is a piece attached to bone 25.
-- - A weapon he holds is a "ShadowWeapon" (its holder at + 0x28), whatever the weapon: 8023AA28
--   places it each frame at his left hand's record (its call to 800AA760 at 8023AA48), and the
--   game draws it and fires from there.
-- - The bones reach RenderWare through these calls, hooked: 80015740 syncs a frame of a model from
--   a game bone (its call at 80015774 to glxCopyMatrix, 80016390: r3 = the frame's RwMatrix, r4 =
--   the bone's world transform); 802A5538 and 802A57AC place a thing at a node, which 8041D31C
--   resolves to a bone (their calls to glxCopyMatrix at 802A5590 and 802A5804); 800B6090 places the
--   frames attached to a bone (his eyes: its call at 800B60CC to 800591F0, which converts a bone, r4,
--   to an RwMatrix, r3); 800B0EAC places the pieces attached to a bone (his left hand: its call at
--   800B0EEC to 8040C148, which copies the bone, r4, to the stack, r3; then it turns the copy 180
--   degrees and converts it). Each call, and the weapon's at 8023AA48, may clobber r0, r5 to r10,
--   ctr and f0 to f5.
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
-- - Standing with the pistol, his left hand is 2.2 units left of the root, 4.2 up and 1.0 forward,
--   turned by GRIP from it: the hand's pose relative to a controller held level and forward.
-- - A pointer path to the pistol's skeleton through [player + 0x224] (version 1.1) was chance: a
--   soldier nearby changed it.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "First-person view",
  author = "DolphinXR",
  version = "1.3",
  description = "With Camera Anchor on, the VR camera is at Shadow's eyes: level, turning with "
    .. "him whatever the game's camera does, his head hidden, his gun in your hand, and up on "
    .. "the stick runs where he faces. For Shadow the Hedgehog NTSC-U and Reloaded.",
  games = {"GUPE8P", "GUPR8P"},
  scratch = 0x3D0,
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
     description = "In the air he somersaults and curls up around the view. With Stick forward: "
       .. "VR, also while he runs away from where you look (walking backward)."},
    {name = "gun", label = "Gun in your hand", type = "choice",
     options = {"Off", "Right controller", "Left controller"}, default = 1,
     description = "His left hand and the weapon it holds follow a VR controller in first "
       .. "person, and the weapon fires from there; his left forearm is hidden."},
    {name = "gun_pitch", label = "Gun pitch", unit = "deg", default = 0, min = -90, max = 90,
     step = 1, description = "Turns the gun up (positive) in your hand."},
    {name = "gun_yaw", label = "Gun yaw", unit = "deg", default = 0, min = -90, max = 90,
     step = 1, description = "Turns the gun left (positive) in your hand."},
    {name = "gun_roll", label = "Gun roll", unit = "deg", default = 0, min = -90, max = 90,
     step = 1},
    {name = "gun_forward", label = "Gun forward", unit = "m", default = 0, min = -0.3, max = 0.3,
     step = 0.01},
    {name = "gun_up", label = "Gun up", unit = "m", default = 0, min = -0.3, max = 0.3,
     step = 0.01},
    {name = "gun_right", label = "Gun right", unit = "m", default = 0, min = -0.3, max = 0.3,
     step = 0.01},
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
     options = {"Shadow's facing", "VR: where you look, left and right turn",
                "The game's camera"}, default = 1,
     description = "What the stick does. Shadow's facing: up keeps him going straight, a sideways "
       .. "push turns him, a push back turns him around. VR: up and back walk forward and backward "
       .. "where you look and left and right turn the view (Turn speed), as in VR games; the view "
       .. "rotation is then Fixed. The game's camera: as the game does."},
    {name = "turn_speed", label = "Turn speed", unit = "deg/s", default = 180, min = 30,
     max = 360, step = 5,
     description = "How fast left and right on the stick turn the view with Stick forward: VR."},
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
local HEAD_END = 22    -- the head joint and its children: 12 to 21
local LEFT_ARM = 22    -- 22 to 25
local LEFT_HAND = 25
local ARM_END = 26
local CAMERAS = 0x8056EE14
local VIEW_IN_CAMERA = 0x3C
local CONTROLS_HELD = 0x8057D768
local PAUSE_MENU = 0x8057D724
-- The cave's entries: the function the hooked call goes to, and how the entry makes the bone's
-- key in r7 (the pieces attached to a bone get the bone's address + 1, a hand's record the motion
-- controller's address).
local ENTRIES = {{0x80016390, "mr r7, r4"},     -- glxCopyMatrix(RwMatrix, bone)
                 {0x800591F0, "mr r7, r4"},     -- 800591F0(RwMatrix, bone)
                 {0x8040C148, "ori r7, r4, 1"}, -- 8040C148(Mtx copy, bone)
                 {0x800AA760, "mr r7, r3"}}     -- 800AA760(motion controller, record)
-- The calls that hand a bone's matrix to RenderWare: {address, original word, entry}.
local HOOKS = {{0x80015774, 0x48000C1D, 1},   -- the frame sync
               {0x802A5590, 0x4BD70E01, 1},   -- things held at a node
               {0x802A5804, 0x4BD70B8D, 1},
               {0x800B60CC, 0x4BFA3125, 2},   -- frames attached to a bone: his eyes
               {0x800B0EEC, 0x4835B25D, 3},   -- pieces attached to a bone: his left hand
               {0x8023AA48, 0x4BE6FD19, 4}}   -- the weapon he holds asks for his left hand
-- The camera update's copy of the view matrix and the code around the hooks: this game's
-- executable.
local SIGNATURE = {{0x80270840, 0x387F003C}, {0x80270848, 0x4819B901},
                   {0x80015768, 0x80860004}, {0x80015778, 0x4800001C},
                   {0x802A558C, 0x38650010}, {0x802A5800, 0x38650010},
                   {0x800B60C8, 0x80840000}, {0x800B60D0, 0x80810008},
                   {0x800B0EE8, 0x809E0000}, {0x800B0EF4, 0x386100C0},
                   {0x8023AA30, 0x38800005}, {0x8023AA44, 0x80630230}, {0x8023AA4C, 0x7C7F1B78}}
-- Eyes from each anchor, game units: {up, forward}.
local EYES = {{6.7, 2.2}, {0.9, 1.6}}
local ROTATIONS = {"level", "fixed", "camera"}
local STICKS = {"facing", "head", "camera"}
local GUN_HANDS = {"right", "left"}
-- His left hand's rotation relative to the controller (rows): his hand relative to his root when
-- he stands with the pistol, turned down 20 degrees and rolled 90, so that the barrel points level
-- along the controller's aim with the grip down (measured with a posed controller: the gun ahead
-- of the hand, rising with the controller's pitch, thin across and tall above the hand).
local GRIP = {0.9934, -0.0673, -0.0984, -0.0844, -0.9817, -0.1775, -0.0848, 0.1850, -0.9798}

-- The motion controller's records of the bones he aims and holds with: a bone pointer every 0x38
-- bytes.
local HOLD_RECORDS = 0xD0
local HOLD_RECORD_COUNT = 6
local HOLD_RECORD_SIZE = 0x38

local S = mem.scratch
-- The hidden bones: two ranges [first, end) of matrices, 0 when unused. The first range is his
-- and must be set when the other is: its first bone is where the hidden ones shrink to.
local HIDDEN = S + 0x00
local HIDDEN_RANGES = 2
local TINY = S + 0x10        -- the hidden bones' scale
local ZERO = S + 0x14
local ROOT = S + 0x18        -- his root's matrix: the moved bones are relative to it
local MOVE_COUNT = S + 0x1C  -- the moved bones in the table, 0 = none
local TEMP = S + 0x20        -- the matrix the cave hands to the game's function (0x30 bytes)
local RECORD = S + 0x50      -- the hand's record the cave hands to the weapon (0x28 bytes)
local TABLE = S + 0x78       -- the moved bones: the key, then their matrix relative to the root
local ENTRY_SIZE = 0x34
local TABLE_SIZE = 2
local CAVE = TABLE + TABLE_SIZE * ENTRY_SIZE  -- one entry per hooked function, then their code

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- Called instead of the hooked functions, whose source (r4) is a bone (the entry puts its key in
-- r7): a moved bone's matrix is the root's times its table matrix, a hidden one (his, or his right
-- glove's) a point at his first hidden bone; the cave builds it at TEMP and calls the function the
-- entry names (r6) with r4 = TEMP. Any other bone goes to that function untouched. For the
-- weapon's call (the key is r3, his motion controller) the moved matrix becomes a hand's record at
-- RECORD (its position, -Z and +Y axes), returned in r3 instead of calling the function.
local function cave_words()
  local o = function(address) return string.format("0x%X", address - S) end
  local text = ""
  for i, e in ipairs(ENTRIES) do
    text = text .. string.format("entry%d:\n", i) .. load_address("r6", e[1]) .. e[2] ..
           "\nb lookup\n"
  end
  text = text .. "lookup:\n" .. load_address("r5", S) ..
         "lwz r8, " .. o(MOVE_COUNT) .. "(r5)\n" ..
         string.format("addi r9, r5, 0x%X\n", TABLE - S - ENTRY_SIZE) ..
         "cmpwi r8, 0\nbeq ranges\n" ..
         "next:\n" .. string.format("lwzu r10, 0x%X(r9)\n", ENTRY_SIZE) ..
         "cmplw r10, r7\nbeq move\naddic. r8, r8, -1\nbne next\n" ..
         "ranges:\n"
  for i = 0, HIDDEN_RANGES - 1 do
    text = text .. string.format("lwz r0, 0x%X(r5)\ncmplw r7, r0\nblt range%d\n", i * 8, i) ..
           string.format("lwz r0, 0x%X(r5)\ncmplw r7, r0\nblt collapse\nrange%d:\n", i * 8 + 4, i)
  end
  text = text .. "b call\n" ..
         "collapse:\nlwz r10, 0(r5)\nlfs f0, " .. o(TINY) .. "(r5)\nlfs f1, " .. o(ZERO) .. "(r5)\n"
  for row = 0, 2 do
    for col = 0, 2 do
      text = text .. string.format("stfs %s, 0x%X(r5)\n", row == col and "f0" or "f1",
                                   TEMP - S + row * 16 + col * 4)
    end
    text = text .. string.format("lfs f2, 0x%X(r10)\nstfs f2, 0x%X(r5)\n", row * 16 + 12,
                                 TEMP - S + row * 16 + 12)
  end
  text = text .. "b temp\nmove:\nlwz r10, " .. o(ROOT) .. "(r5)\n"
  -- TEMP = root * the entry's matrix (r9 + 4), both 3x4 row-major.
  for row = 0, 2 do
    for k = 0, 3 do
      text = text .. string.format("lfs f%d, 0x%X(r10)\n", 2 + k, row * 16 + k * 4)
    end
    for col = 0, 3 do
      text = text .. string.format("lfs f0, 0x%X(r9)\nfmuls f1, f2, f0\n", 4 + col * 4) ..
             string.format("lfs f0, 0x%X(r9)\nfmadds f1, f3, f0, f1\n", 4 + 16 + col * 4) ..
             string.format("lfs f0, 0x%X(r9)\nfmadds f1, f4, f0, f1\n", 4 + 32 + col * 4)
      if col == 3 then
        text = text .. "fadds f1, f1, f5\n"
      end
      text = text .. string.format("stfs f1, 0x%X(r5)\n", TEMP - S + row * 16 + col * 4)
    end
  end
  text = text .. "cmplw r7, r3\nbeq record\n" ..
         "temp:\n" .. string.format("addi r4, r5, 0x%X\n", TEMP - S) ..
         "call:\nmtctr r6\nbctr\nrecord:\n"
  -- RECORD + 4: the position (column 3), + 0x10: -Z (column 2), + 0x1C: +Y (column 1).
  for row = 0, 2 do
    text = text .. string.format("lfs f0, 0x%X(r5)\nstfs f0, 0x%X(r5)\n",
                                 TEMP - S + row * 16 + 12, RECORD - S + 4 + row * 4) ..
           string.format("lfs f0, 0x%X(r5)\nfneg f0, f0\nstfs f0, 0x%X(r5)\n",
                         TEMP - S + row * 16 + 8, RECORD - S + 0x10 + row * 4) ..
           string.format("lfs f0, 0x%X(r5)\nstfs f0, 0x%X(r5)\n",
                         TEMP - S + row * 16 + 4, RECORD - S + 0x1C + row * 4)
  end
  text = text .. string.format("addi r3, r5, 0x%X\nblr\n", RECORD - S)
  local words, err = asm.assemble(text, CAVE)
  if not words then error("assembling the cave: " .. err) end
  assert(CAVE + #words * 4 <= S + mem.scratch_size, "the cave does not fit in the scratch memory")
  return words
end

-- Each entry is four words: lis, ori, the key, b.
local function entry(i)
  return CAVE + (i - 1) * 16
end

local warned = false
local anchored = nil  -- the bone matrices of the last field, for the log
local cave = nil
local holding = nil   -- whether the weapon was in the controller's hand last field, for the log

local function apply_options()
  local upm = settings.upm > 0 and settings.upm or vr.units_per_meter()
  local eyes = EYES[settings.follow + 1]
  -- VR controls: the stick's left and right turn a Fixed view, up and back walk where you look.
  local vr_controls = settings.stick == 1
  vr.set_camera_anchor_options{
    rotation = vr_controls and "fixed" or ROTATIONS[settings.rotation + 1], yaw = settings.yaw,
    up = eyes[1] / upm + settings.up, forward = eyes[2] / upm + settings.forward,
    upm = upm, smoothing = settings.smoothing, heading_smoothing = settings.heading_smoothing,
    stick = vr_controls and "head" or STICKS[settings.stick + 1], stick_lead = 2,
    stick_turn_speed = vr_controls and settings.turn_speed or 0,
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

-- Hides his bones from first to last, and up to two more ranges (gloves): first, end pairs.
local function hide_bones(...)
  local ranges = {...}
  for i = 1, HIDDEN_RANGES * 2 do
    mem.write_u32(HIDDEN + (i - 1) * 4, ranges[i] or 0)
  end
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
  mem.write_u32(MOVE_COUNT, 0)
  mem.write_f32(TINY, 0.001)
  mem.write_f32(ZERO, 0)
  mem.ensure_code(CAVE, cave)
  local changed = false
  for _, hook in ipairs(HOOKS) do
    changed = mem.ensure_code(hook[1], asm.branch(hook[1], entry(hook[3]), true)) or changed
  end
  if changed then
    log("hooks installed to hide and move his bones, cave at", mem.hex(CAVE))
  end
end

local function uninstall()
  hide_bones(0, 0)
  mem.write_u32(MOVE_COUNT, 0)
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

-- A skeleton's first bone matrix and the end of them, or nil: a header 0x160 bytes before the
-- bones holds their count and the address of the first.
local function skeleton_at(header)
  local count = header and mem.u32(header)
  if not count or count < 1 or count > 64 or mem.u32(header + 4) ~= header + 0x160 or
     not mem.valid(header + 0x160, count * BONE_SIZE) then
    return nil
  end
  return header + 0x160, header + 0x160 + count * BONE_SIZE
end

-- Shadow's player object and bone matrices, or nil (no player, a stage loading, another
-- character's model).
local function shadow()
  for _, at in ipairs(PLAYERS) do
    local player = mem.ptr(at[1], at[2])
    if player and mem.valid(player, CONTROLLER_IN_PLAYER + 4) and
       mem.u32(player + 0x3C) == NAME[1] and mem.u32(player + 0x40) == NAME[2] then
      local first, last = skeleton_at(mem.ptr(player + CONTROLLER_IN_PLAYER,
                                              SKELETON_IN_CONTROLLER))
      if first and last - first == BONES * BONE_SIZE then
        return player, first
      end
    end
  end
  return nil
end

-- The bones of his right glove, from the motion controller's records, or nil.
local function right_glove(player, bones)
  local motion = mem.u32(player + CONTROLLER_IN_PLAYER)
  if not motion or not mem.valid(motion, HOLD_RECORDS + HOLD_RECORD_COUNT * HOLD_RECORD_SIZE) then
    return nil
  end
  for i = 0, HOLD_RECORD_COUNT - 1 do
    local bone = mem.u32(motion + HOLD_RECORDS + i * HOLD_RECORD_SIZE)
    if bone and (bone < bones or bone >= bones + BONES * BONE_SIZE) and mem.valid(bone, 0x30) then
      for k = 0, 31 do
        local first, last = skeleton_at(bone - k * BONE_SIZE - 0x160)
        if first then
          return first, last
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

local function inverse(a)
  local m00, m01, m02 = a[1], a[2], a[3]
  local m10, m11, m12 = a[5], a[6], a[7]
  local m20, m21, m22 = a[9], a[10], a[11]
  local c00, c01, c02 = m11 * m22 - m12 * m21, m02 * m21 - m01 * m22, m01 * m12 - m02 * m11
  local det = m00 * c00 + m10 * c01 + m20 * c02
  if math.abs(det) < 1e-12 then return nil end
  local d = 1 / det
  local i = {
    c00 * d, c01 * d, c02 * d, 0,
    (m12 * m20 - m10 * m22) * d, (m00 * m22 - m02 * m20) * d, (m02 * m10 - m00 * m12) * d, 0,
    (m10 * m21 - m11 * m20) * d, (m01 * m20 - m00 * m21) * d, (m00 * m11 - m01 * m10) * d, 0,
  }
  for r = 0, 2 do
    i[r * 4 + 4] = -(i[r * 4 + 1] * a[4] + i[r * 4 + 2] * a[8] + i[r * 4 + 3] * a[12])
  end
  return i
end

-- Rotation about an axis (1 x, 2 y, 3 z) by degrees, as a 3x4 matrix.
local function rotation(axis, degrees)
  local c, s = math.cos(math.rad(degrees)), math.sin(math.rad(degrees))
  if axis == 1 then return {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0} end
  if axis == 2 then return {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0} end
  return {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}
end

-- The controller's pose in the game's world (the game camera's view axes, then the view matrix
-- inverted), or nil.
local function controller_in_world(view, upm)
  local valid, px, py, pz, qx, qy, qz, qw = vr.raw_pose(GUN_HANDS[settings.gun], "view")
  if not valid then return nil end
  local pose = {
    1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw), px * upm,
    2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw), py * upm,
    2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy), pz * upm,
  }
  local to_world = inverse(view)
  return to_world and mul(to_world, pose)
end

-- Fills the table so that his left hand, and the weapon it holds, are in the controller's:
-- the hand's piece on bone 25, and the record the weapon asks for (keyed by his motion
-- controller). True, or false when it cannot (no controller).
local function move_gun(player, bones, view)
  local motion = mem.u32(player + CONTROLLER_IN_PLAYER)
  local root = {mem.f32s(bones, 12)}
  local upm = settings.upm > 0 and settings.upm or vr.units_per_meter()
  local controller = controller_in_world({mem.f32s(view, 12)}, upm)
  local to_root = inverse(root)
  if not controller or not to_root then
    mem.write_u32(MOVE_COUNT, 0)
    return false
  end
  -- The hand's target: the controller, the offsets, then the grip.
  local offset = mul(mul(rotation(2, settings.gun_yaw), rotation(1, settings.gun_pitch)),
                     rotation(3, settings.gun_roll))
  offset[4], offset[8], offset[12] = settings.gun_right * upm, settings.gun_up * upm,
                                     -settings.gun_forward * upm
  local target = mul(mul(controller, offset), {GRIP[1], GRIP[2], GRIP[3], 0, GRIP[4], GRIP[5],
                                               GRIP[6], 0, GRIP[7], GRIP[8], GRIP[9], 0})
  local hand_to_root = mul(to_root, target)
  mem.write_u32(TABLE, bones + LEFT_HAND * BONE_SIZE + 1)
  mem.write_f32s(TABLE + 4, table.unpack(hand_to_root))
  mem.write_u32(TABLE + ENTRY_SIZE, motion)
  mem.write_f32s(TABLE + ENTRY_SIZE + 4, table.unpack(hand_to_root))
  mem.write_u32(ROOT, bones)
  mem.write_u32(MOVE_COUNT, 2)
  return true
end

-- The view of this field: in Shadow's head (true), or the game's camera (false).
-- Whether he faces away from where the headset looks (VR controls, walking backward: his legs
-- swing in front of the view).
local function facing_away(bones, view)
  local valid, _, _, _, qx, qy, qz, qw = vr.raw_pose("head", "view")
  if not valid then return false end
  -- The headset's forward (-Z) in the game camera's view axes, then in the world (the view's
  -- rotation transposed).
  local fx, fy = -2 * (qx * qz + qy * qw), -2 * (qy * qz - qx * qw)
  local fz = 2 * (qx * qx + qy * qy) - 1
  local v = {mem.f32s(view, 12)}
  local wx = v[1] * fx + v[5] * fy + v[9] * fz
  local wz = v[3] * fx + v[7] * fy + v[11] * fz
  -- His facing: the root's -Z.
  local r = {mem.f32s(bones, 12)}
  return wx * -r[3] + wz * -r[11] < 0
end

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
    mem.write_u32(MOVE_COUNT, 0)
    return
  end
  local gun = settings.gun > 0 and move_gun(player, bones, view)
  if gun ~= holding then
    log(gun and "his left hand and weapon follow the controller" or
        "his weapon stays in his hand")
    holding = gun
  end
  if not gun then
    mem.write_u32(MOVE_COUNT, 0)
  end
  local in_the_air = (mem.u32(player + STATUS_IN_PLAYER) or 0) & ON_THE_GROUND == 0
  local away = settings.stick == 1 and facing_away(bones, view)
  if settings.hide_body == 2 or (settings.hide_body == 0 and (in_the_air or away)) then
    -- His right glove with him (his left hand is a piece on bone 25: hidden with him, or
    -- moved to the controller with the weapon).
    local right_first, right_end = right_glove(player, bones)
    hide_bones(bones, bones + BONES * BONE_SIZE, right_first or 0, right_end or 0)
  elseif settings.hide_head then
    hide_bones(bones + HEAD * BONE_SIZE,
               bones + (gun and ARM_END or HEAD_END) * BONE_SIZE)
  elseif gun then
    hide_bones(bones + LEFT_ARM * BONE_SIZE, bones + ARM_END * BONE_SIZE)
  else
    hide_bones(0, 0)
  end
end
