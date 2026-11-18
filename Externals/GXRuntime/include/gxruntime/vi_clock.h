// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_VI_CLOCK_H
#define GXRUNTIME_VI_CLOCK_H

#include "core/types.h"
#include "gxruntime/event_clock.h"

#define DOL_VI_DEFAULT_REFRESH_HZ 60u
#define DOL_VI_DEFAULT_TIMEBASE_HZ 40500000ull

typedef struct DolViClock {
    DolEventClock events;
    u64 work_units_per_retrace;
    u32 refresh_hz;
    u64 timebase_hz;
    u64 timebase_ticks_per_retrace;
} DolViClock;

void dol_vi_clock_init(DolViClock* vi);
void dol_vi_clock_configure(DolViClock* vi, u64 work_units_per_retrace,
                            u32 refresh_hz, u64 timebase_hz);
void dol_vi_clock_advance(DolViClock* vi, u64 work_units);

// Pop one due retrace. Returns the guest timebase ticks represented by this
// retrace through `timebase_ticks`, if non-null.
bool dol_vi_clock_pop_retrace(DolViClock* vi, u64* timebase_ticks);

u64 dol_vi_clock_now(const DolViClock* vi);
u64 dol_vi_clock_work_units_per_retrace(const DolViClock* vi);
u32 dol_vi_clock_refresh_hz(const DolViClock* vi);
u64 dol_vi_clock_timebase_ticks_per_retrace(const DolViClock* vi);

#endif
