-- Metroid Prime Trilogy (NTSC-U, Wii): Echoes' scan and echo visors without their scan lines.
--
-- As in Echoes on the GameCube (G2ME01/smooth-scan-lines.lua): the scan visor's tint, its
-- highlight of the scannable objects and the echo visor's view are shaded through an 8x8 I4
-- texture of horizontal lines (two rows lit, two dark) that the game tiles over the screen. In VR
-- the lines sit at the same place in both eyes' images, so they float at their own distance in
-- front of the objects they cover, and they shimmer. The patch fills the texture with one even
-- level (by default the lines' average). Echoes loads it at 902C96C0 when it starts; the patch
-- writes it only while Echoes runs (the program recognized as in arm-cannon.lua) and only over the
-- lines or its own fill. Disabled, the lines come back.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Smooth scan lines",
  author = "DolphinXR",
  version = "1.0",
  description = "Echoes' scan visor and echo visor get an even tint instead of horizontal lines, "
    .. "which are hard on the eyes in VR. For Metroid Prime Trilogy NTSC-U.",
  games = {"R3ME01"},
  revision = 0,
  settings = {
    {name = "level", label = "Tint level", default = 8, min = 0, max = 15, step = 1,
     description = "How bright the tint is where the lines were: the lines alternate 15 and 0, "
       .. "and 8 keeps their average."},
  },
}

local TEXTURE = 0x902C96C0
-- I4: 4 bytes per row of 8 texels.
local LINES = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0}
-- Echoes' program: the instruction after its arm cannon hook (arm-cannon.lua's first site).
local ECHOES_CODE, ECHOES_WORD = 0x8018ACB4, 0x387C03B0

local function echoes_running()
  return mem.u32(ECHOES_CODE) == ECHOES_WORD
end

local function fill_word()
  return math.floor(settings.level + 0.5) * 0x11111111
end

local function holds_lines()
  for i, word in ipairs(LINES) do
    if mem.u32(TEXTURE + (i - 1) * 4) ~= word then
      return false
    end
  end
  return true
end

-- An even fill: every word the same repeated nibble (this patch's, whatever the level). Zeros
-- count only as the fill of level 0: before Echoes loads the texture, its memory can be blank, and
-- the patch must not write there.
local function holds_fill()
  local first = mem.u32(TEXTURE)
  if not first or first % 0x11111111 ~= 0 then
    return false
  end
  if first == 0 and fill_word() ~= 0 then
    return false
  end
  for i = 1, 7 do
    if mem.u32(TEXTURE + i * 4) ~= first then
      return false
    end
  end
  return true
end

local function write_all(words)
  for i, word in ipairs(words) do
    mem.write_u32(TEXTURE + (i - 1) * 4, word)
  end
end

local warned = false

local function apply()
  if not echoes_running() then
    return
  end
  local fill = fill_word()
  if mem.u32(TEXTURE) == fill and holds_fill() then
    return
  end
  if not holds_lines() and not holds_fill() then
    -- Blank: Echoes has not loaded the texture yet.
    if mem.u32(TEXTURE) == 0 then
      return
    end
    if not warned then
      warn("not applied: no scan line texture at " .. mem.hex(TEXTURE))
    end
    warned = true
    return
  end
  write_all({fill, fill, fill, fill, fill, fill, fill, fill})
end

function on_load()
  apply()
end

function on_state_loaded()
  apply()
end

function on_setting_changed(name, value)
  apply()
end

function on_frame()
  if game.field() % 30 == 0 then
    apply()
  end
end

function on_unload()
  if echoes_running() and holds_fill() then
    write_all(LINES)
  end
end
