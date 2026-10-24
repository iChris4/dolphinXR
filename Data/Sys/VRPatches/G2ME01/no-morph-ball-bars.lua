-- Metroid Prime 2: Echoes (NTSC-U): no black bars in morph ball.
--
-- In morph ball the game narrows its 3D view to a band 80% of the screen high: the player's view
-- height factor (player + 0x11E8) goes to 0.8, and the viewport setup scales the viewport height
-- and the vertical field of view by it. In VR the band leaves black bars above and below the view.
-- The patch turns the instruction of the viewport setup that reads the factor
-- (8003AF60, lfs f31, 0x11E8(r27)) into a load of the constant 1.0 the game keeps at r2 - 0x712C,
-- so the view stays full height. Whatever else reads the factor is unchanged.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "No bars in morph ball",
  author = "DolphinXR",
  version = "1.0",
  description = "The morph ball camera keeps the full view instead of a letterboxed band: no "
    .. "black bars above and below it in VR. For Metroid Prime 2: Echoes NTSC-U.",
  games = {"G2ME01"},
  revision = 0,
}

local SITE = 0x8003AF60
local ORIGINAL = 0xC3FB11E8     -- lfs f31, 0x11E8(r27): the view height factor
local FULL_HEIGHT = 0xC3E28ED4  -- lfs f31, -0x712C(r2)
local ONE = 0x804223C0 - 0x712C -- the constant that load reads: 1.0

local warned = false

local function install()
  local current = mem.u32(SITE)
  if current == FULL_HEIGHT then
    return
  end
  if current ~= ORIGINAL or mem.f32(ONE) ~= 1 then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(SITE) .. " (" .. mem.hex(current) ..
           "): is this Metroid Prime 2 NTSC-U?")
    end
    warned = true
    return
  end
  mem.write_code(SITE, FULL_HEIGHT)
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
  if mem.u32(SITE) == FULL_HEIGHT then
    mem.write_code(SITE, ORIGINAL)
  end
end
