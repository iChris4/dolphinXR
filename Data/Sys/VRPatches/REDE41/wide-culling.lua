-- Red Steel (NTSC-U): the game culls the world for the headset's view, not the game camera's.
--
-- The game draws the rooms seen through portals from the camera's room, and the objects in them,
-- when they cross its view frustum (803151C8 tests a sphere against its planes). The frustum is
-- built by 80276214 from the eight corners of the clip cube, turned into world points by the
-- camera's inverse view projection (r4 + 0x190, row vectors: the inverse projection times the
-- camera's world matrix) through 802716C4: in VR, rooms and objects outside the game camera's
-- view are missing when you look around (the sky shows behind you). The patch hooks that corner
-- transform when the render setup (803675A8) builds the world camera's frustum, and gives it the
-- camera turned by your head with a wider angle: at least "Wide view angle" up and down and left
-- and right, and never narrower than the game's. The game then builds its planes from that. The
-- camera itself is unchanged (its inverse view projection also turns the Wii pointer into the
-- aim).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Wide culling",
  author = "DolphinXR",
  version = "1.0",
  description = "Rooms and objects are culled for what the headset shows, around where you look, "
    .. "instead of the game camera's narrower view: no more missing rooms and objects at the "
    .. "edges and behind you. For Red Steel NTSC-U.",
  games = {"REDE41"},
  revision = 0,
  scratch = 0x240,
  settings = {
    {name = "fov", label = "Wide view angle", unit = "deg", default = 130, min = 100, max = 170,
     step = 5,
     description = "The angle kept up and down and left and right. Headsets show about 100 to "
       .. "125 degrees: a few more keep the edges while you turn your head."},
    {name = "follow_head", label = "Follow the head", type = "bool", default = true,
     description = "The wide view turns with your head. Off: it stays on the game camera."},
  },
}

-- ---- The game (Red Steel NTSC-U) ----

-- 80276214 (r4 the camera) builds a frustum from the corners of the clip cube or, for the world
-- camera, from four corners at a depth: a "bl 802716C4" in a loop for each (r3 the world point,
-- r4 the camera, r5 the corner). Its caller's return address is at r1 + 0x524 there. The
-- camera's projection is at +0x90, its inverse at +0x110 and the inverse view projection at
-- +0x190 (4x4, row vectors).
local HOOKS = {0x80276300, 0x80276714}
local HOOK_ORIGINALS = {0x4BFFB3C5, 0x4BFFAFB1}
local TRANSFORM = 0x802716C4    -- r3 = (r5 x, y, z, w) * (r4 + 0x190)
local CONCAT = 0x804B7B68       -- PSMTX44Concat(a, b, ab): ab = a * b
local RENDER_SETUP = 0x80367BB0 -- 803675A8's return address from 80276214
local CALLER_SLOT = 0x524
local PROJECTION = 0x90
local INVERSE_PROJECTION = 0x110
local INVERSE_VIEW_PROJECTION = 0x190
local SIGNATURE = {{0x80276214, 0x9421FAE0}, {0x8027621C, 0x90010524},
                   {0x802762FC, 0x38610420}, {0x80276304, 0x7C79BA14},
                   {0x80276710, 0x38610248}, {0x80276718, 0x7C12E214},
                   {0x80367BAC, 0x4E800421}, {0x80367BB0, 0x7E84A378}}
-- The world camera's near plane is 10 game units (its projection's row 3 column 2, at +0xC8, is
-- about -10); the player's weapon view's is 1.
local NEAR_LIMIT = -5.0

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local ARMED = S + 0x00      -- 1 when the routine widens the world camera's frustum
local RETURN = S + 0x04     -- the builder's return address
local SAVE = S + 0x08       -- r3, r4, r5 of the call
local CALLER = S + 0x14     -- RENDER_SETUP
local LIMIT = S + 0x18      -- f32: NEAR_LIMIT
local HEAD = S + 0x20       -- 4x4: the head's axes in the camera's view axes (row vectors)
local WIDE = S + 0x60       -- 4x4: the inverse of the projection with the wide angle
local TEMP = S + 0xA0       -- 4x4: HEAD * the projection, then the wide inverse view projection
local TEMP2 = S + 0xE0      -- 4x4: WIDE * HEAD * the projection
local ROUTINE = S + 0x120

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

