// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace AgentServer
{
// Function-entry stubs used to switch game code off: the first word followed by blr makes the
// function return at once. The li forms also set the return value (r3); nop+blr leaves r3 as the
// first argument. These are the patterns the old Dolphin VR culling brute forcer tried.
enum class PatchTemplate
{
  ReturnTrue,   // li r3,1 ; blr
  ReturnFalse,  // li r3,0 ; blr
  ReturnEarly,  // nop ; blr
};

constexpr u32 BLR_INSTRUCTION = 0x4E800020u;

std::optional<PatchTemplate> ParsePatchTemplate(std::string_view name);
std::string_view GetPatchTemplateName(PatchTemplate patch_template);
// The two words written at the function entry.
std::vector<u32> GetPatchTemplateWords(PatchTemplate patch_template);

// Action Replay "04" (32-bit write) lines writing `words` from `address` on, one line per word,
// e.g. "04052FE8 38600001".
std::vector<std::string> BuildARCodeLines(u32 address, std::span<const u32> words);
std::string BuildARCodeText(u32 address, std::span<const u32> words);
}  // namespace AgentServer
