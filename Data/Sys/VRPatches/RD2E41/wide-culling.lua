-- Red Steel 2 (NTSC-U): the game culls the world for the headset's view, not the game camera's.
--
-- The game builds one view frustum per frame from its camera (80210494: the view matrix times
-- the projection, five planes and four corner points) and culls with it the objects and the
-- sectors seen through portals: in VR, rooms and objects outside the game camera's view are
-- missing when you look around (a doorway shows the sky instead of the room behind it). The
-- patch hooks the multiplication of the view matrix by the projection in that builder and gives
-- it the camera turned by your head, with a wider angle: at least "Wide view angle" up and down
-- and left and right, and never narrower than the game's. The game then builds its planes and
-- corners from that.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Wide culling",
  author = "DolphinXR",
  version = "1.0",
  description = "Rooms and objects are culled for what the headset shows, around where you look, "
    .. "instead of the game camera's narrower view: no more missing rooms and objects at the "
    .. "edges. For Red Steel 2 NTSC-U.",
  games = {"RD2E41"},
  revision = 0,
  scratch = 0x180,
  settings = {
    {name = "fov", label = "Wide view angle", unit = "deg", default = 130, min = 90, max = 170,
     step = 5,
     description = "The angle kept up and down and left and right. Headsets show about 100 to "
       .. "125 degrees: a few more keep the edges while you turn your head."},
    {name = "follow_head", label = "Follow the head", type = "bool", default = true,
     description = "The wide view turns with your head. Off: it stays on the game camera."},
  },
}

-- ---- The game ----

local WORLD = 0x8054E178        -- [r13 - 0x6F48]: the renderer, view matrix at +0x210 and
                                -- projection at +0x6FC (4x4, row vectors: clip = p * V * P)
local VIEW_OFFSET = 0x210
local PROJECTION_OFFSET = 0x6FC
local CONCAT = 0x800379C4       -- 4x4 multiplication: r3 = r4 * r5
local HOOK = 0x802104F4         -- in the frustum builder: bl CONCAT (r3 its matrix, r4 the view,
                                -- r5 the projection, also kept in r27 and r28 for the corners)
