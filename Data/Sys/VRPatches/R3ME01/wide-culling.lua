-- Metroid Prime Trilogy (NTSC-U, Wii): the three games cull the world for the headset's view, not
-- the game camera's.
--
-- Each game skips the room geometry and the actors outside its camera's view frustum
-- (CFrustumPlanes, built from the camera's transform, its vertical field of view and aspect
-- ratio). In VR you see more than the game camera does, and you look around it with your head:
-- objects pop in at the edges of the view. The patch hooks the calls that build the frustum of the
-- world view (Metroid Prime and Echoes: the actors' pre-render, the world draw and the planes kept
-- after it; Corruption: its camera's frustum) and builds it from your head's direction instead,
-- with a wider angle: at least "Wide view angle" up and down and left and right, and never
-- narrower than the game's. "No culling" leaves the frustum without planes, so that nothing is
-- culled by it. The frustums of the reticles, the scan visor and the shadows are left as they
-- are.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Wide culling",
  author = "DolphinXR",
  version = "1.0",
  description = "Rooms and objects are culled for what the headset shows, around where you look, "
    .. "instead of the game camera's narrower view: no more geometry popping at the edges. For "
    .. "the three games of Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
  scratch = 0x280,
  settings = {
    {name = "mode", label = "Culling", type = "choice", options = {"Wide view", "No culling"},
     default = 0,
     description = "Wide view: the game culls what is outside a wide view around where you "
       .. "look. No culling: it draws everything the rooms around you hold (more work for the "
       .. "GPU)."},
    {name = "fov", label = "Wide view angle", unit = "deg", default = 115, min = 90, max = 170,
     step = 5,
     description = "The angle kept up and down and left and right. Headsets show about 100 to "
       .. "110 degrees: a few more keep the edges while you turn your head."},
    {name = "follow_head", label = "Follow the head", type = "bool", default = true,
     description = "The wide view turns with your head. Off: it stays on the game camera."},
  },
}

-- ---- The games ----

-- Each game of the disc is its own executable, recognized by the code at its sites. A site is a
-- call to the frustum's constructor (r3 the frustum, r4 the camera's transform). Metroid Prime and
-- Echoes pass the vertical field of view in f1 (radians) and the aspect ratio in f2; Corruption
-- passes a pointer to the field of view in r5 and the aspect ratio in f1. The constructor makes
-- the horizontal half angle the vertical one times the aspect ratio.
local GAMES = {
  {
    name = "Metroid Prime",
    constructor = 0x802FD564,
    fov_in = "f1",
    sites = {
      {hook = 0x8022C680, original = 0x480D0EE5, after = 0x807D0850},  -- actors' pre-render
      {hook = 0x8022C9D0, original = 0x480D0B95, after = 0x806D9C74},  -- the world draw
      {hook = 0x8022DD38, original = 0x480CF82D, after = 0x806D9C74},  -- the planes after it
    },
  },
  {
    name = "Metroid Prime 2: Echoes",
    constructor = 0x80307530,
    fov_in = "f1",
    sites = {
      {hook = 0x8024567C, original = 0x480C1EB5, after = 0x387B2C14},  -- actors' pre-render
      {hook = 0x80245C44, original = 0x480C18ED, after = 0x387C2C14},  -- the world draw
      {hook = 0x80247934, original = 0x480BFBFD, after = 0x387D2C14},  -- the planes after it
    },
  },
  {
    name = "Metroid Prime 3: Corruption",
    constructor = 0x8036D474,
    fov_in = "r5",
    sites = {
      {hook = 0x800BC7A4, original = 0x482B0CD1, after = 0x80DF0014},  -- the camera's frustum
    },
  },
}

-- ---- Scratch memory (mem.scratch) ----

local S = mem.scratch
local MODE = S + 0x00     -- 0 the game's frustum, 1 wide, 2 no planes
local FOV = S + 0x04      -- the wide angle, radians
local VFOV = S + 0x08     -- Corruption: the field of view passed by pointer
local FRUSTUM = S + 0x0C  -- the frustum being built
local RETURN = S + 0x10   -- the routine's return address
local HEAD = S + 0x14     -- 3x3 row-major: the head's rotation in the camera's axes
local XF = S + 0x38       -- the transform passed instead of the camera's (3x4)
local ROUTINE = S + 0x68  -- the code each site calls instead of the constructor

