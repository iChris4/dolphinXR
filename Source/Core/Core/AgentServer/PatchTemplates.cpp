// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/AgentServer/PatchTemplates.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/StringUtil.h"

namespace AgentServer
{
std::optional<PatchTemplate> ParsePatchTemplate(std::string_view name)
{
  if (Common::CaseInsensitiveEquals(name, "li_r3_1") ||
      Common::CaseInsensitiveEquals(name, "return_true"))
    return PatchTemplate::ReturnTrue;
  if (Common::CaseInsensitiveEquals(name, "li_r3_0") ||
      Common::CaseInsensitiveEquals(name, "return_false"))
    return PatchTemplate::ReturnFalse;
  if (Common::CaseInsensitiveEquals(name, "nop_blr") ||
      Common::CaseInsensitiveEquals(name, "return_early"))
    return PatchTemplate::ReturnEarly;
  return std::nullopt;
}

std::string_view GetPatchTemplateName(PatchTemplate patch_template)
{
  switch (patch_template)
  {
  case PatchTemplate::ReturnTrue:
    return "li_r3_1";
  case PatchTemplate::ReturnFalse:
    return "li_r3_0";
  case PatchTemplate::ReturnEarly:
    return "nop_blr";
  }
  return "";
}

std::vector<u32> GetPatchTemplateWords(PatchTemplate patch_template)
{
  switch (patch_template)
  {
  case PatchTemplate::ReturnTrue:
    return {0x38600001u, BLR_INSTRUCTION};
  case PatchTemplate::ReturnFalse:
    return {0x38600000u, BLR_INSTRUCTION};
  case PatchTemplate::ReturnEarly:
    return {0x60000000u, BLR_INSTRUCTION};
  }
  return {};
}

std::vector<std::string> BuildARCodeLines(u32 address, std::span<const u32> words)
{
  std::vector<std::string> lines;
  lines.reserve(words.size());
  for (size_t i = 0; i < words.size(); ++i)
  {
    const u32 target = address + static_cast<u32>(i * 4);
    const u32 ar_address = 0x04000000u | (target & 0x01FFFFFFu);
    lines.push_back(fmt::format("{:08X} {:08X}", ar_address, words[i]));
  }
  return lines;
}

std::string BuildARCodeText(u32 address, std::span<const u32> words)
{
  return fmt::format("{}", fmt::join(BuildARCodeLines(address, words), "\n"));
}
}  // namespace AgentServer
