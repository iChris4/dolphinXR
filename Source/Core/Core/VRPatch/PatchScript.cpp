// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/PatchScript.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fmt/format.h>

#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Core/VRPatch/GeckoRegion.h"
#include "Core/VRPatch/ScriptContext.h"

namespace VRPatch
{
PadResolver::~PadResolver() = default;

namespace
{
constexpr int HOOK_INTERVAL = 1000;
constexpr u32 LOG_LINES_PER_SECOND = 60;
constexpr const char* SETTING_CHANGED_HOOK = "on_setting_changed";
constexpr const char* HOOK_NAMES[] = {"on_load", "on_frame", "on_state_loaded", "on_unload"};
static_assert(std::size(HOOK_NAMES) == static_cast<size_t>(Hook::Count));

// ---- Allocator, budget, protected calls ----

void* Allocate(void* user_data, void* ptr, size_t old_size, size_t new_size)
{
  auto* context = static_cast<ScriptContext*>(user_data);
  // With ptr == nullptr, old_size encodes the type of the new object, not a size.
  const size_t current = ptr ? old_size : 0;
  if (new_size == 0)
  {
    std::free(ptr);
    context->memory_used -= current;
    return nullptr;
  }
  if (new_size > current && context->memory_used - current + new_size > context->memory_limit)
    return nullptr;
  void* result = std::realloc(ptr, new_size);
  if (!result)
  {
    // Lua assumes that shrinking never fails: keep the old block.
    return new_size <= current ? ptr : nullptr;
  }
  context->memory_used = context->memory_used - current + new_size;
  return result;
}

void BudgetHook(lua_State* L, lua_Debug*)
{
  ScriptContext* context = GetContext(L);
  context->instructions += HOOK_INTERVAL;
  if (context->instructions > context->limits.max_instructions ||
      Common::Timer::NowUs() > context->deadline_us)
  {
    // Level 0 is the Lua function that was running.
    luaL_where(L, 0);
    lua_pushfstring(L,
                    "the patch ran for too long (over %d ms or %d million instructions in one "
                    "call)",
                    static_cast<int>(context->limits.max_milliseconds),
                    static_cast<int>(context->limits.max_instructions / 1'000'000));
    lua_concat(L, 2);
    lua_error(L);
  }
}

int MessageHandler(lua_State* L)
{
  const char* message = lua_tostring(L, 1);
  if (!message)
  {
    if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
      return 1;
    message = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  }
  luaL_traceback(L, L, message, 1);
  return 1;
}

enum class RequestKind
{
  Load,
  CallFunction,
  SetSetting,
};

struct Request
{
  RequestKind kind = RequestKind::Load;
  ScriptContext* context = nullptr;
  std::string_view source;
  const char* function_name = nullptr;
  // CallFunction with a (name, value) argument pair (on_setting_changed), and SetSetting.
  const char* argument_name = nullptr;
  double argument_value = 0.0;
  bool argument_is_bool = false;
  const SettingDef* setting = nullptr;
  // Load: the manifest was invalid (context->error says why).
  bool manifest_failed = false;
};

// ---- Manifest ----
// These run inside a protected call and write into the context; see ScriptContext.h for why they
// keep no C++ objects of their own.

bool Fail(ScriptContext* context, std::string message)
{
  context->error = "patch table: " + std::move(message);
  return false;
}

// Reads t[key] (t at `index`) as a string. Absent = unchanged.
bool ReadString(lua_State* L, int index, const char* key, std::string* out, ScriptContext* context)
{
  const int type = lua_getfield(L, index, key);
  if (type == LUA_TNIL)
  {
    lua_pop(L, 1);
    return true;
  }
  if (type != LUA_TSTRING)
  {
    lua_pop(L, 1);
    return Fail(context, fmt::format("{} must be a string", key));
  }
  size_t length = 0;
  const char* text = lua_tolstring(L, -1, &length);
  out->assign(text, length);
  lua_pop(L, 1);
  return true;
}

bool ReadNumber(lua_State* L, int index, const char* key, double* out, ScriptContext* context)
{
  const int type = lua_getfield(L, index, key);
  if (type == LUA_TNIL)
  {
    lua_pop(L, 1);
    return true;
  }
  if (type == LUA_TBOOLEAN)
  {
    *out = lua_toboolean(L, -1) ? 1.0 : 0.0;
    lua_pop(L, 1);
    return true;
  }
  if (type != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, -1)))
  {
    lua_pop(L, 1);
    return Fail(context, fmt::format("{} must be a number", key));
  }
  *out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return true;
}

