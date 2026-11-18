// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Common/Config/Config.h"

namespace Config
{
extern const Info<bool> MAIN_STATICRECOMP_MODULE;
extern const Info<u32> MAIN_STATICRECOMP_IDLE_PC;
extern const Info<std::string> MAIN_STATICRECOMP_FALLBACK_RANGES;
}  // namespace Config
