// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/Config/StaticRecompSettings.h"
#include "Core/System.h"

namespace Config
{
const Info<bool> MAIN_STATICRECOMP_MODULE{{System::Main, "Core", "StaticRecompModule"}, true};
const Info<u32> MAIN_STATICRECOMP_IDLE_PC{{System::Main, "Core", "StaticRecompIdlePC"}, 0};
const Info<std::string> MAIN_STATICRECOMP_FALLBACK_RANGES{
    {System::Main, "Core", "StaticRecompFallbackRanges"}, ""};
}  // namespace Config