// A string, or an array of strings, at t[key], appended to `out`.
bool ReadStringList(lua_State* L, int index, const char* key, std::vector<std::string>* out,
                    ScriptContext* context)
{
  const int type = lua_getfield(L, index, key);
  if (type == LUA_TNIL)
  {
    lua_pop(L, 1);
    return true;
  }
  if (type == LUA_TSTRING)
  {
    size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    out->emplace_back(text, length);
    lua_pop(L, 1);
    return true;
  }
  if (type != LUA_TTABLE)
  {
    lua_pop(L, 1);
    return Fail(context, fmt::format("{} must be a string or a list of strings", key));
  }
  const lua_Integer count = luaL_len(L, -1);
  for (lua_Integer i = 1; i <= count; ++i)
  {
    if (lua_geti(L, -1, i) != LUA_TSTRING)
    {
      lua_pop(L, 2);
      return Fail(context, fmt::format("{}[{}] must be a string", key, i));
    }
    size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    out->emplace_back(text, length);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return true;
}

bool IsValidSettingName(std::string_view name)
{
  if (name.empty() || name.size() > 64)
    return false;
  if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
    return false;
  return std::ranges::all_of(
      name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool IsGameIdPrefix(std::string_view id)
{
  return !id.empty() && id.size() <= 6 && std::ranges::all_of(id, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c));
  });
}

bool IsPatchableAddress(u64 address)
{
  return (address >= 0x80000000 && address <= 0x81800000) ||
         (address >= 0x90000000 && address <= 0x94000000);
}

bool ReadSetting(lua_State* L, ScriptContext* context, lua_Integer position)
{
  Manifest& manifest = context->manifest;
  manifest.settings.emplace_back();
  SettingDef& def = manifest.settings.back();
  const int table = lua_gettop(L);
  if (!ReadString(L, table, "name", &def.name, context))
    return false;
  if (!IsValidSettingName(def.name))
  {
    return Fail(context, fmt::format("settings[{}].name must be a Lua identifier (\"{}\")",
                                     position, def.name));
  }
  const std::string& name = def.name;
  if (std::ranges::count_if(manifest.settings,
                            [&](const SettingDef& d) { return d.name == name; }) > 1)
  {
    return Fail(context, fmt::format("two settings are named \"{}\"", name));
  }

  std::string type = "number";
  if (!ReadString(L, table, "type", &type, context) ||
      !ReadString(L, table, "label", &def.label, context) ||
      !ReadString(L, table, "description", &def.description, context) ||
      !ReadString(L, table, "unit", &def.unit, context) ||
      !ReadNumber(L, table, "default", &def.default_value, context) ||
      !ReadNumber(L, table, "min", &def.min, context) ||
      !ReadNumber(L, table, "max", &def.max, context) ||
      !ReadNumber(L, table, "step", &def.step, context) ||
      !ReadStringList(L, table, "options", &def.options, context))
  {
    return false;
  }
  if (def.label.empty())
    def.label = def.name;

  if (type == "number")
  {
    def.type = SettingType::Number;
    if (def.min > def.max)
      return Fail(context, fmt::format("setting {}: min is above max", def.name));
    if (def.step < 0)
      return Fail(context, fmt::format("setting {}: step is negative", def.name));
  }
  else if (type == "bool")
  {
    def.type = SettingType::Bool;
    def.min = 0;
    def.max = 1;
    def.step = 1;
  }
  else if (type == "choice")
  {
    def.type = SettingType::Choice;
    if (def.options.empty())
      return Fail(context, fmt::format("setting {}: a choice needs options", def.name));
    def.min = 0;
    def.max = static_cast<double>(def.options.size() - 1);
    def.step = 1;
  }
  else
  {
    return Fail(context, fmt::format("setting {}: type is number, bool or choice", def.name));
  }
  def.default_value = def.Normalize(def.default_value);
  return true;
}

