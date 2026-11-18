// The dispatcher of a REL recompiled into the module (Wii games: Mario Kart Wii's StaticR.rel).
// Built against the REL tree's generated.h, as its chunks are: each DolRecomp tree has its own
// dispatch tables in its header. module_export.c sends the REL's addresses here.

#include "generated.h"

int module_rel_dispatch(CPUState* ctx, u32 address)
{
    return dolrecomp_call(ctx, address);
}
