-- Metroid Prime 2: Echoes (NTSC-U): no black bars in cutscenes.
--
-- Cutscenes letterbox the 3D view with camera filters: the viewport setup takes the smallest
-- height factor of the state manager's 11 filters (a letterbox filter eases to 0.8) and scales the
-- viewport height and the vertical field of view by it, a 640x360 band in VR with black bars
-- above and below. The patch drops the instruction that keeps the smaller factor
-- (8003AFA4, fmr f31, f1), so the view keeps the player's own height factor (full height, or the
-- morph ball's, see no-morph-ball-bars). The game still draws its bar quads: the profile's
-- Elements entry "Cinematic bars" hides them.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "No bars in cutscenes",
  author = "DolphinXR",
  version = "1.0",
  description = "Cutscenes keep the full view instead of a letterboxed band: no black bars above "
    .. "and below them in VR. For Metroid Prime 2: Echoes NTSC-U.",
  games = {"G2ME01"},
  revision = 0,
}

local SITE = 0x8003AFA4
local ORIGINAL = 0xFFE00890  -- fmr f31, f1: keeps a filter's smaller height factor
local NOP = 0x60000000
local COMPARE = 0x8003AF9C
local COMPARE_ORIGINAL = 0xFC01F840  -- fcmpo cr0, f1, f31

local warned = false

local function install()
  local current = mem.u32(SITE)
  if current == NOP then
    return
  end
  if current ~= ORIGINAL or mem.u32(COMPARE) ~= COMPARE_ORIGINAL then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(SITE) .. " (" .. mem.hex(current) ..
           "): is this Metroid Prime 2 NTSC-U?")
    end
    warned = true
    return
  end
  mem.write_code(SITE, NOP)
  log("installed at", mem.hex(SITE))
end

function on_load()
  install()
end

function on_state_loaded()
  install()
end

function on_frame()
  if game.field() % 60 == 0 then
    install()
  end
end

function on_unload()
  if mem.u32(SITE) == NOP then
    mem.write_code(SITE, ORIGINAL)
  end
end