bool ReadAddressRanges(lua_State* L, int index, ScriptContext* context)
{
  if (lua_getfield(L, index, "reserve") == LUA_TNIL)
  {
    lua_pop(L, 1);
    return true;
  }
  if (!lua_istable(L, -1))
    return Fail(context, "reserve must be a list of {start, end} pairs");
  const lua_Integer count = luaL_len(L, -1);
  for (lua_Integer i = 1; i <= count; ++i)
  {
    lua_geti(L, -1, i);
    if (!lua_istable(L, -1))
      return Fail(context, fmt::format("reserve[{}] must be {{start, end}}", i));
    lua_geti(L, -1, 1);
    lua_geti(L, -2, 2);
    const bool numbers = lua_isinteger(L, -2) && lua_isinteger(L, -1);
    const lua_Integer start = lua_tointeger(L, -2);
    const lua_Integer end = lua_tointeger(L, -1);
    lua_pop(L, 3);
    if (!numbers || start < 0 || end <= start || !IsPatchableAddress(static_cast<u64>(start)) ||
        !IsPatchableAddress(static_cast<u64>(end)))
    {
      return Fail(context, fmt::format("reserve[{}] must be {{start, end}} with start < end, in "
                                       "MEM1 (0x80000000...) or MEM2 (0x90000000...)",
                                       i));
    }
    context->manifest.reserve.push_back({static_cast<u32>(start), static_cast<u32>(end)});
  }
  lua_pop(L, 1);
  return true;
}

bool ReadConflicts(lua_State* L, int index, ScriptContext* context)
{
  if (lua_getfield(L, index, "conflicts") == LUA_TNIL)
  {
    lua_pop(L, 1);
    return true;
  }
  if (!lua_istable(L, -1))
    return Fail(context, "conflicts must be a table like {texture = {\"Name\"}}");
  const int table = lua_gettop(L);
  for (const OverrideKind kind :
       {OverrideKind::Shader, OverrideKind::Elements, OverrideKind::Texture})
  {
    context->strings.clear();
    if (!ReadStringList(L, table, OverrideKindName(kind), &context->strings, context))
      return false;
    for (std::string& name : context->strings)
      context->manifest.conflicts.push_back({kind, std::move(name)});
  }
  context->strings.clear();
  lua_pop(L, 1);
  return true;
}

bool IsGeckoLine(std::string_view line)
{
  if (line.size() != 17 || line[8] != ' ')
    return false;
  for (size_t i = 0; i < line.size(); ++i)
  {
    if (i != 8 && !std::isxdigit(static_cast<unsigned char>(line[i])))
      return false;
  }
  return true;
}

// Splits the gecko entries into lines, drops blank lines and comments, checks the format.
bool NormalizeGeckoLines(ScriptContext* context)
{
  std::vector<std::string> lines;
  for (const std::string& entry : context->manifest.gecko_lines)
  {
    for (std::string_view line : SplitString(entry, '\n'))
    {
      line = StripWhitespace(line);
      if (line.empty() || line.front() == '#' || line.front() == '*')
        continue;
      std::string normalized(line);
      // Collapse runs of spaces and tabs between the two words.
      const size_t space = normalized.find_first_of(" \t");
      if (space != std::string::npos)
      {
        const size_t second = normalized.find_first_not_of(" \t", space);
        if (second != std::string::npos)
          normalized = normalized.substr(0, space) + ' ' + normalized.substr(second);
      }
      if (!IsGeckoLine(normalized))
        return Fail(context, fmt::format("\"{}\" is not a Gecko code line", line));
      lines.push_back(std::move(normalized));
    }
  }
  context->manifest.gecko_lines = std::move(lines);
  return true;
}

