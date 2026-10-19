// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Pure helpers over the game's code: the instruction counts of the function profile, and the OS
// module list (the REL files the game loaded), which tells the module of an address.

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace AgentServer
{
struct InstructionCounts
{
  u32 fcmp = 0;          // fcmpu and fcmpo: a frustum or distance test
  u32 bool_results = 0;  // li r3,0 and li r3,1: a test's result, often set in several places
  u32 returns = 0;       // blr and the conditional bclr forms (not blrl, a call)
};
InstructionCounts CountInstructions(std::span<const u32> words);

struct ModuleSection
{
  u32 address = 0;
  u32 size = 0;
  bool executable = false;
};

struct LoadedModule
{
  u32 header = 0;  // the OSModuleInfo (the loaded REL header)
  u32 id = 0;
  std::string name;  // the file name kept in the header, without its directory
  std::vector<ModuleSection> sections;
};

// Reads a big-endian word of emulated memory; nothing when the address is not mapped.
using ReadWordFn = std::function<std::optional<u32>(u32 address)>;

// The OS global holding the first loaded module (the modules link through their headers).
constexpr u32 OS_MODULE_LIST_HEAD = 0x800030C8;

// The modules in load order. Stops at the end of the list, at a broken header or at a loop.
std::vector<LoadedModule> ReadLoadedModules(const ReadWordFn& read);

struct ModuleLocation
{
  size_t module = 0;
  size_t section = 0;
  u32 offset = 0;  // from the section start
};
std::optional<ModuleLocation> FindModuleLocation(std::span<const LoadedModule> modules,
                                                 u32 address);
}  // namespace AgentServer