local HOOK_ORIGINAL = 0x4BE274D1
-- The builder's code around the hook: lwz r6, -0x6F48(r13) / addi r28, r6, 0x6FC, and after it
-- mr r4, r27.
local SIGNATURE = {{0x802104D8, 0x80CD90B8}, {0x802104E8, 0x3B8606FC}, {0x802104F8, 0x7F64DB78}}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local MODE = S + 0x00     -- 0 the game's frustum, 1 wide
local RETURN = S + 0x04   -- the routine's return address
local DEST = S + 0x08     -- the builder's matrix
local HEAD = S + 0x10     -- 4x4: the head's rotation in the camera's view axes (row vectors)
local WIDE = S + 0x50     -- 4x4: the game's projection with the wide angle
local XF = S + 0x90       -- 4x4: the view turned by the head
local ROUTINE = S + 0xD0  -- the code the hook calls instead of CONCAT

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- Called by the hook in place of CONCAT, with its arguments. In the wide mode, for the main view
-- only: XF = the view * HEAD, then the projection is WIDE; r27 and r28, which the builder keeps
-- for the corner points, follow. Only volatile registers change besides those, as across the
-- original call, and the builder saves r27 and r28 itself.
local function routine_words()
  local function o(address) return address - S end
  local text = "mflr r0\n" .. load_address("r11", S) ..
    string.format("stw r0, 0x%X(r11)\n", o(RETURN)) ..
    string.format("lwz r12, 0x%X(r11)\ncmpwi r12, 0\nbeq call\n", o(MODE)) ..
    string.format("lwz r12, -0x6F48(r13)\naddi r12, r12, 0x%X\ncmplw r4, r12\nbne call\n",
                  VIEW_OFFSET) ..
    string.format("stw r3, 0x%X(r11)\naddi r3, r11, 0x%X\naddi r5, r11, 0x%X\nbl `0x%X`\n",
                  o(DEST), o(XF), o(HEAD), CONCAT) ..
    load_address("r11", S) ..
    string.format("lwz r3, 0x%X(r11)\naddi r4, r11, 0x%X\nmr r27, r4\n", o(DEST), o(XF)) ..
    string.format("addi r5, r11, 0x%X\nmr r28, r5\n", o(WIDE)) ..
    string.format("call:\nbl `0x%X`\n", CONCAT) ..
    load_address("r11", S) ..
    string.format("lwz r0, 0x%X(r11)\nmtlr r0\nblr\n", o(RETURN))
  local words, err = asm.assemble(text, ROUTINE)
  if not words then error("assembling the routine: " .. err) end
  assert(ROUTINE + #words * 4 <= S + mem.scratch_size,
         "the routine does not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hook ----

local installed = false
local routine = nil
local waiting_logged = false

local function present()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then return false end
  end
  local word = mem.u32(HOOK)
  return word == HOOK_ORIGINAL or word == asm.branch(HOOK, ROUTINE, true)
end

local function install()
  if not present() then
    installed = false
    if not waiting_logged then log("waiting for the game's frustum code") end
    waiting_logged = true
    return
  end
  routine = routine or routine_words()
  local changed = mem.ensure_code(ROUTINE, routine)
  changed = mem.ensure_code(HOOK, asm.branch(HOOK, ROUTINE, true)) or changed
  if changed or not installed then log("hook installed, routine at", mem.hex(ROUTINE)) end
  installed = true
  waiting_logged = false
end

local function uninstall()
  if mem.u32(HOOK) == asm.branch(HOOK, ROUTINE, true) then
    mem.write_code(HOOK, HOOK_ORIGINAL)
  end
  installed = false
end

-- ---- The head and the wide projection ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local raw_pose, f32s, write_f32s, u32 = vr.raw_pose, mem.f32s, mem.write_f32s, mem.u32
local tan, rad, min = math.tan, math.rad, math.min

-- The head's rotation seen from the game camera, from OpenXR's view axes (+X right, +Y up,
-- -Z forward) to the game's (+X right, +Y up, +Z forward), as the matrix a row vector of the
-- camera's view space is multiplied by to get the head's view space. False without a head pose
-- (flat screen).
local function write_head()
  local ok, x, y, z, w = false, 0, 0, 0, 1
  if settings.follow_head then
    local px, py, pz
    ok, px, py, pz, x, y, z, w = raw_pose("head", "view")
    if not ok then return false end
  elseif not raw_pose("head", "view") then
    return false
  end
  write_f32s(HEAD,
             1 - 2 * (y * y + z * z), 2 * (x * y - z * w), -2 * (x * z + y * w), 0,
             2 * (x * y + z * w), 1 - 2 * (x * x + z * z), -2 * (y * z - x * w), 0,
             -2 * (x * z - y * w), -2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0,
             0, 0, 0, 1)
  return true
end

-- The game's projection with its x and y scales lowered to the wide angle's, when narrower.
-- False when it is not a perspective of the expected form.
local function write_wide()
  local world = u32(WORLD)
  if not world or world < 0x80000000 or world >= 0x81800000 then return false end
  local p00, p01, p02, p03, p10, p11, p12, p13, p20, p21, p22, p23, p30, p31, p32, p33 =
    f32s(world + PROJECTION_OFFSET, 16)
  if not p00 or p03 ~= 0 or p13 ~= 0 or p23 ~= 1 or p33 ~= 0 or p00 <= 0 or p11 <= 0 then
    return false
  end
  local scale = 1 / tan(rad(settings.fov) / 2)
  write_f32s(WIDE, min(p00, scale), p01, p02, p03, p10, min(p11, scale), p12, p13,
             p20, p21, p22, p23, p30, p31, p32, p33)
  return true
end

local function update()
  local mode = (installed and write_head() and write_wide()) and 1 or 0
  mem.write_u32(MODE, mode)
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(MODE, 0)
  install()
  update()
end

function on_state_loaded()
  installed = false
  mem.write_u32(MODE, 0)
  install()
  update()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then install() end
  update()
end

function on_unload()
  mem.write_u32(MODE, 0)
  uninstall()
end
