-- Shadow the Hedgehog (NTSC-U) and Shadow the Hedgehog: Reloaded: the game runs at the headset's
-- refresh rate, and the console's video timing (VBI) with it.
--
-- How the game sets its frame rate. Shadow runs on RenderWare. Every frame its system update
-- (801E1EB8) asks RenderWare for the current video mode (RwEngineGetCurrentVideoMode, then
-- RwEngineGetVideoModeInfo into an RwVideoMode on its stack) and stores the mode's refRate as the
-- game's frame rate (8057D6F0). The GameCube driver reports a refRate of 0, so the game falls
-- back to 50 or 60 by TV format (li r0, 50 / li r0, 60). What times the game reads that rate:
-- the frame limiter waits 1/rate s between frames on the CPU clock, the time steps are 1/rate,
-- the motion step (805EDD60) is 60/rate. The update rewrites all of it every frame, so a new rate
-- applies from the next frame, at any time (writing 8057D6F0 alone is undone the next frame).
--
-- The patch does what the community 90 and 120 FPS codes (dreamsyntax) do: it turns the load of
-- refRate (801E1F54, lwz r0, 0x18(r1)) into li r0, <rate>, and the 60 fallback (801E1F94) too.
-- The game also waits for the video interface's retrace before it shows a frame, so it cannot run
-- faster than the VBI: a rate above the VBI plays in slow motion, which is why the codes need the
-- matching VBI by hand. The patch sets ForcedVBIFrequency to the same rate for the session (the
-- emulated CPU runs faster by the same factor: each frame keeps the console's CPU time; the PC
-- emulates that much more per second).
--
-- Only in a VR session: ForcedVBIFrequency applies only then, and the headset's rate is known only
-- then. Turn the 90 and 120 FPS Action Replay codes off: they write the same instruction.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Frame rate follows the headset",
  author = "DolphinXR",
  version = "1.0",
  description = "The game renders at the headset's refresh rate (72, 90, 120 Hz...) instead of "
    .. "60 fps, at its normal speed: the frame rate code and the VBI frequency are set to match. "
    .. "Turn the 90/120 FPS Action Replay codes off. A higher rate needs a faster PC: the console "
    .. "is emulated that much faster.",
  games = {"GUPE8P", "GUPR8P"},
  settings = {
    {name = "rate", label = "Frame rate", type = "choice",
     options = {"Headset refresh rate", "60 fps", "72 fps", "80 fps", "90 fps", "120 fps",
                "144 fps"},
     default = 0,
     description = "Headset: the refresh rate the VR session reports. A fixed rate below the "
       .. "headset's lightens the emulation; the headset then repeats or reprojects frames."},
  },
}

local RATE_LOAD = 0x801E1F54             -- lwz r0, 0x18(r1): the video mode's refRate
local RATE_LOAD_ORIGINAL = 0x80010018
local NTSC_FALLBACK = 0x801E1F94         -- li r0, 60 when refRate is 0 on an NTSC console
local NTSC_FALLBACK_ORIGINAL = 0x3800003C
-- The words around them: this game's system update.
local SIGNATURE = {
  {0x801E1F50, 0x482A3E95},  -- bl RwEngineGetVideoModeInfo
  {0x801E1F74, 0x901F0050},  -- stw r0, 0x50(r31): the frame rate
  {0x801E1F98, 0x901F0050},
}
local FIXED_RATES = {60, 72, 80, 90, 120, 144}
local MIN_RATE, MAX_RATE = 30, 240       -- what ForcedVBIFrequency accepts
local VBI = "ForcedVBIFrequency"

local applied = nil      -- the rate the game's code holds now; nil: the game's own code
local own_vbi = nil      -- ForcedVBIFrequency before the patch changed it
local warned_code = false
local warned_other = false

local function li_r0(value)
  return 0x38000000 | value
end

local function is_li_r0(word)
  return (word & 0xFFFF0000) == 0x38000000
end

-- The game's code, or this patch's (or a frame rate code's) li r0, <rate> in it.
local function is_this_game()
  for _, word in ipairs(SIGNATURE) do
    if mem.u32(word[1]) ~= word[2] then
      return false
    end
  end
  local load = mem.u32(RATE_LOAD)
  local fallback = mem.u32(NTSC_FALLBACK)
  return (load == RATE_LOAD_ORIGINAL or is_li_r0(load)) and
         (fallback == NTSC_FALLBACK_ORIGINAL or is_li_r0(fallback))
end

-- The rate to run at, or nil for the game's own (no VR session yet).
local function target_rate()
  local headset = vr.refresh_rate()
  if not headset then
    return nil
  end
  local rate = settings.rate == 0 and math.floor(headset + 0.5) or FIXED_RATES[settings.rate]
  if rate < MIN_RATE or rate > MAX_RATE then
    return nil
  end
  return rate
end

local function set_vbi(rate)
  if rate then
    if own_vbi == nil then
      own_vbi = vr.setting(VBI)
    end
    vr.set_setting(VBI, rate)
  elseif own_vbi ~= nil then
    vr.set_setting(VBI, own_vbi)
  end
end

local function restore()
  if mem.u32(RATE_LOAD) == li_r0(applied) then
    mem.write_code(RATE_LOAD, RATE_LOAD_ORIGINAL)
  end
  if mem.u32(NTSC_FALLBACK) == li_r0(applied) then
    mem.write_code(NTSC_FALLBACK, NTSC_FALLBACK_ORIGINAL)
  end
  set_vbi(nil)
  applied = nil
end

local function update(rate)
  if rate == nil then
    if applied then
      restore()
      log("back to the game's own frame rate")
    end
    return
  end
  if not is_this_game() then
    if not warned_code then
      warn("not installed: unexpected code at " .. mem.hex(RATE_LOAD) .. " (" ..
           mem.hex(mem.u32(RATE_LOAD) or 0) .. "): is this Shadow the Hedgehog NTSC-U or Reloaded?")
      warned_code = true
    end
    return
  end
  local word = li_r0(rate)
  local current = mem.u32(RATE_LOAD)
  if applied == rate and current ~= word and current ~= RATE_LOAD_ORIGINAL and not warned_other then
    warn("another code also writes the frame rate (" .. mem.hex(current) .. " at " ..
         mem.hex(RATE_LOAD) .. "): turn the 90/120 FPS Action Replay codes off")
    warned_other = true
  end
  mem.ensure_patches({{RATE_LOAD, word}, {NTSC_FALLBACK, word}})
  if applied ~= rate then
    set_vbi(rate)
    log("frame rate and VBI", rate, "Hz (headset", vr.refresh_rate(), "Hz)")
    applied = rate
  end
end

function on_frame()
  local rate = target_rate()
  if rate ~= applied or game.field() % 30 == 0 then
    update(rate)
  end
end

function on_state_loaded()
  -- The loaded RAM holds the code of the time it was saved.
  update(target_rate())
end

function on_unload()
  if applied then
    restore()
  end
end
