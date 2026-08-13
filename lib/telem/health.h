/*
 * health.h -- small health aggregator (pure logic, ticks injected, no OS calls).
 *
 * Backs the `health` task (TASKS sec 2, sec 4): it tracks per-subsystem liveness
 * (last-seen tick) and consecutive control-deadline misses, and exposes the
 * signals the safety SM + watchdog-feed decision consume. It does NOT implement
 * the safety state machine ( owns lib/safety) -- it only produces inputs:
 *
 *   - gov_health_all_alive()   : every tracked subsystem seen within threshold
 *   - gov_health_timing_fault(): control missed >= GOV_MISS_LIMIT deadlines
 *                                in a row (SAFETY_SM T10 trigger, registry F11)
 *   - gov_health_feed_watchdog(): the composite "system scheduling correctly"
 *                                 decision the health task uses to feed the dog
 *
 * Time is a caller-supplied monotonic tick (same units as the control tick);
 * this module never reads a clock. No allocation (R1), no recursion (R7).
 */
#ifndef GOV_TELEM_HEALTH_H
#define GOV_TELEM_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telem_config.h"

/*
 * Tracked subsystems. Kept small and fixed; the array is sized by
 * GOV_HEALTH_SUBSYS_COUNT so adding one is a single enum edit.
 */
enum gov_health_subsys {
	GOV_HS_CONTROL = 0, /* the 100 Hz control loop      */
	GOV_HS_LINK,        /* the link rx/tx path          */
	GOV_HS_SENSOR,      /* the sensor/driver path       */
	GOV_HEALTH_SUBSYS_COUNT
};

struct gov_health {
	/* last tick at which each subsystem reported alive; seen[] gates it. */
	uint32_t last_seen[GOV_HEALTH_SUBSYS_COUNT];
	bool seen[GOV_HEALTH_SUBSYS_COUNT];
	uint32_t consec_misses; /* consecutive control-deadline misses */
};

/* Reset all liveness state; no subsystem is "seen" yet. */
void gov_health_init(struct gov_health *h);

/*
 * Record that `sys` is alive as of `tick`. Called on each subsystem heartbeat.
 * Out-of-range `sys` is ignored (defensive; caller passes the enum).
 */
void gov_health_note_alive(struct gov_health *h, enum gov_health_subsys sys, uint32_t tick);

/*
 * Record one control-loop deadline result. `met` true resets the consecutive
 * miss counter; false increments it. This is the T10 miss detector.
 */
void gov_health_note_deadline(struct gov_health *h, bool met);

/*
 * True iff every tracked subsystem has been seen at least once AND its last
 * report is within `threshold` ticks of `now` (i.e. none is silent). A
 * subsystem never seen counts as NOT alive.
 */
bool gov_health_all_alive(const struct gov_health *h, uint32_t now, uint32_t threshold);

/*
 * True iff a single named subsystem is currently alive (seen and within
 * `threshold` of `now`). Convenience for callers that need per-subsystem view.
 */
bool gov_health_subsys_alive(const struct gov_health *h, enum gov_health_subsys sys, uint32_t now,
			     uint32_t threshold);

/*
 * True iff the control loop has missed GOV_MISS_LIMIT or more deadlines in a
 * row -- the timing-fault signal the safety SM turns into T10 / FAULT_TIMING.
 */
bool gov_health_timing_fault(const struct gov_health *h);

/* Current consecutive-miss count (0..). Exposed for telemetry/tests. */
uint32_t gov_health_consec_misses(const struct gov_health *h);

/*
 * Composite watchdog-feed decision (TASKS sec 4): the dog may be fed iff every
 * subsystem is alive AND the control loop is meeting its deadlines. If either
 * fails, the health task withholds the feed and the hardware watchdog is
 * allowed to trip. This module returns the decision only; it performs no I/O.
 */
bool gov_health_feed_watchdog(const struct gov_health *h, uint32_t now, uint32_t threshold);

#endif /* GOV_TELEM_HEALTH_H */
