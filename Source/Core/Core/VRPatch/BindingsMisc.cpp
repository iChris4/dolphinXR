// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// asm.*, pad.* / wiimote.* / nunchuk.*, game.* and the log. See ScriptContext.h for the rules a
// binding follows.

#include <algorithm>
#include <cmath>
#include <limits>

#include <fmt/format.h>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/GekkoDisassembler.h"
#include "Core/VRPatch/ScriptContext.h"

namespace VRPatch
{
namespace
{
// ---- asm ----

// Assembles into context->words; false with context->text set on an error.
bool Assemble(ScriptContext* context, std::string_view text, u32 base)
{
  context->words.clear();
  const auto result = Common::GekkoAssembler::Assemble(text, base);
  if (Common::GekkoAssembler::IsFailure(result))
  {
    context->text = Common::GekkoAssembler::GetFailure(result).FormatError();
    return false;
  }
  const auto& blocks = Common::GekkoAssembler::GetT(result);
  u32 next = base;
  for (const auto& block : blocks)
  {
    if (block.instructions.empty())
      continue;
    if (block.block_address != next && !context->words.empty())
    {
      context->text = ".locate is not supported: assemble each block separately";
      return false;
    }
    next = block.block_address + static_cast<u32>(block.instructions.size());
    for (size_t i = 0; i + 3 < block.instructions.size(); i += 4)
    {
      context->words.push_back(static_cast<u32>(block.instructions[i]) << 24 |
                               static_cast<u32>(block.instructions[i + 1]) << 16 |
                               static_cast<u32>(block.instructions[i + 2]) << 8 |
                               static_cast<u32>(block.instructions[i + 3]));
    }
  }
  return true;
}

// asm.assemble(text, base) -> {words} or nil, error. Gekko assembly with the extended mnemonics;
// branch targets are labels, displacements, or absolute addresses in backquotes (b `0x80040FF0`).
int AsmAssemble(lua_State* L)
{
  size_t length = 0;
  const char* text = luaL_checklstring(L, 1, &length);
  const lua_Integer base = luaL_checkinteger(L, 2);
  luaL_argcheck(L, base >= 0 && base <= 0xFFFFFFFF && base % 4 == 0, 2,
                "base must be a word-aligned address");
  ScriptContext* context = GetContext(L);
  if (!Assemble(context, std::string_view(text, length), static_cast<u32>(base)))
  {
    lua_pushnil(L);
    lua_pushlstring(L, context->text.data(), context->text.size());
    return 2;
  }
  lua_createtable(L, static_cast<int>(context->words.size()), 0);
  for (size_t i = 0; i < context->words.size(); ++i)
  {
    lua_pushinteger(L, context->words[i]);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

// asm.branch(from, to, link) -> word: b / bl from `from` to `to`.
int AsmBranch(lua_State* L)
{
  const lua_Integer from = luaL_checkinteger(L, 1);
  const lua_Integer to = luaL_checkinteger(L, 2);
  const bool link = lua_toboolean(L, 3);
  const lua_Integer offset = to - from;
  luaL_argcheck(L, (from & 3) == 0 && (to & 3) == 0, 2, "addresses must be word-aligned");
  luaL_argcheck(L, offset >= -0x02000000 && offset < 0x02000000, 2,
                "the target is more than 32 MiB away");
  const u32 word = 0x48000000u | (static_cast<u32>(offset) & 0x03FFFFFCu) | (link ? 1u : 0u);
  lua_pushinteger(L, word);
  return 1;
}

// asm.disassemble(word, address) -> "lwz r3, 0x0740 (r28)".
int AsmDisassemble(lua_State* L)
{
  const lua_Integer word = luaL_checkinteger(L, 1);
  const lua_Integer address = luaL_optinteger(L, 2, 0);
  ScriptContext* context = GetContext(L);
  context->text =
      Common::GekkoDisassembler::Disassemble(static_cast<u32>(word), static_cast<u32>(address));
  lua_pushlstring(L, context->text.data(), context->text.size());
  return 1;
}

// ---- pad / wiimote / nunchuk ----

PadDevice UpvalueDevice(lua_State* L)
{
  return static_cast<PadDevice>(lua_tointeger(L, lua_upvalueindex(1)));
}

u8 CheckPort(lua_State* L, int arg)
{
  const lua_Integer port = luaL_checkinteger(L, arg);
  luaL_argcheck(L, port >= 1 && port <= 4, arg, "ports are 1 to 4");
  return static_cast<u8>(port - 1);
}

const char* DeviceName(PadDevice device)
{
  switch (device)
  {
  case PadDevice::GCPad:
    return "pad";
  case PadDevice::Wiimote:
    return "wiimote";
  case PadDevice::Attachment:
  default:
    return "nunchuk";
  }
}

// The controls a name resolves to, cached; nullptr with context->text set when it does not
// resolve (logged once per name).
const std::vector<PadControl>* Resolve(ScriptContext* context, PadDevice device, u8 port,
                                       PadControlKind kind, std::string_view name)
{
  auto key = std::make_tuple(device, port, kind, std::string(name));
  auto it = context->pad_names.find(key);
  if (it == context->pad_names.end())
  {
    std::vector<PadControl> controls;
    std::string error = "controller input is not available here";
    if (context->env && context->env->pad)
      controls = context->env->pad->Resolve(device, port, kind, name, &error);
    if (controls.empty())
    {
      AppendLog(
          context, 1,
          fmt::format("{}: {} {} \"{}\": {}", DeviceName(device), "port", port + 1, name, error));
    }
    it = context->pad_names.emplace(std::move(key), std::move(controls)).first;
  }
  if (it->second.empty())
  {
    context->text =
        fmt::format("\"{}\" is not a control of {} port {}", name, DeviceName(device), port + 1);
    return nullptr;
  }
  return &it->second;
}

// Holds (or with `release`, releases) the resolved controls at the given values.
bool SetHold(ScriptContext* context, PadDevice device, u8 port, PadControlKind kind,
             std::string_view name, double first, double second, bool release)
{
  const std::vector<PadControl>* controls = Resolve(context, device, port, kind, name);
  if (!controls)
    return false;
  bool changed = false;
  for (size_t i = 0; i < controls->size(); ++i)
  {
    PadHoldKey key{device, port, (*controls)[i].group, (*controls)[i].control};
    if (release)
    {
      changed |= context->pad_holds.erase(key) != 0;
      continue;
    }
    // Sticks and the pointer: X then Y. Buttons and triggers: every control gets the value.
    const bool two_axes = kind == PadControlKind::Stick || kind == PadControlKind::Pointer;
    const double value = two_axes && i == 1 ? second : first;
    auto [it, inserted] = context->pad_holds.try_emplace(std::move(key), value);
    // A hidden pointer (NaN) held again is no change.
    const bool same = it->second == value || (std::isnan(it->second) && std::isnan(value));
    if (inserted || !same)
    {
      it->second = value;
      changed = true;
    }
  }
  if (changed)
    ++context->pad_holds_generation;
  return true;
}

void ClearHolds(ScriptContext* context, PadDevice device, u8 port)
{
  const size_t erased = std::erase_if(context->pad_holds, [&](const auto& entry) {
    return std::get<0>(entry.first) == device && std::get<1>(entry.first) == port;
  });
  if (erased != 0)
    ++context->pad_holds_generation;
}

int PushHoldResult(lua_State* L, bool ok)
{
  if (ok)
  {
    lua_pushboolean(L, true);
    return 1;
  }
  const ScriptContext* context = GetContext(L);
  lua_pushboolean(L, false);
  lua_pushlstring(L, context->text.data(), context->text.size());
  return 2;
}

// pad.set_button(port, name, pressed): holds a button pressed; false stops holding it (the user's
// own input comes back).
int PadSetButton(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  size_t length = 0;
  const char* name = luaL_checklstring(L, 2, &length);
  const bool pressed = lua_isnone(L, 3) || lua_toboolean(L, 3);
  return PushHoldResult(L, SetHold(GetContext(L), device, port, PadControlKind::Button,
                                   std::string_view(name, length), 1.0, 0.0, !pressed));
}

// pad.set_value(port, name, value): forces a button or trigger to a raw value (0 = released even
// when the user presses it).
int PadSetValue(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  size_t length = 0;
  const char* name = luaL_checklstring(L, 2, &length);
  const double value = std::clamp(luaL_checknumber(L, 3), 0.0, 1.0);
  return PushHoldResult(L, SetHold(GetContext(L), device, port, PadControlKind::Button,
                                   std::string_view(name, length), value, 0.0, false));
}

// pad.set_trigger(port, name, value 0-1): an analog trigger ("L-Analog", "R-Analog").
int PadSetTrigger(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  size_t length = 0;
  const char* name = luaL_checklstring(L, 2, &length);
  const double value = std::clamp(luaL_checknumber(L, 3), 0.0, 1.0);
  return PushHoldResult(L, SetHold(GetContext(L), device, port, PadControlKind::Analog,
                                   std::string_view(name, length), value, 0.0, false));
}

// pad.set_stick(port, name, x, y): a stick ("Main Stick", "C-Stick", "Stick"; "" = the first one)
// at -1..1.
int PadSetStick(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  size_t length = 0;
  const char* name = luaL_optlstring(L, 2, "", &length);
  const double x = luaL_checknumber(L, 3);
  const double y = luaL_checknumber(L, 4);
  const double cx = std::isfinite(x) ? std::clamp(x, -1.0, 1.0) : 0.0;
  const double cy = std::isfinite(y) ? std::clamp(y, -1.0, 1.0) : 0.0;
  return PushHoldResult(L, SetHold(GetContext(L), device, port, PadControlKind::Stick,
                                   std::string_view(name, length), cx, cy, false));
}

// The pointer group's name, for set_pointer and release.
constexpr std::string_view POINTER_NAME = "IR";

// wiimote.set_pointer(port, x, y): the Wii Remote's pointer at -1..1 (x right, y up); without x,
// off the screen (the game sees no sensor bar).
int PadSetPointer(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  double x = std::numeric_limits<double>::quiet_NaN();
  double y = x;
  if (!lua_isnoneornil(L, 2))
  {
    const double raw_x = luaL_checknumber(L, 2);
    const double raw_y = luaL_checknumber(L, 3);
    x = std::isfinite(raw_x) ? std::clamp(raw_x, -1.0, 1.0) : 0.0;
    y = std::isfinite(raw_y) ? std::clamp(raw_y, -1.0, 1.0) : 0.0;
  }
  return PushHoldResult(
      L, SetHold(GetContext(L), device, port, PadControlKind::Pointer, POINTER_NAME, x, y, false));
}

// Releases a name the patch held before, whatever its kind.
bool ReleaseName(ScriptContext* context, PadDevice device, u8 port, std::string_view name)
{
  bool released = false;
  for (const PadControlKind kind : {PadControlKind::Button, PadControlKind::Analog,
                                    PadControlKind::Stick, PadControlKind::Pointer})
  {
    // Only the kinds the name was resolved as, so the other kinds are not logged as unknown.
    const auto it = context->pad_names.find(std::make_tuple(device, port, kind, std::string(name)));
    if (it != context->pad_names.end() && !it->second.empty())
      released |= SetHold(context, device, port, kind, name, 0.0, 0.0, true);
  }
  return released;
}

// pad.release(port, name): stops holding a button, trigger or stick.
int PadRelease(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  size_t length = 0;
  const char* name = luaL_checklstring(L, 2, &length);
  lua_pushboolean(L, ReleaseName(GetContext(L), device, port, std::string_view(name, length)));
  return 1;
}

// pad.clear(port): stops holding everything on that controller.
int PadClear(lua_State* L)
{
  const PadDevice device = UpvalueDevice(L);
  const u8 port = CheckPort(L, 1);
  ClearHolds(GetContext(L), device, port);
  return 0;
}

void RegisterPadTable(lua_State* L, const char* name, PadDevice device)
{
  static constexpr luaL_Reg FUNCTIONS[] = {
      {"set_button", PadSetButton},
      {"set_value", PadSetValue},
      {"set_trigger", PadSetTrigger},
      {"set_stick", PadSetStick},
      {"set_pointer", PadSetPointer},
      {"release", PadRelease},
      {"clear", PadClear},
      {nullptr, nullptr},
  };
  lua_newtable(L);
  lua_pushinteger(L, static_cast<lua_Integer>(device));
  luaL_setfuncs(L, FUNCTIONS, 1);
  lua_setglobal(L, name);
}

// ---- game ----

const ScriptEnvironment* Env(lua_State* L)
{
  return GetContext(L)->env;
}

int GameId(lua_State* L)
{
  const ScriptEnvironment* env = Env(L);
  if (!env)
    lua_pushliteral(L, "");
  else
    lua_pushlstring(L, env->game_id.data(), env->game_id.size());
  return 1;
}

int GameRevision(lua_State* L)
{
  const ScriptEnvironment* env = Env(L);
  lua_pushinteger(L, env ? env->revision : 0);
  return 1;
}

int GameField(lua_State* L)
{
  const ScriptEnvironment* env = Env(L);
  lua_pushinteger(L, static_cast<lua_Integer>(env ? env->field : 0));
  return 1;
}

int GameIsWii(lua_State* L)
{
  const ScriptEnvironment* env = Env(L);
  lua_pushboolean(L, env && env->is_wii);
  return 1;
}

// __vrpatch_log(level, text)
int Log(lua_State* L)
{
  const lua_Integer level = luaL_checkinteger(L, 1);
  size_t length = 0;
  const char* text = luaL_checklstring(L, 2, &length);
  AppendLog(GetContext(L), static_cast<u8>(std::clamp<lua_Integer>(level, 0, 2)),
            std::string_view(text, length));
  return 0;
}
}  // namespace

void RegisterMiscBindings(lua_State* L)
{
  static constexpr luaL_Reg ASM[] = {
      {"assemble", AsmAssemble},
      {"branch", AsmBranch},
      {"disassemble", AsmDisassemble},
      {nullptr, nullptr},
  };
  luaL_newlib(L, ASM);
  lua_setglobal(L, "asm");

  RegisterPadTable(L, "pad", PadDevice::GCPad);
  RegisterPadTable(L, "wiimote", PadDevice::Wiimote);
  RegisterPadTable(L, "nunchuk", PadDevice::Attachment);

  static constexpr luaL_Reg GAME[] = {
      {"id", GameId},        {"revision", GameRevision}, {"field", GameField},
      {"is_wii", GameIsWii}, {nullptr, nullptr},
  };
  luaL_newlib(L, GAME);
  lua_setglobal(L, "game");

  lua_pushcfunction(L, Log);
  lua_setglobal(L, "__vrpatch_log");
}
}  // namespace VRPatch
