// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// mem.*: emulated memory. See ScriptContext.h for the rules a binding follows.

#include <bit>
#include <cmath>
#include <optional>

#include "Core/VRPatch/MemoryAccess.h"
#include "Core/VRPatch/ScriptContext.h"

namespace VRPatch
{
namespace
{
constexpr int MAX_FLOATS = 64;
constexpr lua_Integer MAX_WORDS = 4096;

MemoryAccess* Memory(lua_State* L)
{
  const ScriptContext* context = GetContext(L);
  return context->env ? context->env->memory : nullptr;
}

u32 CheckAddress(lua_State* L, int arg)
{
  const lua_Integer value = luaL_checkinteger(L, arg);
  if (value < 0 || value > 0xFFFFFFFF)
    luaL_argerror(L, arg, "not a 32-bit address");
  return static_cast<u32>(value);
}

// A value to write: an integer (negative values wrap) or an integral float.
u64 CheckWord(lua_State* L, int arg)
{
  return static_cast<u64>(luaL_checkinteger(L, arg));
}

void WarnNonFinite(lua_State* L)
{
  ScriptContext* context = GetContext(L);
  if (context->warned_non_finite)
    return;
  context->warned_non_finite = true;
  AppendLog(context, 1,
            "a float write with NaN or infinity was refused (warned once per patch load)");
}

template <typename T, typename Read>
int PushRead(lua_State* L, Read read)
{
  const u32 address = CheckAddress(L, 1);
  MemoryAccess* memory = Memory(L);
  const std::optional<T> value = memory ? read(*memory, address) : std::nullopt;
  if (!value)
  {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(*value));
  return 1;
}

int MemU8(lua_State* L)
{
  return PushRead<u8>(L, [](const MemoryAccess& m, u32 a) { return m.Read8(a); });
}

int MemU16(lua_State* L)
{
  return PushRead<u16>(L, [](const MemoryAccess& m, u32 a) { return m.Read16(a); });
}

int MemU32(lua_State* L)
{
  return PushRead<u32>(L, [](const MemoryAccess& m, u32 a) { return m.Read32(a); });
}

int MemS8(lua_State* L)
{
  return PushRead<s8>(L, [](const MemoryAccess& m, u32 a) -> std::optional<s8> {
    const auto value = m.Read8(a);
    return value ? std::optional<s8>(static_cast<s8>(*value)) : std::nullopt;
  });
}

int MemS16(lua_State* L)
{
  return PushRead<s16>(L, [](const MemoryAccess& m, u32 a) -> std::optional<s16> {
    const auto value = m.Read16(a);
    return value ? std::optional<s16>(static_cast<s16>(*value)) : std::nullopt;
  });
}

int MemS32(lua_State* L)
{
  return PushRead<s32>(L, [](const MemoryAccess& m, u32 a) -> std::optional<s32> {
    const auto value = m.Read32(a);
    return value ? std::optional<s32>(static_cast<s32>(*value)) : std::nullopt;
  });
}

int MemF32(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  MemoryAccess* memory = Memory(L);
  const std::optional<u32> value = memory ? memory->Read32(address) : std::nullopt;
  if (!value)
    lua_pushnil(L);
  else
    lua_pushnumber(L, std::bit_cast<float>(*value));
  return 1;
}

int MemF64(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  MemoryAccess* memory = Memory(L);
  const std::optional<u64> value = memory ? memory->Read64(address) : std::nullopt;
  if (!value)
    lua_pushnil(L);
  else
    lua_pushnumber(L, std::bit_cast<double>(*value));
  return 1;
}

// mem.f32s(address, count): `count` consecutive floats, or nil when one cannot be read.
int MemF32s(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const lua_Integer count = luaL_checkinteger(L, 2);
  luaL_argcheck(L, count >= 1 && count <= MAX_FLOATS, 2, "1 to 64 floats");
  MemoryAccess* memory = Memory(L);
  float values[MAX_FLOATS];
  for (lua_Integer i = 0; i < count; ++i)
  {
    const std::optional<u32> value =
        memory ? memory->Read32(address + static_cast<u32>(i) * 4) : std::nullopt;
    if (!value)
    {
      lua_pushnil(L);
      return 1;
    }
    values[i] = std::bit_cast<float>(*value);
  }
  luaL_checkstack(L, static_cast<int>(count), "too many floats");
  for (lua_Integer i = 0; i < count; ++i)
    lua_pushnumber(L, values[i]);
  return static_cast<int>(count);
}

template <int Bytes>
int MemWrite(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const u64 value = CheckWord(L, 2);
  MemoryAccess* memory = Memory(L);
  bool ok = false;
  if (memory)
  {
    if constexpr (Bytes == 1)
      ok = memory->Write8(address, static_cast<u8>(value));
    else if constexpr (Bytes == 2)
      ok = memory->Write16(address, static_cast<u16>(value));
    else
      ok = memory->Write32(address, static_cast<u32>(value));
  }
  lua_pushboolean(L, ok);
  return 1;
}

int MemWriteF32(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const double value = luaL_checknumber(L, 2);
  if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
  {
    WarnNonFinite(L);
    lua_pushboolean(L, false);
    return 1;
  }
  MemoryAccess* memory = Memory(L);
  const bool ok = memory && memory->Write32(address, std::bit_cast<u32>(static_cast<float>(value)));
  lua_pushboolean(L, ok);
  return 1;
}

int MemWriteF64(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const double value = luaL_checknumber(L, 2);
  if (!std::isfinite(value))
  {
    WarnNonFinite(L);
    lua_pushboolean(L, false);
    return 1;
  }
  MemoryAccess* memory = Memory(L);
  const bool ok = memory && memory->Write64(address, std::bit_cast<u64>(value));
  lua_pushboolean(L, ok);
  return 1;
}

// mem.write_f32s(address, ...): consecutive floats; nothing is written when one is not finite.
int MemWriteF32s(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const int count = lua_gettop(L) - 1;
  luaL_argcheck(L, count >= 1 && count <= MAX_FLOATS, 2, "1 to 64 floats");
  float values[MAX_FLOATS];
  for (int i = 0; i < count; ++i)
  {
    const double value = luaL_checknumber(L, i + 2);
    values[i] = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(values[i]))
    {
      WarnNonFinite(L);
      lua_pushboolean(L, false);
      return 1;
    }
  }
  MemoryAccess* memory = Memory(L);
  bool ok = memory != nullptr && memory->IsRAM(address, static_cast<u32>(count) * 4);
  for (int i = 0; ok && i < count; ++i)
    ok = memory->Write32(address + static_cast<u32>(i) * 4, std::bit_cast<u32>(values[i]));
  lua_pushboolean(L, ok);
  return 1;
}

int MemValid(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const lua_Integer size = luaL_optinteger(L, 2, 1);
  luaL_argcheck(L, size >= 1 && size <= 0x01000000, 2, "size must be 1 to 16 MiB");
  MemoryAccess* memory = Memory(L);
  lua_pushboolean(L, memory && memory->IsRAM(address, static_cast<u32>(size)));
  return 1;
}

// mem.ptr(address, o1, ..., on): reads a pointer at address, then one at pointer + o1, ... and
// returns the last pointer read; nil when a read fails or a pointer is not RAM.
int MemPtr(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const int offsets = lua_gettop(L) - 1;
  for (int i = 0; i < offsets; ++i)
    luaL_checkinteger(L, i + 2);
  MemoryAccess* memory = Memory(L);
  if (!memory)
  {
    lua_pushnil(L);
    return 1;
  }
  std::optional<u32> pointer = memory->Read32(address);
  for (int i = 0; pointer && i < offsets; ++i)
  {
    if (!memory->IsRAM(*pointer, 4))
    {
      pointer.reset();
      break;
    }
    pointer = memory->Read32(*pointer + static_cast<u32>(lua_tointeger(L, i + 2)));
  }
  if (!pointer || !memory->IsRAM(*pointer, 1))
    lua_pushnil(L);
  else
    lua_pushinteger(L, *pointer);
  return 1;
}

// mem.chain(address, o1, ..., on): Cheat Engine style [[address] + o1] + ... + on, an address.
int MemChain(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const int offsets = lua_gettop(L) - 1;
  for (int i = 0; i < offsets; ++i)
    luaL_checkinteger(L, i + 2);
  MemoryAccess* memory = Memory(L);
  if (!memory)
  {
    lua_pushnil(L);
    return 1;
  }
  std::optional<u32> pointer = memory->Read32(address);
  for (int i = 0; pointer && i + 1 < offsets; ++i)
  {
    if (!memory->IsRAM(*pointer, 4))
    {
      pointer.reset();
      break;
    }
    pointer = memory->Read32(*pointer + static_cast<u32>(lua_tointeger(L, i + 2)));
  }
  if (!pointer || !memory->IsRAM(*pointer, 1))
  {
    lua_pushnil(L);
    return 1;
  }
  const u32 last = offsets > 0 ? static_cast<u32>(lua_tointeger(L, offsets + 1)) : 0;
  lua_pushinteger(L, static_cast<u32>(*pointer + last));
  return 1;
}

// Reads a word or a list of words at `arg` into context->words.
void CheckWords(lua_State* L, int arg)
{
  ScriptContext* context = GetContext(L);
  context->words.clear();
  if (lua_isinteger(L, arg))
  {
    context->words.push_back(static_cast<u32>(lua_tointeger(L, arg)));
    return;
  }
  luaL_checktype(L, arg, LUA_TTABLE);
  const lua_Integer count = luaL_len(L, arg);
  luaL_argcheck(L, count >= 1 && count <= MAX_WORDS, arg, "1 to 4096 words");
  for (lua_Integer i = 1; i <= count; ++i)
  {
    lua_geti(L, arg, i);
    if (!lua_isinteger(L, -1))
      luaL_error(L, "word %d is not an integer", static_cast<int>(i));
    context->words.push_back(static_cast<u32>(lua_tointeger(L, -1)));
    lua_pop(L, 1);
  }
}

// mem.read_words(address, count): a list of 32-bit words, or nil.
int MemReadWords(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  const lua_Integer count = luaL_checkinteger(L, 2);
  luaL_argcheck(L, count >= 1 && count <= MAX_WORDS, 2, "1 to 4096 words");
  MemoryAccess* memory = Memory(L);
  ScriptContext* context = GetContext(L);
  context->words.clear();
  for (lua_Integer i = 0; memory && i < count; ++i)
  {
    const std::optional<u32> word = memory->Read32(address + static_cast<u32>(i) * 4);
    if (!word)
      break;
    context->words.push_back(*word);
  }
  if (context->words.size() != static_cast<size_t>(count))
  {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, static_cast<int>(count), 0);
  for (lua_Integer i = 0; i < count; ++i)
  {
    lua_pushinteger(L, context->words[static_cast<size_t>(i)]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

// Writes context->words at `address` (only the words that differ when `only_changed`) and
// invalidates the code there. Returns the number of words written, or -1 on failure.
int WriteCode(MemoryAccess& memory, u32 address, const std::vector<u32>& words, bool only_changed)
{
  const u32 size = static_cast<u32>(words.size()) * 4;
  if (address % 4 != 0 || !memory.IsRAM(address, size))
    return -1;
  int written = 0;
  for (size_t i = 0; i < words.size(); ++i)
  {
    const u32 word_address = address + static_cast<u32>(i) * 4;
    if (only_changed)
    {
      const std::optional<u32> current = memory.Read32(word_address);
      if (current && *current == words[i])
        continue;
    }
    if (!memory.Write32(word_address, words[i]))
      return -1;
    ++written;
  }
  if (written > 0)
    memory.InvalidateCode(address, size);
  return written;
}

// mem.write_code(address, word | {words}): writes code and drops the JIT blocks that cover it.
int MemWriteCode(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  CheckWords(L, 2);
  MemoryAccess* memory = Memory(L);
  const int written = memory ? WriteCode(*memory, address, GetContext(L)->words, false) : -1;
  lua_pushboolean(L, written >= 0);
  return 1;
}

// mem.ensure_code(address, word | {words}) -> changed: rewrites the words that differ (a cave or
// hook the game, a savestate or a reload put back), nil on failure.
int MemEnsureCode(lua_State* L)
{
  const u32 address = CheckAddress(L, 1);
  CheckWords(L, 2);
  MemoryAccess* memory = Memory(L);
  const int written = memory ? WriteCode(*memory, address, GetContext(L)->words, true) : -1;
  if (written < 0)
    lua_pushnil(L);
  else
    lua_pushboolean(L, written > 0);
  return 1;
}
}  // namespace

void RegisterMemBindings(lua_State* L)
{
  static constexpr luaL_Reg FUNCTIONS[] = {
      {"u8", MemU8},
      {"u16", MemU16},
      {"u32", MemU32},
      {"s8", MemS8},
      {"s16", MemS16},
      {"s32", MemS32},
      {"f32", MemF32},
      {"f64", MemF64},
      {"f32s", MemF32s},
      {"write_u8", MemWrite<1>},
      {"write_u16", MemWrite<2>},
      {"write_u32", MemWrite<4>},
      {"write_f32", MemWriteF32},
      {"write_f64", MemWriteF64},
      {"write_f32s", MemWriteF32s},
      {"valid", MemValid},
      {"ptr", MemPtr},
      {"chain", MemChain},
      {"read_words", MemReadWords},
      {"write_code", MemWriteCode},
      {"ensure_code", MemEnsureCode},
      {nullptr, nullptr},
  };
  luaL_newlib(L, FUNCTIONS);
  const ScriptContext* context = GetContext(L);
  const ScriptEnvironment* env = context->env;
  lua_pushinteger(L, env ? env->scratch_base : 0);
  lua_setfield(L, -2, "scratch");
  lua_pushinteger(L, env ? env->scratch_size : 0);
  lua_setfield(L, -2, "scratch_size");
  lua_setglobal(L, "mem");
}
}  // namespace VRPatch
