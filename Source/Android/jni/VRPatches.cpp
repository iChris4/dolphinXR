// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The VR game patches (Core/VRPatch) for the Quest settings screens: the list as JSON, and the
// enable and setting changes, written by the same C++ code as the desktop and the headset menu.

#include <jni.h>

#include <optional>
#include <string>

#include <picojson.h>

#include "jni/AndroidCommon/AndroidCommon.h"

#ifdef ENABLE_VR
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/VRPatch/VRPatch.h"
#endif

namespace
{
#ifdef ENABLE_VR
const char* SettingTypeName(VRPatch::SettingType type)
{
  switch (type)
  {
  case VRPatch::SettingType::Bool:
    return "bool";
  case VRPatch::SettingType::Choice:
    return "choice";
  case VRPatch::SettingType::Number:
  default:
    return "number";
  }
}

std::string PatchesJson(const std::string& game_id, std::optional<u16> revision)
{
  picojson::array patches;
  for (const VRPatch::PatchInfo& info : VRPatch::ListPatches(game_id, revision))
  {
    picojson::object patch;
    patch["id"] = picojson::value(info.id);
    patch["origin"] = picojson::value(std::string(VRPatch::PatchOriginName(info.origin)));
    patch["enabled"] = picojson::value(info.enabled);
    patch["state"] = picojson::value(std::string(VRPatch::PatchStateName(info.state)));
    patch["status"] = picojson::value(info.status);
    patch["error"] = picojson::value(info.manifest_error);
    patch["for_this_game"] = picojson::value(info.for_this_game);
    picojson::array settings;
    if (info.manifest)
    {
      const VRPatch::Manifest& m = *info.manifest;
      patch["title"] = picojson::value(m.title);
      patch["author"] = picojson::value(m.author);
      patch["description"] = picojson::value(m.description);
      for (const VRPatch::SettingDef& def : m.settings)
      {
        picojson::object setting;
        setting["name"] = picojson::value(def.name);
        setting["label"] = picojson::value(def.label);
        setting["description"] = picojson::value(def.description);
        setting["unit"] = picojson::value(def.unit);
        setting["type"] = picojson::value(std::string(SettingTypeName(def.type)));
        setting["min"] = picojson::value(def.min);
        setting["max"] = picojson::value(def.max);
        setting["step"] = picojson::value(def.step);
        setting["default"] = picojson::value(def.default_value);
        const auto value = info.values.find(def.name);
        setting["value"] =
            picojson::value(value == info.values.end() ? def.default_value : value->second);
        picojson::array options;
        for (const std::string& option : def.options)
          options.emplace_back(option);
        setting["options"] = picojson::value(std::move(options));
        settings.emplace_back(std::move(setting));
      }
    }
    else
    {
      patch["title"] = picojson::value(info.id);
    }
    patch["settings"] = picojson::value(std::move(settings));
    patches.emplace_back(std::move(patch));
  }
  return picojson::value(std::move(patches)).serialize();
}

// While emulation is paused the CPU thread would only apply the change on resume.
void ApplyNowIfPaused()
{
  Core::System& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Paused)
    VRPatch::ApplyPendingNow(system);
}
#endif
}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_GetVRPatches(JNIEnv* env,
                                                                                    jclass,
                                                                                    jstring game_id,
                                                                                    jint revision)
{
#ifdef ENABLE_VR
  const std::optional<u16> rev =
      revision >= 0 ? std::optional<u16>(static_cast<u16>(revision)) : std::nullopt;
  return ToJString(env, PatchesJson(GetJString(env, game_id), rev));
#else
  return ToJString(env, "[]");
#endif
}

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_SetVRPatchEnabled(
    JNIEnv* env, jclass, jstring game_id, jstring id, jboolean enabled)
{
#ifdef ENABLE_VR
  const bool saved =
      VRPatch::SetPatchEnabled(GetJString(env, game_id), GetJString(env, id), enabled);
  ApplyNowIfPaused();
  return static_cast<jboolean>(saved);
#else
  return JNI_FALSE;
#endif
}

JNIEXPORT jboolean JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_SetVRPatchSetting(
    JNIEnv* env, jclass, jstring game_id, jstring id, jstring name, jdouble value)
{
#ifdef ENABLE_VR
  const bool saved = VRPatch::SetPatchSetting(GetJString(env, game_id), GetJString(env, id),
                                              GetJString(env, name), value);
  ApplyNowIfPaused();
  return static_cast<jboolean>(saved);
#else
  return JNI_FALSE;
#endif
}

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_NativeLibrary_ReloadVRPatches(JNIEnv*, jclass)
{
#ifdef ENABLE_VR
  VRPatch::ReloadPatches();
  ApplyNowIfPaused();
#endif
}
}
