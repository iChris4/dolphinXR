// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_EVENT_CLOCK_H
#define GXRUNTIME_EVENT_CLOCK_H

#include "core/types.h"

#define DOL_EVENT_CLOCK_MAX_EVENTS 32u

typedef struct DolEventClockEvent {
    u32 id;
    u64 deadline;
    u64 period;
    bool active;
} DolEventClockEvent;

typedef struct DolEventClock {
    u64 now;
    // Cached earliest active deadline, so pop_due can early-out without scanning
    // the event array on the overwhelmingly common not-yet-due dispatch. Kept in
    // sync by schedule/cancel/pop; `advance` only moves `now` and never changes a
    // deadline, so it leaves the cache valid. `has_next` is false when no event
    // is active.
    u64 next_deadline;
    bool has_next;
    DolEventClockEvent events[DOL_EVENT_CLOCK_MAX_EVENTS];
} DolEventClock;

void dol_event_clock_init(DolEventClock* clock);
u64 dol_event_clock_now(const DolEventClock* clock);
void dol_event_clock_advance(DolEventClock* clock, u64 work_units);

// Schedule an event `delay` work units from the current clock. A non-zero
// `period` makes the event periodic; each due pop advances its deadline by one
// period, so callers can observe every elapsed tick after a large advance.
bool dol_event_clock_schedule(DolEventClock* clock, u32 id, u64 delay, u64 period);
bool dol_event_clock_cancel(DolEventClock* clock, u32 id);
bool dol_event_clock_is_scheduled(const DolEventClock* clock, u32 id);
bool dol_event_clock_next_deadline(const DolEventClock* clock, u64* deadline);

// Pop the earliest due event at or before `now`. Ties are deterministic:
// lower slot index wins, which is insertion order unless an id is rescheduled.
bool dol_event_clock_pop_due(DolEventClock* clock, u32* id, u64* deadline);

#endif
