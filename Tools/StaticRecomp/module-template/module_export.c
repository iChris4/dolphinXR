// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

#include "module_tables.inc"

#if MODULE_HAS_REL
// The REL tree's dispatcher (module_rel_dispatch.c, built against the REL's generated.h).
int module_rel_dispatch(CPUState* ctx, u32 address);
#endif

static int chassis_dispatch(CPUState* ctx, u32 address)
{
#if MODULE_HAS_REL
    if (address >= MODULE_REL_CODE_START && address < MODULE_REL_CODE_END)
        return module_rel_dispatch(ctx, address);
#endif
    return dolrecomp_call(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

// Guarded direct cross-chunk calls (DolRecomp --direct-calls): the chassis's per-chunk states,
// and the check every such call makes before running another chunk natively.
static const u8* s_chunk_states;
static u32 s_chunk_state_count;

int dolrecomp_chunk_ready(u32 chunk_start)
{
    if (!s_chunk_states)
        return 0;
    u32 lo = 0, hi = MODULE_CHUNK_RANGE_COUNT;
    while (lo < hi)
    {
        const u32 mid = lo + (hi - lo) / 2u;
        if (s_chunk_ranges[mid].start < chunk_start)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < s_chunk_state_count && s_chunk_ranges[lo].start == chunk_start &&
           s_chunk_states[lo] == STATICRECOMP_CHUNK_VERIFIED;
}

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
    MODULE_REL_MODULES,
    MODULE_REL_MODULE_COUNT,
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

RECOMP_MODULE_EXPORT void staticrecomp_set_chunk_states(const u8* states, u32 count)
{
    s_chunk_states = states;
    s_chunk_state_count = count;
}
