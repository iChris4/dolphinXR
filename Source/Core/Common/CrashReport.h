// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Common
{
// On Windows, a crash that nothing handles (an access violation outside the JIT's fastmem) writes
// a report to `directory`: crash-<time>.txt, with the exception and the crashing thread's stack
// named from the program's PDB when it is found, and crash-<time>.dmp, a minidump. The crash then
// goes on to Windows as before. Nothing elsewhere. Call once, when the user directory is known.
void InstallCrashReporter(const std::string& directory);
}  // namespace Common
