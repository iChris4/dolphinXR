// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Common/Config/Config.h"

namespace Config
{
extern const Info<bool> MAIN_STATICRECOMP_MODULE;
extern const Info<u32> MAIN_STATICRECOMP_IDLE_PC;
// Busy-wait loops found automatically are skipped (CoreTiming::Idle). A game's settings can turn
// it off ([Core] StaticRecompIdleSkip = False) if skipping misbehaves there.
extern const Info<bool> MAIN_STATICRECOMP_IDLE_SKIP;
extern const Info<std::string> MAIN_STATICRECOMP_FALLBACK_RANGES;
}  // namespace Config