local function o(address) return address - S end

-- In place of the corner transform's calls, with their arguments. For the world camera's frustum
-- of the render setup: WIDE's depth rows are the camera's (+0x110), its x and y scales at least
-- the camera's, and the corner goes through WIDE * HEAD * the projection * the camera's inverse
-- view projection, a matrix in TEMP that the transform reads as r4 + 0x190. Only volatile
-- registers change, as across the original call.
local function routine_words()
  local reload = load_address("r11", S)
  local t = {reload}
  t[#t + 1] = string.format("lwz r12, 0x%X(r11)\ncmpwi r12, 0\nbeq plain\n", o(ARMED))
  t[#t + 1] = string.format("lwz r12, 0x%X(r1)\nlwz r10, 0x%X(r11)\ncmplw r12, r10\nbne plain\n",
                            CALLER_SLOT, o(CALLER))
  t[#t + 1] = string.format("lfs f0, 0x%X(r4)\nlfs f1, 0x%X(r11)\nfcmpo cr0, f0, f1\n" ..
                            "bge plain\n", PROJECTION + 0x38, o(LIMIT))
  t[#t + 1] = string.format("mflr r0\nstw r0, 0x%X(r11)\nstw r3, 0x%X(r11)\nstw r4, 0x%X(r11)\n" ..
                            "stw r5, 0x%X(r11)\n", o(RETURN), o(SAVE), o(SAVE) + 4, o(SAVE) + 8)
  -- WIDE's depth rows (2 and 3) from the camera's inverse projection.
  t[#t + 1] = string.format("addi r9, r4, 0x%X\naddi r10, r11, 0x%X\nli r0, 8\nmtctr r0\n" ..
                            "rows:\nlwzu r0, 4(r9)\nstwu r0, 4(r10)\nbdnz rows\n",
                            INVERSE_PROJECTION + 0x20 - 4, o(WIDE) + 0x20 - 4)
  -- Never narrower than the camera's x and y scales.
  for _, offset in ipairs({0x00, 0x14}) do
    t[#t + 1] = string.format("lfs f0, 0x%X(r4)\nlfs f1, 0x%X(r11)\nfcmpo cr0, f0, f1\n" ..
                              "ble keep%X\nstfs f0, 0x%X(r11)\nkeep%X:\n",
                              INVERSE_PROJECTION + offset, o(WIDE) + offset, offset,
                              o(WIDE) + offset, offset)
  end
  -- TEMP2 = WIDE * (HEAD * projection), then TEMP = TEMP2 * the inverse view projection.
  t[#t + 1] = string.format("addi r3, r11, 0x%X\naddi r4, r4, 0x%X\naddi r5, r11, 0x%X\n" ..
                            "bl `0x%X`\n", o(HEAD), PROJECTION, o(TEMP), CONCAT)
  t[#t + 1] = reload .. string.format("addi r3, r11, 0x%X\naddi r4, r11, 0x%X\n" ..
                                      "addi r5, r11, 0x%X\nbl `0x%X`\n", o(WIDE), o(TEMP),
                                      o(TEMP2), CONCAT)
  t[#t + 1] = reload .. string.format("addi r3, r11, 0x%X\nlwz r4, 0x%X(r11)\n" ..
                                      "addi r4, r4, 0x%X\naddi r5, r11, 0x%X\nbl `0x%X`\n",
                                      o(TEMP2), o(SAVE) + 4, INVERSE_VIEW_PROJECTION, o(TEMP),
                                      CONCAT)
  t[#t + 1] = reload .. string.format("lwz r3, 0x%X(r11)\nlwz r5, 0x%X(r11)\n" ..
                                      "addi r4, r11, %d\nlwz r0, 0x%X(r11)\nmtlr r0\n",
                                      o(SAVE), o(SAVE) + 8,
                                      o(TEMP) - INVERSE_VIEW_PROJECTION, o(RETURN))
  t[#t + 1] = string.format("plain:\nb `0x%X`\n", TRANSFORM)
  local words, err = asm.assemble(table.concat(t), ROUTINE)
  if not words then error("assembling the routine: " .. err) end
  assert(ROUTINE + #words * 4 <= S + mem.scratch_size,
         "the routine does not fit in the scratch memory")
  return words
end

-- ---- Installing and removing the hooks ----

local installed = false
local routine = nil
local waiting_logged = false

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- A branch from a hook into the patches' scratch memory is this patch's routine, maybe at
-- another address, kept by a savestate made while it ran: installing replaces it.
local function our_branch(hook, word)
  local target = branch_target(hook, word or 0)
  return target ~= nil and target >= 0x80001800 and target < 0x80003000
end

local function present()
  for _, check in ipairs(SIGNATURE) do
    if mem.u32(check[1]) ~= check[2] then return false end
  end
  for i, hook in ipairs(HOOKS) do
    local word = mem.u32(hook)
    if word ~= HOOK_ORIGINALS[i] and not our_branch(hook, word) then return false end
  end
  return true
end

local function install()
  if not present() then
    installed = false
    if not waiting_logged then log("waiting for the game's frustum code") end
    waiting_logged = true
    return
  end
  routine = routine or routine_words()
  mem.write_u32(CALLER, RENDER_SETUP)
  mem.write_f32(LIMIT, NEAR_LIMIT)
  local changed = mem.ensure_code(ROUTINE, routine)
  for _, hook in ipairs(HOOKS) do
    changed = mem.ensure_code(hook, asm.branch(hook, ROUTINE, true)) or changed
  end
  if changed or not installed then log("hooks installed, routine at", mem.hex(ROUTINE)) end
  installed = true
  waiting_logged = false
end

local function uninstall()
  mem.write_u32(ARMED, 0)
  for i, hook in ipairs(HOOKS) do
    if our_branch(hook, mem.u32(hook)) then mem.write_code(hook, HOOK_ORIGINALS[i]) end
  end
  installed = false
end

-- ---- The head and the wide angle ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local raw_pose, write_f32s = vr.raw_pose, mem.write_f32s
local tan, rad = math.tan, math.rad

-- HEAD: the head's right, up and forward axes seen from the game camera, in the game's view axes
-- (+X right, +Y up, +Z forward: OpenXR's with z negated), as the rows of the matrix a row vector
-- of the head's view space is multiplied by to get the camera's. False without a head pose (flat
-- screen).
local function write_head()
  local ok, _, _, _, x, y, z, w = raw_pose("head", "view")
  if not ok then return false end
  if not settings.follow_head then
    x, y, z, w = 0, 0, 0, 1
  end
  return write_f32s(HEAD,
                    1 - 2 * (y * y + z * z), 2 * (x * y + z * w), -2 * (x * z - y * w), 0,
                    2 * (x * y - z * w), 1 - 2 * (x * x + z * z), -2 * (y * z + x * w), 0,
                    -2 * (x * z + y * w), -2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
                    0, 0, 0, 1)
end

-- WIDE's x and y scales (the inverse projection's: the tangents of the half angles); the routine
-- copies the camera's depth rows and keeps the camera's scales when they are wider.
local function write_wide()
  local half = tan(rad(settings.fov) / 2)
  return write_f32s(WIDE, half, 0, 0, 0, 0, half, 0, 0)
end

local function update()
  local armed = installed and write_head() and write_wide()
  mem.write_u32(ARMED, armed and 1 or 0)
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(ARMED, 0)
  install()
  update()
end

function on_state_loaded()
  installed = false
  mem.write_u32(ARMED, 0)
  install()
  update()
end

function on_frame()
  if game.field() % 60 == 0 or not installed then install() end
  update()
end

function on_unload()
  uninstall()
end
