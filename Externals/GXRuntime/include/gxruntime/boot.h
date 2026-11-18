// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_BOOT_H
#define GXRUNTIME_BOOT_H

#include "core/cpu.h"
#include "gxruntime/loader.h"

// Populate the GameCube low-memory OS globals that game code reads during
// early boot (memory size, clock speeds, console type, arena bounds), and set
// up an initial stack pointer. This mirrors the state the IPL/apploader leaves
// behind before jumping to the DOL entry point.
void boot_setup_os_globals(CPUState* cpu, const DolLayout* layout);

#endif
