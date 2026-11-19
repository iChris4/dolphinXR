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

// DolRecomp --function-chunks: every guest call is a host call, so generated code stops calling
// (and returns to the chassis) below this stack address. Set at each entry from the chassis, a
// budget below that frame; the CPU thread's stack (512 KB on Darwin) keeps the rest for the
// chassis's own calls from hooks.
#define MODULE_STACK_BUDGET (192u * 1024u)
char* dolrecomp_stack_floor;

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    dolrecomp_stack_floor = (char*)__builtin_frame_address(0) - MODULE_STACK_BUDGET;
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

// DolRecomp --function-chunks: call sites name their target by its index in this module's chunk
// table (DolRecomp sorts the chunks as gen_module_tables.py does); the start confirms it.
int dolrecomp_chunk_ready_at(u32 index, u32 chunk_start)
{
    if (__builtin_expect(index < s_chunk_state_count && s_chunk_ranges[index].start == chunk_start, 1))
        return s_chunk_states[index] == STATICRECOMP_CHUNK_VERIFIED;
    return s_chunk_states ? chunk_ready_search(chunk_start) : 0;
}

// The chunk containing any guest address (indirect calls): the first chunk ending past each
// 256-byte page of guest code, then a few steps.
#define ADDR_PAGE_SHIFT 8u
static u32* s_addr_pages;
static u32 s_addr_page_base;
static u32 s_addr_page_count;

static u32 chunk_index_at(u32 address)
{
    const u32 page = (address - s_addr_page_base) >> ADDR_PAGE_SHIFT;
    if (page >= s_addr_page_count)
        return ~0u;
    u32 i = s_addr_pages[page];
    while (i < MODULE_CHUNK_RANGE_COUNT && s_chunk_ranges[i].end <= address)
        ++i;
    return i < MODULE_CHUNK_RANGE_COUNT && s_chunk_ranges[i].start <= address ? i : ~0u;
}

// An indirect call or jump (bctrl, bctr out of the function) in --function-chunks code: run the
// chunk holding `target` when the chassis has verified it and the stack allows; 0 otherwise, and
// the generated code returns to the chassis, which dispatches it.
int dolrecomp_call_indirect(CPUState* ctx, u32 target)
{
    const u32 i = chunk_index_at(target);
    if (i >= s_chunk_state_count || s_chunk_states[i] != STATICRECOMP_CHUNK_VERIFIED ||
        (char*)__builtin_frame_address(0) <= dolrecomp_stack_floor)
        return 0;
    ctx->pc = target;
    s_chunk_funcs[i](ctx);
    return 1;
}

static void build_addr_pages(void)
{
    free(s_addr_pages);
    s_addr_pages = NULL;
    s_addr_page_count = 0;
    if (!s_chunk_states || MODULE_CHUNK_RANGE_COUNT == 0)
        return;
    const u32 base = s_chunk_ranges[0].start & ~((1u << ADDR_PAGE_SHIFT) - 1u);
    const u32 count =
        ((s_chunk_ranges[MODULE_CHUNK_RANGE_COUNT - 1].end - 1u - base) >> ADDR_PAGE_SHIFT) + 1u;
    u32* pages = (u32*)malloc((size_t)count * sizeof(u32));
    if (!pages)
        return;
    u32 i = 0;
    for (u32 page = 0; page < count; ++page)
    {
        const u32 page_start = base + (page << ADDR_PAGE_SHIFT);
        while (i < MODULE_CHUNK_RANGE_COUNT && s_chunk_ranges[i].end <= page_start)
            ++i;
        pages[page] = i;
    }
    s_addr_page_base = base;
    s_addr_pages = pages;
    s_addr_page_count = count;
}

static void build_chunk_pages(void)
{
    free(s_chunk_pages);
    s_chunk_pages = NULL;
    s_chunk_page_count = 0;
    build_addr_pages();
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
        if (!taken[page])
        {
            taken[page] = 1;
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
    // Lookups stop first (no page tables), then the states change, then the tables are rebuilt.
    s_chunk_page_count = 0;
    s_addr_page_count = 0;
    s_chunk_state_count = 0;
    s_chunk_states = states;
    s_chunk_state_count = count;
    build_chunk_pages();
}