bool ReadManifestTable(lua_State* L, ScriptContext* context)
{
  context->manifest = {};
  if (lua_getglobal(L, "patch") != LUA_TTABLE)
  {
    lua_pop(L, 1);
    return Fail(context, "the file must set a global table `patch = { title = ..., ... }`");
  }
  const int table = lua_gettop(L);
  Manifest& manifest = context->manifest;
  if (!ReadString(L, table, "title", &manifest.title, context) ||
      !ReadString(L, table, "author", &manifest.author, context) ||
      !ReadString(L, table, "description", &manifest.description, context) ||
      !ReadString(L, table, "version", &manifest.version, context) ||
      !ReadStringList(L, table, "games", &manifest.games, context) ||
      !ReadStringList(L, table, "gecko", &manifest.gecko_lines, context) ||
      !ReadAddressRanges(L, table, context) || !ReadConflicts(L, table, context))
  {
    return false;
  }
  if (manifest.title.empty())
    return Fail(context, "title is missing");
  for (const std::string& game : manifest.games)
  {
    if (!IsGameIdPrefix(game))
      return Fail(context, fmt::format("games: \"{}\" is not a game ID", game));
  }
  if (!NormalizeGeckoLines(context))
    return false;

  double revision = -1;
  if (!ReadNumber(L, table, "revision", &revision, context))
    return false;
  if (revision >= 0)
  {
    if (revision > 0xFFFF || revision != std::floor(revision))
      return Fail(context, "revision must be an integer");
    manifest.revision = static_cast<u16>(revision);
  }

  double scratch = 0;
  if (!ReadNumber(L, table, "scratch", &scratch, context))
    return false;
  if (scratch < 0 || scratch > MAX_SCRATCH_BYTES)
    return Fail(context, fmt::format("scratch must be 0 to {} bytes", MAX_SCRATCH_BYTES));
  manifest.scratch_bytes =
      (static_cast<u32>(scratch) + SCRATCH_ALIGNMENT - 1) & ~(SCRATCH_ALIGNMENT - 1);

  if (lua_getfield(L, table, "settings") != LUA_TNIL)
  {
    if (!lua_istable(L, -1))
      return Fail(context, "settings must be a list of {name = ..., ...} tables");
    const lua_Integer count = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= count; ++i)
    {
      if (lua_geti(L, -1, i) != LUA_TTABLE)
        return Fail(context, fmt::format("settings[{}] must be a table", i));
      if (!ReadSetting(L, context, i))
        return false;
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 2);
  return true;
}

// Integer settings (choices, whole-number ranges and steps) are Lua integers, so that they print
// and index like the patch expects.
bool IsIntegerSetting(const SettingDef& def)
{
  if (def.type == SettingType::Choice)
    return true;
  const auto whole = [](double v) { return std::floor(v) == v && std::abs(v) < 1e15; };
  return def.type == SettingType::Number && def.step > 0 && whole(def.step) && whole(def.min) &&
         whole(def.max) && whole(def.default_value);
}

void PushSetting(lua_State* L, const SettingDef& def, double value)
{
  if (def.type == SettingType::Bool)
    lua_pushboolean(L, value != 0.0);
  else if (IsIntegerSetting(def) && std::floor(value) == value)
    lua_pushinteger(L, static_cast<lua_Integer>(value));
  else
    lua_pushnumber(L, value);
}

void CreateSettingsTable(lua_State* L, ScriptContext* context)
{
  lua_createtable(L, 0, static_cast<int>(context->manifest.settings.size()));
  for (const SettingDef& def : context->manifest.settings)
  {
    PushSetting(L, def, def.default_value);
    lua_setfield(L, -2, def.name.c_str());
  }
  lua_setglobal(L, "settings");
}

