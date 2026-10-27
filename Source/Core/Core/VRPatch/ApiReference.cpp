// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <string_view>

#include "Core/VRPatch/PatchScript.h"

namespace VRPatch
{
namespace
{
constexpr std::string_view FORMAT = R"(DolphinXR VR game patches (Lua 5.4)

A patch is one .lua file: a `patch` table (the manifest) and functions the engine calls on the CPU
thread. Files: Sys/VRPatches/<GameID or prefix>/<id>.lua (shipped) and
User/VRPatches/<GameID or prefix>/<id>.lua (the user's; same id wins over Sys). The folder is the
game ID, its first 3 letters (all regions) or GameIDr<revision>. The id is the file name.
Enabled per game in GameSettingsVR/<GameID>.ini, section [VRPatch]: `<id> = True`, settings as
`<id>/<name> = <value>`. Patches never run with netplay, a movie (TAS) or RetroAchievements
hardcore mode.

patch = {
  title = "Arm cannon follows the right hand",     -- required
  author = "...", description = "...", version = "1.0",
  games = {"GM8E01"},          -- game ID prefixes it is for (optional)
  revision = 0,                -- only this disc revision (optional)
  settings = {                 -- shown in the UIs, read as settings.<name>
    {name = "hand", type = "choice", options = {"Left", "Right"}, default = 1},
    {name = "pitch", label = "Pitch offset", unit = "deg", default = 0, min = -90, max = 90, step = 1},
    {name = "beams", type = "bool", default = true, description = "..."},
  },
  scratch = 0x100,             -- bytes of free memory for the patch: mem.scratch (16-aligned)
  reserve = {{0x80002300, 0x80002400}},  -- fixed addresses it writes (caves): conflict checks, and
                                          -- the low memory it may write besides its scratch
  gecko = {"C2xxxxxx 00000002", "..."},  -- Gecko lines installed while the patch runs
  conflicts = {texture = {"Cannon"}},     -- VR overrides (shader/elements/texture names) turned
                                          -- off while it runs (a visual anchor it replaces)
}

function on_load() end          -- first field with memory after the patch starts (and after a
                                -- live reload): install code caves here
function on_frame() end         -- every VI field (60 or 50 per second), before the Gecko codes
function on_state_loaded() end  -- after any savestate load (RAM was replaced: reinstall caves)
function on_setting_changed(name, value) end
function on_unload() end        -- disabled or game stopped: restore the game's code; mem.* reads
                                -- return nil when the emulated memory is already gone

The top level runs once when the patch loads: define tables and functions there, do not touch
memory. Limits per call: 40 ms, 50 million instructions, 32 MiB. An error is logged; three in a
row stop the patch (its log keeps the message). No io, os, require, load or dofile.
)";

constexpr std::string_view MEMORY = R"(
mem (emulated memory, big-endian, RAM only; reads return nil when the address is not RAM)
  mem.u8/u16/u32/s8/s16/s32/f32/f64(address)        -> number or nil
  mem.write_u8/u16/u32/f32/f64(address, value)       -> true/false (floats must be finite)
  mem.f32s(address, n) -> n numbers; mem.write_f32s(address, ...)
  mem.vec3(address) / mem.write_vec3(address, v)     -- 3 floats
  mem.mat33(address) / mem.write_mat33(address, m)   -- 9 floats, row-major
  mem.mat34(address) / mem.write_mat34(address, m)   -- 12 floats, row-major (GameCube Mtx)
  mem.valid(address [, size])
  mem.ptr(address, o1, ..., on)    -> the pointer at address, then the pointer at p + o1, ...;
                                      nil when one read fails or is not a RAM address
  mem.chain(address, o1, ..., on)  -> [[address] + o1] + ... + on (an address, Cheat Engine style)
  mem.read_words(address, n)       -> {words}
  mem.write_code(address, word | {words})   -- writes code and drops the JIT blocks over it
  mem.ensure_code(address, word | {words})  -> changed: rewrites only differing words (use it every
                                               few fields to keep caves installed); nil on failure
  mem.ensure_patches({{address, word}, ...}) -> changed
  mem.scratch, mem.scratch_size   -- the patch's scratch block (0 when the manifest asks none)
  mem.hex(value) -> "8045A1A8"
  A write below 0x80003100 (the OS globals, exception vectors and Gecko code handler; also through
  0xC0000000) is an error unless it is inside mem.scratch .. mem.scratch + mem.scratch_size or a
  reserve range: check that an assembled cave fits its scratch before writing it.

asm
  asm.assemble(text, base) -> {words} or nil, error   -- Gekko assembly, extended mnemonics,
                              labels. A bare number after b/bl/bc is a displacement; write an
                              absolute target in backquotes: "b `0x80040FF0`"
  asm.branch(from, to [, link]) -> the b / bl word
  asm.disassemble(word [, address]) -> text
)";

constexpr std::string_view VR = R"(
vr (OpenXR reference space: meters, right-handed, +Y up, -Z forward; hand = "left"/"right"/0/1)
  vr.head([kind]) -> pose or nil   -- kind "view": the center of the eyes relative to the game
                                      camera as DolphinXR renders them (the axes of a hand's
                                      "view"), e.g. to cast a ray from the eyes through a point
                                      of the virtual screen or to cull for where they look (with
                                      the tracking mode none, the eyes keep the home
                                      orientation); nil in flat-screen mode
  vr.hand(hand [, kind]) -> pose or nil   -- kind: "aim" (default, predicted to the display time),
                                             "grip", "pointer" (aim sampled now), or "view": the
                                             aim relative to the game camera as DolphinXR renders
                                             it (recenter, camera height, Camera Anchor), meters,
                                             +X right +Y up -Z forward of the camera; nil in
                                             flat-screen mode. Use "view" to place an object in
                                             the hand. "level_view": the same relative to the
                                             camera's level frame, without the pitch the VR view
                                             removes (vr.set_camera_pitch_source).
  vr.hand_in_head_space(hand [, kind]) -> pose relative to the head
  vr.raw_pose(hand | "head" [, kind]) -> valid, px, py, pz, qx, qy, qz, qw   -- the same as plain
                                             numbers (valid false when there is none): no tables
  vr.button(hand, name) -> bool   -- primary (A/X), secondary (B/Y), button_3, button_4, menu,
                                     shoulder, trigger, squeeze (grip), thumbstick (click)
  vr.axis(hand, name) -> number   -- trigger, squeeze, squeeze_force, thumbstick_x, thumbstick_y
  vr.connected(hand), vr.active(), vr.focused()
  vr.menu_open()  -- DolphinXR's VR menu is open: buttons read released and poses frozen
  vr.velocity(hand) -> linear, angular (vec3 or nil; m/s and rad/s)
  vr.screen_hit(hand) -> u, v, distance or nil (aim ray on the virtual screen, -1..1)
  vr.units_per_meter()   -- the profile's world scale (game units per meter)
  vr.haptic(hand, amplitude 0-1 [, seconds])
  vr.setting(key) -> string; vr.set_setting(key, value)   -- the [Graphics.VR] keys, this session
  vr.set_flag(name [, active]); vr.clear_flags()   -- override flags (condition=<name> in the
      game's Shader, Elements and Texture overrides): raised until lowered with false or the
      patch stops; the overrides see a change from the next video frame. Names: 1 to 64 letters,
      digits, '_', '.', '-'. For the game's state read from memory (a visor, a menu).
  vr.flag(name) -> bool   -- whether a running patch raised the flag, as of the end of the last
      field's hooks (a patch's own change shows from the next field). For patches that share a
      control: e.g. a gesture raises a flag while it uses a thumbstick, and a movement patch
      leaves that thumbstick alone meanwhile.
  vr.set_camera_pitch_source(address | nil)   -- the f32 at address is the sine of the game
      camera's pitch (+ up), e.g. the up component of its forward axis in its matrix. The VR view
      removes that pitch from each frame (read when the frame starts drawing, so it matches the
      frame), while the virtual screen and the Wii pointer turn with the game camera: a patch can
      let the game tilt its camera (as the Wii pointer does at the screen's edges) without the
      world moving. Until nil or the patch stops. vr.camera_pitch_source() -> the address the
      running patches named, or nil. raw_pose(hand, "level_view") is the hand relative to the
      game camera's level frame (its pitch removed): for an object placed from the camera at the
      time the game draws, rebuild the level frame from the camera's right axis.
  vr.keep_pointer_on_screen(keep)   -- true: an aim past the virtual screen's edge holds the Wii
      pointer at that edge, and an aim that misses the screen keeps the last position, instead of
      losing the sensor bar; the remote's tilt in its accelerometer stays within +-40 degrees, as
      when aiming at a TV. For games that turn the camera on their own while the pointer is lost,
      or stop reading it from a remote held far up (Metroid Prime 3: past 45 degrees). Until false
      or the patch stops.

pad / wiimote / nunchuk (emulated controllers, port 1-4; held until changed or released, under the
agent's press_input; names as in Dolphin's controller settings, "Group/Control" when ambiguous)
  pad.set_button(port, name [, pressed])  -- pressed=false stops holding (the user's input returns)
  pad.set_value(port, name, value 0-1)    -- forces a value (0 = released even if the user presses)
  pad.set_trigger(port, name, value 0-1)  -- "L-Analog", "R-Analog"
  pad.set_stick(port, name, x, y)         -- "Main Stick", "C-Stick" ("" = the first stick), -1..1
  wiimote.set_pointer(port, x, y)         -- the IR pointer, -1..1 (x right, y up) as the VR
                                             pointer sets it; no x: off the screen.
                                             wiimote.release(port, "IR") gives it back
  pad.release(port, name), pad.clear(port)
  All return true, or false and a message.

game: game.id(), game.revision(), game.field() (VI fields since boot), game.is_wii()
log(...), warn(...), print(...)   -- the patch log (get_vr_patch_log, the UIs) and Dolphin's log
settings.<name>                   -- the current setting values
)";

constexpr std::string_view MATH = R"(
Math (pure Lua, globals). Each vec3, quat, mat33, mat34 or pose is a table: fine in on_load or
for an occasional value, but a few dozen per field keep the garbage collector busy on the Quest.
In on_frame prefer plain numbers: vr.raw_pose, mem.f32s, mem.write_f32s, tables reused in place
(the arm cannon patches of GM8E01 and G2ME01 do; that took them from about 180 to 60 us per field
on a Quest 3).
  vec3(x, y, z): + - * (scalar or per component) / unary -, :dot :cross :length :distance
    :normalized :copy :unpack; vec3.lerp(a, b, t), vec3.zero()
  quat(x, y, z, w): q * q, q * v (rotates), :conjugate :inverse :normalized :angle_to(q)
    :to_yaw_pitch_roll() (degrees); quat.identity(), quat.axis_angle(axis, deg),
    quat.axis_x/axis_y/axis_z(deg), quat.from_euler(x, y, z [, order "xyz"]), quat.slerp(a, b, t)
  mat33(9 numbers | table): m * m, m * v, :row(i) :column(j) (0-based) :get(r, c) :transpose
    :inverse :determinant :to_quat :is_orthonormal(tol) :unpack; mat33.identity(),
    mat33.from_quat(q), mat33.from_rows(a, b, c), mat33.from_columns(a, b, c)
  mat34(12 numbers | table): m * m, m * v (point), :rotate(v) :transform(v) :basis() :translation()
    :with_translation(v) :with_basis(m33) :row :column :inverse :looks_like_transform() :unpack;
    mat34.identity(), mat34.from_basis(m33, t), mat34.from_quat(q, t)
  pose(pos, rot): p * p, p * v, :inverse :matrix (mat34) :forward :up :right
  smoother(keep [, snap]):update(number | vec3 | quat | mat34), :reset()
    -- new = old * keep + value * (1 - keep); a jump over `snap` is not smoothed
  clamp(x, lo, hi), lerp(a, b, t), wrap180(degrees)

Typical patch (an object follows a controller):
  local smooth = smoother(0.24, 2.0)
  function on_load() mem.ensure_patches(CAVE) end
  function on_state_loaded() mem.ensure_patches(CAVE) end
  function on_frame()
    if game.field() % 60 == 0 then mem.ensure_patches(CAVE) end
    local player = mem.ptr(0x8045A1A8 + 0x84C)
    local hand = vr.hand(settings.hand)
    if not player or not hand then mem.write_u32(mem.scratch, 0) return end
    local m = smooth:update(to_game_axes(hand))   -- the game's axes and units
    mem.write_mat34(mem.scratch + 4, m)
    mem.write_u32(mem.scratch, player)            -- arms the cave for this object
  end
  function on_unload() mem.write_code(HOOK_ADDRESS, ORIGINAL_WORD) end
)";

const std::string& Joined()
{
  static const std::string text =
      std::string(FORMAT) + std::string(MEMORY) + std::string(VR) + std::string(MATH);
  return text;
}
}  // namespace

std::string_view GetApiReference()
{
  return Joined();
}
}  // namespace VRPatch
