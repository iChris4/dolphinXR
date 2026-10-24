-- Metroid Prime 2: Echoes (NTSC-U): the scan and echo visors without their scan lines.
--
-- The scan visor's highlight of the scannable objects and the echo visor's view are shaded through
-- an 8x8 I4 texture of horizontal lines (two rows lit, two dark) that the game tiles over the
-- screen; it is loaded at boot at 806FE5A0. In VR the lines sit at the same place in both eyes'
-- images, so they float at their own distance in front of the objects they cover, and they
-- shimmer: hard on the eyes on a big object. The patch fills the texture with one even level (by
-- default the lines' average): the objects keep their colour and the scan's sweep. Disabled, the
-- lines come back.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Smooth scan lines",
  author = "DolphinXR",
  version = "1.0",
  description = "The scan visor's highlights and the echo visor's view get an even tint instead "
    .. "of horizontal lines, which are hard on the eyes in VR. For Metroid Prime 2: Echoes NTSC-U.",
  games = {"G2ME01"},
  revision = 0,
  settings = {
    {name = "level", label = "Tint level", default = 8, min = 0, max = 15, step = 1,
     description = "How bright the tint is where the lines were: the lines alternate 15 and 0, "
       .. "and 8 keeps their average."},
  },
}

local TEXTURE = 0x806FE5A0
-- I4: 4 bytes per row of 8 texels.
local LINES = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0}

local warned = false

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

-- An even fill: every word the same repeated nibble (this patch's, whatever the level).
local function holds_fill()
  local first = mem.u32(TEXTURE)
  if not first or first % 0x11111111 ~= 0 then
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

local function apply()
  local fill = fill_word()
  if mem.u32(TEXTURE) == fill and holds_fill() then
    return
  end
  if not holds_lines() and not holds_fill() then
    if not warned then
      warn("not applied: no scan line texture at " .. mem.hex(TEXTURE) ..
           ": is this Metroid Prime 2 NTSC-U?")
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
  if holds_fill() then
    write_all(LINES)
  end
end