int Trampoline(lua_State* L)
{
  auto* request = static_cast<Request*>(lua_touserdata(L, 1));
  ScriptContext* context = request->context;
  switch (request->kind)
  {
  case RequestKind::Load:
  {
    const std::string_view source = request->source;
    if (luaL_loadbufferx(L, source.data(), source.size(), lua_tostring(L, 2), "t") != LUA_OK)
      return lua_error(L);
    lua_call(L, 0, 0);
    if (!ReadManifestTable(L, context))
    {
      request->manifest_failed = true;
      return 0;
    }
    for (size_t i = 0; i < std::size(HOOK_NAMES); ++i)
    {
      context->hooks[i] = lua_getglobal(L, HOOK_NAMES[i]) == LUA_TFUNCTION;
      lua_pop(L, 1);
    }
    context->has_setting_changed = lua_getglobal(L, SETTING_CHANGED_HOOK) == LUA_TFUNCTION;
    lua_pop(L, 1);
    CreateSettingsTable(L, context);
    return 0;
  }
  case RequestKind::CallFunction:
  {
    if (lua_getglobal(L, request->function_name) != LUA_TFUNCTION)
      return 0;
    int arguments = 0;
    if (request->argument_name)
    {
      lua_pushstring(L, request->argument_name);
      if (request->setting)
        PushSetting(L, *request->setting, request->argument_value);
      else if (request->argument_is_bool)
        lua_pushboolean(L, request->argument_value != 0.0);
      else
        lua_pushnumber(L, request->argument_value);
      arguments = 2;
    }
    lua_call(L, arguments, 0);
    return 0;
  }
  case RequestKind::SetSetting:
  {
    if (lua_getglobal(L, "settings") != LUA_TTABLE)
    {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setglobal(L, "settings");
    }
    PushSetting(L, *request->setting, request->argument_value);
    lua_setfield(L, -2, request->argument_name);
    return 0;
  }
  }
  return 0;
}

// Drops the traceback lines that only name the engine's C entry points ("[C]: in ?"), and the
// whole traceback when nothing else is left.
void CleanTraceback(std::string* error)
{
  const size_t header = error->find("\nstack traceback:");
  if (header == std::string::npos)
    return;
  std::string kept;
  size_t position = error->find('\n', header + 1);
  while (position != std::string::npos)
  {
    const size_t next = error->find('\n', position + 1);
    const std::string_view line = std::string_view(*error).substr(
        position + 1, next == std::string::npos ? std::string::npos : next - position - 1);
    if (line != "\t[C]: in ?" && !line.empty())
    {
      kept += '\n';
      kept += line;
    }
    position = next;
  }
  error->resize(header);
  if (!kept.empty())
    *error += "\nstack traceback:" + kept;
}

// Runs `request` in protected mode; false with context->error set on an error.
bool RunProtected(ScriptContext* context, Request* request, const char* chunk_name)
{
  lua_State* L = context->L;
  lua_settop(L, 0);
  lua_pushcfunction(L, MessageHandler);
  lua_pushcfunction(L, Trampoline);
  lua_pushlightuserdata(L, request);
  int arguments = 1;
  if (chunk_name)
  {
    lua_pushstring(L, chunk_name);
    arguments = 2;
  }

  const u64 start = Common::Timer::NowUs();
  context->instructions = 0;
  context->deadline_us = start + static_cast<u64>(context->limits.max_milliseconds) * 1000;
  lua_sethook(L, BudgetHook, LUA_MASKCOUNT, HOOK_INTERVAL);
  const int status = lua_pcall(L, arguments, 0, 1);
  lua_sethook(L, nullptr, 0, 0);
  context->last_call_us = Common::Timer::NowUs() - start;

  if (status != LUA_OK)
  {
    if (lua_type(L, -1) == LUA_TSTRING)
    {
      size_t length = 0;
      const char* message = lua_tolstring(L, -1, &length);
      context->error.assign(message, length);
      CleanTraceback(&context->error);
    }
    else
    {
      context->error = "unknown error";
    }
    lua_settop(L, 0);
    return false;
  }
  lua_settop(L, 0);
  if (request->manifest_failed)
    return false;
  return true;
}

void OpenLibrary(lua_State* L, const char* name, lua_CFunction open)
{
  luaL_requiref(L, name, open, 1);
  lua_pop(L, 1);
}

