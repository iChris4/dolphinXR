-- Metroid Prime 2: Echoes (NTSC-U): the scan visor's highlight is not drawn again in the scan box.
--
-- The renderer draws the highlight of the scannable objects (zz_80265d70, the scan visor's only
-- user) as two strips: one over the whole view, then one magnified inside the scan box, which the
-- box's magnified view covers. In VR the profile hides the box (Elements override "Scan visor:
-- box"), whose place is the centre of each eye's image, not where the head looks; the magnified
-- highlight was left alone there, a bright rectangle at a different place in each eye. The patch
-- branches over the second strip (80266348, its begin call, to 802663C8, after its end call).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "No scan box highlight",
  author = "DolphinXR",
  version = "1.0",
  description = "While scanning, no bright rectangle at the centre of each eye's view: the scan "
    .. "visor's highlight is not drawn a second time inside the scan box, which is hidden in "
    .. "VR. For Metroid Prime 2: Echoes NTSC-U.",
  games = {"G2ME01"},
  revision = 0,
}

local SITE = 0x80266348
local ORIGINAL = 0x48057651  -- bl 0x802BD998: begins the box's strip
local SKIP = 0x48000080      -- b 0x802663C8: past its end call
local END_CALL = 0x802663C4
local END_ORIGINAL = 0x480575D1  -- bl 0x802BD994

local warned = false

local function install()
  local current = mem.u32(SITE)
  if current == SKIP then
    return
  end
  if current ~= ORIGINAL or mem.u32(END_CALL) ~= END_ORIGINAL then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(SITE) .. " (" .. mem.hex(current) ..
           "): is this Metroid Prime 2 NTSC-U?")
    end
    warned = true
    return
  end
  mem.write_code(SITE, SKIP)
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
  if mem.u32(SITE) == SKIP then
    mem.write_code(SITE, ORIGINAL)
  end
end
