// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// vr.*: the VR controllers and headset (raw functions; the prelude wraps the poses into objects).
// See ScriptContext.h for the rules a binding follows.

#include <cctype>
#include <cstring>

#include "Common/Config/Config.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/VRPatch/ScriptContext.h"
#include "Core/VRPatch/VRSettingsTable.h"
#include "VideoCommon/VR/CameraPitch.h"

namespace VRPatch
{
namespace
{
const VRInputFrame& Input(lua_State* L)
{
  static const VRInputFrame NO_INPUT{};
  const ScriptContext* context = GetContext(L);
  return context->env && context->env->input ? *context->env->input : NO_INPUT;
}

// "left" / "right" / 0 / 1.
int CheckHand(lua_State* L, int arg)
{
  if (lua_type(L, arg) == LUA_TNUMBER)
  {
    const lua_Integer hand = luaL_checkinteger(L, arg);
    luaL_argcheck(L, hand == 0 || hand == 1, arg, "hand is \"left\", \"right\", 0 or 1");
    return static_cast<int>(hand);
  }
  const char* name = luaL_checkstring(L, arg);
  if (std::strcmp(name, "left") == 0)
    return 0;
  if (std::strcmp(name, "right") == 0)
    return 1;
  return luaL_argerror(L, arg, "hand is \"left\", \"right\", 0 or 1");
}

int PushPose(lua_State* L, const PoseInput& pose)
{
  lua_pushboolean(L, pose.valid);
  for (const float value : pose.position)
    lua_pushnumber(L, value);
  for (const float value : pose.orientation)
    lua_pushnumber(L, value);
  return 8;
}

// vr.raw_pose(which, kind) -> valid, px, py, pz, qx, qy, qz, qw. which: "head" or a hand; kind:
// "aim" (default), "grip", "pointer", "view" or "level_view".
int VRRawPose(lua_State* L)
{
  const VRInputFrame& input = Input(L);
  if (lua_type(L, 1) == LUA_TSTRING && std::strcmp(lua_tostring(L, 1), "head") == 0)
    return PushPose(L, input.head);
  const HandInput& hand = input.hands[CheckHand(L, 1)];
  const char* kind = luaL_optstring(L, 2, "aim");
  if (std::strcmp(kind, "aim") == 0)
    return PushPose(L, hand.aim);
  if (std::strcmp(kind, "grip") == 0)
    return PushPose(L, hand.grip);
  if (std::strcmp(kind, "pointer") == 0)
    return PushPose(L, hand.pointer);
  if (std::strcmp(kind, "view") == 0)
    return PushPose(L, hand.view);
  if (std::strcmp(kind, "level_view") == 0)
    return PushPose(L, hand.level_view);
  return luaL_argerror(L, 2, "kind is \"aim\", \"grip\", \"pointer\", \"view\" or \"level_view\"");
}

int VRButton(lua_State* L)
{
  const HandInput& hand = Input(L).hands[CheckHand(L, 1)];
  const char* name = luaL_checkstring(L, 2);
  struct Button
  {
    const char* name;
    bool HandInput::* field;
  };
  static constexpr Button BUTTONS[] = {
      {"primary", &HandInput::primary},
      {"secondary", &HandInput::secondary},
      {"button_3", &HandInput::button_3},
      {"button_4", &HandInput::button_4},
      {"menu", &HandInput::menu},
      {"shoulder", &HandInput::shoulder},
      {"trigger", &HandInput::trigger_button},
      {"squeeze", &HandInput::squeeze_button},
      {"thumbstick", &HandInput::thumbstick_button},
  };
  for (const Button& button : BUTTONS)
  {
    if (std::strcmp(name, button.name) == 0)
    {
      lua_pushboolean(L, hand.*button.field);
      return 1;
    }
  }
  return luaL_argerror(L, 2,
                       "button is primary, secondary, button_3, button_4, menu, shoulder, trigger, "
                       "squeeze or thumbstick");
}

int VRAxis(lua_State* L)
{
  const HandInput& hand = Input(L).hands[CheckHand(L, 1)];
  const char* name = luaL_checkstring(L, 2);
  struct Axis
  {
    const char* name;
    float HandInput::* field;
  };
  static constexpr Axis AXES[] = {
      {"trigger", &HandInput::trigger},
      {"squeeze", &HandInput::squeeze},
      {"squeeze_force", &HandInput::squeeze_force},
      {"thumbstick_x", &HandInput::thumbstick_x},
      {"thumbstick_y", &HandInput::thumbstick_y},
  };
  for (const Axis& axis : AXES)
  {
    if (std::strcmp(name, axis.name) == 0)
    {
      lua_pushnumber(L, hand.*axis.field);
      return 1;
    }
  }
  return luaL_argerror(L, 2,
                       "axis is trigger, squeeze, squeeze_force, thumbstick_x or thumbstick_y");
}

int VRConnected(lua_State* L)
{
  lua_pushboolean(L, Input(L).hands[CheckHand(L, 1)].connected);
  return 1;
}

// vr.raw_velocity(hand) -> linear_valid, lx, ly, lz, angular_valid, ax, ay, az (grip, m/s and
// rad/s in the reference space).
int VRRawVelocity(lua_State* L)
{
  const HandInput& hand = Input(L).hands[CheckHand(L, 1)];
  lua_pushboolean(L, hand.linear_velocity_valid);
  for (const float value : hand.linear_velocity)
    lua_pushnumber(L, value);
  lua_pushboolean(L, hand.angular_velocity_valid);
  for (const float value : hand.angular_velocity)
    lua_pushnumber(L, value);
  return 8;
}

// vr.screen_hit(hand) -> u, v, distance (where the aim ray hits the virtual screen, -1..1), or nil.
int VRScreenHit(lua_State* L)
{
  const HandInput& hand = Input(L).hands[CheckHand(L, 1)];
  if (!hand.screen_hit_valid)
  {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, hand.screen_u);
  lua_pushnumber(L, hand.screen_v);
  lua_pushnumber(L, hand.screen_distance);
  return 3;
}

int VRActive(lua_State* L)
{
  lua_pushboolean(L, Input(L).runtime_active);
  return 1;
}

int VRFocused(lua_State* L)
{
  lua_pushboolean(L, Input(L).session_focused);
  return 1;
}

int VRMenuOpen(lua_State* L)
{
  lua_pushboolean(L, Input(L).menu_open);
  return 1;
}

int VRUnitsPerMeter(lua_State* L)
{
  const ScriptContext* context = GetContext(L);
  lua_pushnumber(L, context->env ? context->env->units_per_meter : 1.0);
  return 1;
}

// vr.haptic(hand, amplitude 0-1, seconds): a one-shot vibration.
int VRHaptic(lua_State* L)
{
  const int hand = CheckHand(L, 1);
  const double amplitude = luaL_checknumber(L, 2);
  const double seconds = luaL_optnumber(L, 3, 0.05);
  luaL_argcheck(L, seconds >= 0 && seconds <= 2, 3, "0 to 2 seconds");
  const ScriptContext* context = GetContext(L);
  if (context->env && context->env->memory)
  {
    Common::VR::OpenXRInputState::PulseHaptic(static_cast<size_t>(hand),
                                              static_cast<float>(amplitude),
                                              static_cast<int64_t>(seconds * 1e9));
  }
  return 0;
}

// Copies the current value of a VR setting into context->text. False for an unknown key.
bool ReadSetting(ScriptContext* context, const char* key)
{
  const VRSetting* setting = FindVRSetting(key);
  if (!setting)
    return false;
  context->text = Config::GetAsString(setting->location).value_or(setting->default_value);
  return true;
}

// Queues a VR setting change (applied on the host thread to the CurrentRun layer). False with
// context->text set when the key or the value is invalid.
bool RequestSetting(ScriptContext* context, const char* key, const char* value)
{
  const VRSetting* setting = FindVRSetting(key);
  if (!setting)
  {
    context->text = std::string("unknown VR setting ") + key;
    return false;
  }
  const std::optional<std::string> normalized = setting->normalize(value);
  if (!normalized)
  {
    context->text = std::string("invalid value for ") + key + ": " + value;
    return false;
  }
  std::string& last = context->requested_settings[setting->location.key];
  if (last == *normalized)
    return true;
  last = *normalized;
  context->setting_requests.push_back({setting->location.key, *normalized});
  return true;
}

// vr.setting(key) -> the value as a string (e.g. "1.5", "True").
int VRSettingGet(lua_State* L)
{
  const char* key = luaL_checkstring(L, 1);
  ScriptContext* context = GetContext(L);
  if (!ReadSetting(context, key))
    return luaL_argerror(L, 1, "unknown VR setting (the [Graphics.VR] keys)");
  lua_pushlstring(L, context->text.data(), context->text.size());
  return 1;
}

// vr.set_setting(key, value): for this session (not saved), applied after the field.
int VRSettingSet(lua_State* L)
{
  const char* key = luaL_checkstring(L, 1);
  const char* value = nullptr;
  if (lua_isboolean(L, 2))
    value = lua_toboolean(L, 2) ? "True" : "False";
  else
    value = luaL_checkstring(L, 2);
  ScriptContext* context = GetContext(L);
  if (!RequestSetting(context, key, value))
    return luaL_error(L, "%s", context->text.c_str());
  return 0;
}

// Raises or lowers one of the patch's override flags. False for an invalid name.
bool SetFlag(ScriptContext* context, const char* name, bool active)
{
  const size_t length = std::strlen(name);
  if (length == 0 || length > 64)
    return false;
  for (size_t i = 0; i < length; ++i)
  {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (!std::isalnum(c) && c != '_' && c != '.' && c != '-')
      return false;
  }
  const bool changed =
      active ? context->flags.insert(name).second : context->flags.erase(name) != 0;
  if (changed)
    ++context->flags_generation;
  return true;
}

// vr.set_flag(name[, active]): raises an override flag (lowers it with false). Override entries
// condition on it like on the flags draws raise (condition=<name>); it stays until lowered.
int VRSetFlag(lua_State* L)
{
  const char* name = luaL_checkstring(L, 1);
  const bool active = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
  if (!SetFlag(GetContext(L), name, active))
    return luaL_argerror(L, 1, "a flag name is 1 to 64 letters, digits, '_', '.' or '-'");
  return 0;
}

// vr.set_camera_pitch_source(address | nil): the f32 at `address` holds the sine of the game
// camera's pitch (+ up); the VR view removes that pitch from each frame. nil: no more.
int VRSetCameraPitchSource(lua_State* L)
{
  ScriptContext* context = GetContext(L);
  if (lua_isnoneornil(L, 1))
  {
    context->camera_pitch_source.reset();
    return 0;
  }
  const lua_Integer address = luaL_checkinteger(L, 1);
  if (address < 0x80000000 || address > 0xFFFFFFFC || (address & 3) != 0)
    return luaL_argerror(L, 1, "an aligned address of the game's memory (0x8xxxxxxx, 0x9xxxxxxx)");
  context->camera_pitch_source = static_cast<u32>(address);
  return 0;
}

// vr.camera_pitch_source() -> address or nil: the source the running patches named, whose pitch
// the VR view removes.
int VRCameraPitchSource(lua_State* L)
{
  if (const std::optional<u32> source = VR::CameraPitch::GetSource())
    lua_pushinteger(L, *source);
  else
    lua_pushnil(L);
  return 1;
}

// vr.keep_pointer_on_screen(keep): an aim past the screen's edge holds the VR pointer at that
// edge instead of losing it.
int VRKeepPointerOnScreen(lua_State* L)
{
  GetContext(L)->keep_pointer_on_screen = lua_toboolean(L, 1);
  return 0;
}

// vr.clear_flags(): lowers every flag of the patch.
int VRClearFlags(lua_State* L)
{
  ScriptContext* context = GetContext(L);
  if (!context->flags.empty())
  {
    context->flags.clear();
    ++context->flags_generation;
  }
  return 0;
}
}  // namespace

void RegisterVRBindings(lua_State* L)
{
  static constexpr luaL_Reg FUNCTIONS[] = {
      {"raw_pose", VRRawPose},
      {"button", VRButton},
      {"axis", VRAxis},
      {"connected", VRConnected},
      {"raw_velocity", VRRawVelocity},
      {"screen_hit", VRScreenHit},
      {"active", VRActive},
      {"focused", VRFocused},
      {"menu_open", VRMenuOpen},
      {"units_per_meter", VRUnitsPerMeter},
      {"haptic", VRHaptic},
      {"setting", VRSettingGet},
      {"set_setting", VRSettingSet},
      {"set_flag", VRSetFlag},
      {"clear_flags", VRClearFlags},
      {"set_camera_pitch_source", VRSetCameraPitchSource},
      {"camera_pitch_source", VRCameraPitchSource},
      {"keep_pointer_on_screen", VRKeepPointerOnScreen},
      {nullptr, nullptr},
  };
  luaL_newlib(L, FUNCTIONS);
  lua_setglobal(L, "vr");
}
}  // namespace VRPatch
