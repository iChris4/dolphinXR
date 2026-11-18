// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_AURORA_BACKEND_H
#define GXRUNTIME_AURORA_BACKEND_H

#include "gxruntime/platform.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AuroraBackendConfig {
    const char* app_name;
    unsigned window_width;
    unsigned window_height;
    bool vsync;
    bool allow_texture_dumps;
    bool info_logging;
    bool graphics_logging;
    bool force_untextured;
} AuroraBackendConfig;

bool dol_aurora_initialize(int argc, char** argv,
                           const AuroraBackendConfig* config);
void dol_aurora_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
