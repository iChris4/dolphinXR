// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    return dolrecomp_call(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

#include "module_tables.inc"

static const StaticRecompModuleDesc s_desc = {
    STATICRECOMP_ABI_VERSION,
    GXRUNTIME_CPU_ABI_VERSION,
    (u32)sizeof(CPUState),
    MODULE_GAME_ID,
    DOLRECOMP_ENTRY_POINT,
    chassis_dispatch,
    chassis_on_state_loaded,
    s_code_ranges,
    MODULE_CODE_RANGE_COUNT,
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges,
    MODULE_CHUNK_RANGE_COUNT,
    s_chunk_hashes,
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
    return &s_desc;
}
