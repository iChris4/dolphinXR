// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include <stdlib.h>

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

// Every direct call asks first, so the lookup must not be a search: chunk starts by 4 KB page of
// guest code (a page where two chunks start keeps the binary search), built with the states.
#define CHUNK_PAGE_SHIFT 12u
typedef struct ChunkPage
{
    u32 start;  // the chunk starting in this page; 0 when none or more than one
    u32 index;
} ChunkPage;
static ChunkPage* s_chunk_pages;
static u32 s_chunk_page_base;
static u32 s_chunk_page_count;

static int chunk_ready_search(u32 chunk_start)
{
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

int dolrecomp_chunk_ready(u32 chunk_start)
{
    const u32 page = (chunk_start - s_chunk_page_base) >> CHUNK_PAGE_SHIFT;
    if (__builtin_expect(page < s_chunk_page_count, 1))
    {
        const ChunkPage entry = s_chunk_pages[page];
        if (__builtin_expect(entry.start == chunk_start, 1))
            return s_chunk_states[entry.index] == STATICRECOMP_CHUNK_VERIFIED;
    }
    return s_chunk_states ? chunk_ready_search(chunk_start) : 0;
}

static void build_chunk_pages(void)
{
    free(s_chunk_pages);
    s_chunk_pages = NULL;
    s_chunk_page_count = 0;
    if (!s_chunk_states || MODULE_CHUNK_RANGE_COUNT == 0)
        return;
    const u32 base = s_chunk_ranges[0].start & ~((1u << CHUNK_PAGE_SHIFT) - 1u);
    const u32 count =
        ((s_chunk_ranges[MODULE_CHUNK_RANGE_COUNT - 1].start - base) >> CHUNK_PAGE_SHIFT) + 1u;
    ChunkPage* pages = (ChunkPage*)calloc(count, sizeof(ChunkPage));
    if (!pages)
        return;
    u8* taken = (u8*)calloc(count, 1);
    if (!taken)
    {
        free(pages);
        return;
    }
    for (u32 i = 0; i < MODULE_CHUNK_RANGE_COUNT && i < s_chunk_state_count; ++i)
    {
        const u32 page = (s_chunk_ranges[i].start - base) >> CHUNK_PAGE_SHIFT;
        if (taken[page]++ == 0)
        {
            pages[page].start = s_chunk_ranges[i].start;
            pages[page].index = i;
        }
        else
        {
            pages[page].start = 0;  // shared page: binary search
        }
    }
    free(taken);
    s_chunk_page_base = base;
    s_chunk_pages = pages;
    s_chunk_page_count = count;
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
    // Lookups stop first (no page table), then the states change, then the table is rebuilt.
    s_chunk_page_count = 0;
    s_chunk_states = states;
    s_chunk_state_count = count;
    build_chunk_pages();
}
