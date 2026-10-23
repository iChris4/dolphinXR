// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/VRPatch/PatchTypes.h"

#include <algorithm>
#include <cmath>

#include "Common/StringUtil.h"

namespace VRPatch
{
double SettingDef::Normalize(double value) const
{
  if (!std::isfinite(value))
    value = default_value;
  switch (type)
  {
  case SettingType::Bool:
    return value != 0.0 ? 1.0 : 0.0;
  case SettingType::Choice:
  {
    const double last = options.empty() ? 0.0 : static_cast<double>(options.size() - 1);
    return std::clamp(std::round(value), 0.0, last);
  }
  case SettingType::Number:
  default:
    break;
  }
  if (min <= max)
    value = std::clamp(value, min, max);
  return value;
}

const char* OverrideKindName(OverrideKind kind)
{
  switch (kind)
  {
  case OverrideKind::Shader:
    return "shader";
  case OverrideKind::Elements:
    return "elements";
  case OverrideKind::Texture:
  default:
    return "texture";
  }
}

std::optional<OverrideKind> ParseOverrideKind(std::string_view name)
{
  if (Common::CaseInsensitiveEquals(name, "shader"))
    return OverrideKind::Shader;
  if (Common::CaseInsensitiveEquals(name, "elements"))
    return OverrideKind::Elements;
  if (Common::CaseInsensitiveEquals(name, "texture"))
    return OverrideKind::Texture;
  return std::nullopt;
}

const SettingDef* Manifest::FindSetting(std::string_view name) const
{
  const auto it =
      std::ranges::find_if(settings, [&](const SettingDef& def) { return def.name == name; });
  return it == settings.end() ? nullptr : &*it;
}

const char* PatchOriginName(PatchOrigin origin)
{
  switch (origin)
  {
  case PatchOrigin::Sys:
    return "Sys";
  case PatchOrigin::User:
    return "User";
  case PatchOrigin::Session:
  default:
    return "Session";
  }
}
}  // namespace VRPatch