int OpenSandbox(lua_State* L)
{
  OpenLibrary(L, LUA_GNAME, luaopen_base);
  OpenLibrary(L, LUA_COLIBNAME, luaopen_coroutine);
  OpenLibrary(L, LUA_TABLIBNAME, luaopen_table);
  OpenLibrary(L, LUA_STRLIBNAME, luaopen_string);
  OpenLibrary(L, LUA_MATHLIBNAME, luaopen_math);
  OpenLibrary(L, LUA_UTF8LIBNAME, luaopen_utf8);

  // No file access and no bytecode loading.
  for (const char* name : {"dofile", "loadfile", "load", "require"})
  {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }

  RegisterMemBindings(L);
  RegisterVRBindings(L);
  RegisterMiscBindings(L);

  const std::string_view math = GetPreludeMath();
  if (luaL_loadbufferx(L, math.data(), math.size(), "=prelude", "t") != LUA_OK)
    return lua_error(L);
  lua_call(L, 0, 0);
  const std::string_view api = GetPreludeApi();
  if (luaL_loadbufferx(L, api.data(), api.size(), "=prelude", "t") != LUA_OK)
    return lua_error(L);
  lua_call(L, 0, 0);
  return 0;
}

void WarnFunction(void* user_data, const char* message, int to_continue)
{
  // Lua's warn(): control messages start with '@'. Pieces are appended as separate lines.
  auto* context = static_cast<ScriptContext*>(user_data);
  if (!message || (*message == '@' && !to_continue))
    return;
  AppendLog(context, 1, message);
}
}  // namespace

ScriptContext* GetContext(lua_State* L)
{
  return *static_cast<ScriptContext**>(lua_getextraspace(L));
}

void AppendLog(ScriptContext* context, u8 level, std::string_view text)
{
  const u64 now = Common::Timer::NowUs();
  if (now - context->log_window_start_us > 1'000'000)
  {
    if (context->log_lines_in_window > LOG_LINES_PER_SECOND)
    {
      context->log.push_back(
          {1, context->env ? context->env->field : 0,
           fmt::format("({} log lines dropped: at most {} per second are kept)",
                       context->log_lines_in_window - LOG_LINES_PER_SECOND, LOG_LINES_PER_SECOND)});
    }
    context->log_window_start_us = now;
    context->log_lines_in_window = 0;
  }
  if (++context->log_lines_in_window > LOG_LINES_PER_SECOND)
    return;
  constexpr size_t MAX_LINE = 1000;
  context->log.push_back({level, context->env ? context->env->field : 0,
                          std::string(text.substr(0, std::min(text.size(), MAX_LINE)))});
  // A patch that logs every field but whose log nobody takes must not grow without bound.
  if (context->log.size() > 1000)
    context->log.erase(context->log.begin(), context->log.begin() + 500);
}

const char* HookName(Hook hook)
{
  return hook < Hook::Count ? HOOK_NAMES[static_cast<size_t>(hook)] : "?";
}

PatchScript::PatchScript(std::string id, ScriptLimits limits)
    : m_context(std::make_unique<ScriptContext>())
{
  m_context->id = std::move(id);
  m_context->limits = limits;
  m_context->memory_limit = limits.max_memory_bytes;
}

PatchScript::~PatchScript()
{
  if (m_context->L)
    lua_close(m_context->L);
}

bool PatchScript::Load(std::string_view source, const ScriptEnvironment& env)
{
  ScriptContext* context = m_context.get();
  if (context->L)
  {
    lua_close(context->L);
    context->L = nullptr;
    context->memory_used = 0;
  }
  context->error.clear();
  context->hooks = {};
  context->has_setting_changed = false;
  context->manifest = {};

  lua_State* L = lua_newstate(Allocate, context);
  if (!L)
  {
    context->error = "could not create the Lua state";
    return false;
  }
  context->L = L;
  *static_cast<ScriptContext**>(lua_getextraspace(L)) = context;
  lua_setwarnf(L, WarnFunction, context);
  lua_gc(L, LUA_GCGEN, 0, 0);

  context->env = &env;
  // The sandbox itself, outside the patch's budget accounting but still protected.
  lua_settop(L, 0);
  lua_pushcfunction(L, MessageHandler);
  lua_pushcfunction(L, OpenSandbox);
  const int status = lua_pcall(L, 0, 0, 1);
  if (status != LUA_OK)
  {
    context->error = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "sandbox error";
    context->env = nullptr;
    return false;
  }

  Request request;
  request.kind = RequestKind::Load;
  request.context = context;
  request.source = source;
  const std::string chunk_name = "@" + context->id + ".lua";
  const bool ok = RunProtected(context, &request, chunk_name.c_str());
  context->env = nullptr;
  return ok;
}

