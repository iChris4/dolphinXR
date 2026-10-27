-- Metroid Prime Trilogy (NTSC-U, Wii): no bright disk at Echoes' scan cursor.
--
-- Echoes' scan visor draws its tint (zz_803ae8b4, run only by the scan visor) as a quad over the
-- whole view, then a fan of 40 vertices, a disk around the pointer, through the same shader: a
-- brighter spot where the lens is. In VR the profile puts the lens on the virtual screen, while
-- the tint covers each eye's whole view, so the disk lands at the pointer's place in each eye's
-- image, away from the lens. The patch branches over the fan (803AEEC8, its begin call, to
-- 803AEFFC, after its end call); the tint stays. It writes only while Echoes runs (the program
-- recognized as in arm-cannon.lua). Disabled, the disk comes back.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "No scan spotlight",
  author = "DolphinXR",
  version = "1.0",
  description = "Echoes' scan visor without the bright disk around the pointer, which lands away "
    .. "from the scan lens in VR. For Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
}

local SITE = 0x803AEEC8
local ORIGINAL = 0x4BF59489      -- bl 0x80308350: begins the fan (GX_TRIANGLEFAN, 40 vertices)
local END_CALL = 0x803AEFF8
local END_ORIGINAL = 0x4BF593C1  -- bl 0x803083B8: ends it
local SKIP = asm.branch(SITE, END_CALL + 4)
-- Echoes' program: the instruction after its arm cannon hook (arm-cannon.lua's first site).
local ECHOES_CODE, ECHOES_WORD = 0x8018ACB4, 0x387C03B0

local warned = false

local function install()
  if mem.u32(ECHOES_CODE) ~= ECHOES_WORD then
    return
  end
  local current = mem.u32(SITE)
  if current == SKIP then
    return
  end
  if current ~= ORIGINAL or mem.u32(END_CALL) ~= END_ORIGINAL then
    if not warned then
      warn("not installed: unexpected code at " .. mem.hex(SITE) .. " (" .. mem.hex(current or 0) ..
           ")")
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
  -- Echoes may have started from the disc's menu: its program comes back unpatched.
  if game.field() % 30 == 0 then
    install()
  end
end

function on_unload()
  if mem.u32(SITE) == SKIP then
    mem.write_code(SITE, ORIGINAL)
  end
end