local function load_address(register, address)
  local hi = (address >> 16) & 0xFFFF
  if hi >= 0x8000 then hi = hi - 0x10000 end
  return string.format("lis %s, %d\nori %s, %s, 0x%X\n", register, hi, register, register,
                       address & 0xFFFF)
end

-- The routine of g, called by its sites in place of the constructor, with its arguments. In the
-- wide mode: the transform = the camera's rotation * HEAD, at the camera's position; the vertical
-- angle = max(FOV, the game's); the horizontal one = max(FOV, the game's), through the aspect
-- ratio. Then the constructor; in the no-planes mode the frustum's plane count is set to 0 after
-- it. Only volatile registers change besides the arguments, as across the original call.
local function routine_words(g)
  local function o(address) return address - S end
  local t = {"mflr r0\n", load_address("r11", S),
             string.format("stw r0, 0x%X(r11)\nlwz r12, 0(r11)\ncmpwi r12, 1\nbne call\n",
                           o(RETURN))}
  for row = 0, 2 do
    local r = row * 0x10
    t[#t + 1] = string.format("lfs f5, 0x%X(r4)\nlfs f6, 0x%X(r4)\nlfs f7, 0x%X(r4)\n", r, r + 4,
                              r + 8)
    for col = 0, 2 do
      t[#t + 1] = string.format(
        "lfs f8, 0x%X(r11)\nlfs f9, 0x%X(r11)\nlfs f10, 0x%X(r11)\n" ..
        "fmuls f11, f5, f8\nfmadds f11, f6, f9, f11\nfmadds f11, f7, f10, f11\n" ..
        "stfs f11, 0x%X(r11)\n", o(HEAD) + col * 4, o(HEAD) + 12 + col * 4,
        o(HEAD) + 24 + col * 4, o(XF) + r + col * 4)
    end
    t[#t + 1] = string.format("lfs f11, 0x%X(r4)\nstfs f11, 0x%X(r11)\n", r + 12, o(XF) + r + 12)
  end
  -- f12 = the game's vertical angle, f13 = its aspect ratio.
  if g.fov_in == "f1" then
    t[#t + 1] = "fmr f12, f1\nfmr f13, f2\n"
  else
    t[#t + 1] = "lfs f12, 0(r5)\nfmr f13, f1\n"
  end
  -- f8 = the vertical angle, f10 = the horizontal one, f13 = their ratio.
  t[#t + 1] = string.format(
    "lfs f8, 0x%X(r11)\nfmr f10, f8\nfmuls f9, f12, f13\n" ..
    "fcmpo cr0, f9, f10\nble h_ok\nfmr f10, f9\nh_ok:\n" ..
    "fcmpo cr0, f12, f8\nble v_ok\nfmr f8, f12\nv_ok:\n" ..
    "fdivs f13, f10, f8\naddi r4, r11, 0x%X\n", o(FOV), o(XF))
  if g.fov_in == "f1" then
    t[#t + 1] = "fmr f1, f8\nfmr f2, f13\n"
  else
    t[#t + 1] = string.format("stfs f8, 0x%X(r11)\naddi r5, r11, 0x%X\nfmr f1, f13\n", o(VFOV),
                              o(VFOV))
  end
  t[#t + 1] = string.format("call:\nstw r3, 0x%X(r11)\nbl `0x%X`\n", o(FRUSTUM), g.constructor)
  t[#t + 1] = load_address("r11", S)
  t[#t + 1] = string.format(
    "lwz r12, 0(r11)\ncmpwi r12, 2\nbne done\nlwz r12, 0x%X(r11)\nli r0, 0\nstw r0, 0(r12)\n" ..
    "done:\nlwz r0, 0x%X(r11)\nmtlr r0\nblr\n", o(FRUSTUM), o(RETURN))
  local words, err = asm.assemble(table.concat(t), ROUTINE)
  if not words then error("assembling the routine for " .. g.name .. ": " .. err) end
  assert(ROUTINE + #words * 4 <= S + mem.scratch_size,
         "the routine does not fit in the scratch memory")
  return words
end

-- ---- Finding the game, installing and removing the hooks ----

local current = nil       -- the game in GAMES whose hooks are installed
local routines = {}       -- the built routine per game
local waiting_logged = false

local function branch_target(from, word)
  if word & 0xFC000002 ~= 0x48000000 then return nil end
  local offset = word & 0x03FFFFFC
  if offset >= 0x02000000 then offset = offset - 0x04000000 end
  return (from + offset) & 0xFFFFFFFF
end

-- Whether a site's code is in memory: the hook holds the original call or a call of the routine,
-- followed by the site's next instruction.
local function site_present(site)
  if mem.u32(site.hook + 4) ~= site.after then return false end
  local word = mem.u32(site.hook)
  if word == site.original then return true end
  local target = branch_target(site.hook, word or 0)
  return target == ROUTINE
end

-- Whether g's executable is in memory: all its sites.
local function present(g)
  for _, site in ipairs(g.sites) do
    if not site_present(site) then return false end
  end
  return true
end

local function detect()
  for _, g in ipairs(GAMES) do
    if present(g) then return g end
  end
  return nil
end

local function install()
  local g = detect()
  if not g then
    if current then log("no game of the trilogy is running") end
    current = nil
    if not waiting_logged then log("waiting for one of the three games") end
    waiting_logged = true
    return
  end
  routines[g] = routines[g] or routine_words(g)
  local changed = mem.ensure_code(ROUTINE, routines[g])
  for _, site in ipairs(g.sites) do
    changed = mem.ensure_code(site.hook, asm.branch(site.hook, ROUTINE, true)) or changed
  end
  if changed or g ~= current then
    log(g.name .. ": hooks installed, routine at", mem.hex(ROUTINE))
  end
  current = g
  waiting_logged = false
end

local function uninstall()
  local g = current
  current = nil
  if not g then return end
  for _, site in ipairs(g.sites) do
    if branch_target(site.hook, mem.u32(site.hook) or 0) == ROUTINE then
      mem.write_code(site.hook, site.original)
    end
  end
end

-- ---- The head ----

-- The per-field code uses plain numbers, not the math objects (each is a Lua table, which the
-- Quest's garbage collector would have to free).
local raw_pose, write_f32s, rad = vr.raw_pose, mem.write_f32s, math.rad

-- The head's rotation seen from the game camera, from OpenXR's view axes (+X right, +Y up,
-- -Z forward) to the camera's (+X right, +Y forward, +Z up), row-major; the identity when it does
-- not follow the head or there is no head pose (flat screen).
local function write_head()
  local ok, px, py, pz, x, y, z, w = false
  if settings.follow_head then
    ok, px, py, pz, x, y, z, w = raw_pose("head", "view")
  end
  if not ok then
    return write_f32s(HEAD, 1, 0, 0, 0, 1, 0, 0, 0, 1)
  end
  return write_f32s(HEAD,
                    1 - 2 * (y * y + z * z), -2 * (x * z + y * w), 2 * (x * y - z * w),
                    -2 * (x * z - y * w), 1 - 2 * (x * x + y * y), -2 * (y * z + x * w),
                    2 * (x * y + z * w), -2 * (y * z - x * w), 1 - 2 * (x * x + z * z))
end

local function update()
  local mode = settings.mode == 1 and 2 or 1
  if mode == 1 and not write_head() then mode = 0 end
  write_f32s(FOV, rad(settings.fov))
  mem.write_u32(MODE, mode)
end

-- ---- Hooks ----

function on_load()
  mem.write_u32(MODE, 0)
  install()
  update()
end

function on_state_loaded()
  current = nil
  mem.write_u32(MODE, 0)
  install()
  update()
end

function on_frame()
  -- Another game of the disc may have started: its executable replaced the hooked one.
  if game.field() % 60 == 0 or not current or not present(current) then install() end
  update()
end

function on_unload()
  mem.write_u32(MODE, 0)
  uninstall()
end