const std::string& PatchScript::GetId() const
{
  return m_context->id;
}

const Manifest& PatchScript::GetManifest() const
{
  return m_context->manifest;
}

bool PatchScript::HasHook(Hook hook) const
{
  return hook < Hook::Count && m_context->hooks[static_cast<size_t>(hook)];
}

bool PatchScript::HasSettingChangedHook() const
{
  return m_context->has_setting_changed;
}

bool PatchScript::Call(Hook hook, const ScriptEnvironment& env)
{
  if (!m_context->L || !HasHook(hook))
    return true;
  Request request;
  request.kind = RequestKind::CallFunction;
  request.context = m_context.get();
  request.function_name = HOOK_NAMES[static_cast<size_t>(hook)];
  m_context->env = &env;
  const bool ok = RunProtected(m_context.get(), &request, nullptr);
  m_context->env = nullptr;
  return ok;
}

bool PatchScript::CallSettingChanged(const std::string& name, double value,
                                     const ScriptEnvironment& env)
{
  if (!m_context->L || !m_context->has_setting_changed)
    return true;
  const SettingDef* def = m_context->manifest.FindSetting(name);
  Request request;
  request.kind = RequestKind::CallFunction;
  request.context = m_context.get();
  request.function_name = SETTING_CHANGED_HOOK;
  request.argument_name = name.c_str();
  request.argument_value = value;
  request.argument_is_bool = def && def->type == SettingType::Bool;
  request.setting = def;
  m_context->env = &env;
  const bool ok = RunProtected(m_context.get(), &request, nullptr);
  m_context->env = nullptr;
  return ok;
}

bool PatchScript::SetSettingValue(const SettingDef& def, double value)
{
  if (!m_context->L)
    return false;
  Request request;
  request.kind = RequestKind::SetSetting;
  request.context = m_context.get();
  request.argument_name = def.name.c_str();
  request.argument_value = def.Normalize(value);
  request.argument_is_bool = def.type == SettingType::Bool;
  request.setting = &def;
  return RunProtected(m_context.get(), &request, nullptr);
}

const std::string& PatchScript::Error() const
{
  return m_context->error;
}

std::vector<LogLine> PatchScript::TakeLog()
{
  return std::exchange(m_context->log, {});
}

const PadHolds& PatchScript::GetPadHolds() const
{
  return m_context->pad_holds;
}

u64 PatchScript::GetPadHoldsGeneration() const
{
  return m_context->pad_holds_generation;
}

std::vector<SettingRequest> PatchScript::TakeSettingRequests()
{
  return std::exchange(m_context->setting_requests, {});
}

size_t PatchScript::GetMemoryUsed() const
{
  return m_context->memory_used;
}

u64 PatchScript::GetLastCallMicroseconds() const
{
  return m_context->last_call_us;
}

std::variant<Manifest, std::string> ReadManifest(std::string_view source, std::string_view id)
{
  ScriptLimits limits;
  limits.max_instructions = 20'000'000;
  limits.max_milliseconds = 500;
  limits.max_memory_bytes = 16 * 1024 * 1024;
  PatchScript script(std::string(id), limits);
  const VRInputFrame no_input{};
  ScriptEnvironment env;
  env.input = &no_input;
  if (!script.Load(source, env))
    return script.Error();
  return script.GetManifest();
}

bool IsValidPatchId(std::string_view id)
{
  if (id.empty() || id.size() > 64 || id.front() == '.')
    return false;
  return std::ranges::all_of(id, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
  });
}
}  // namespace VRPatch
