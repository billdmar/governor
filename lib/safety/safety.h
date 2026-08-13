/*
 * safety.h — governor safety state machine (host-portable, NO Zephyr includes).
 *
 *  core (never delegated). Implements docs/SAFETY_SM.md exactly:
 * states INIT/RUN/DEGRADED/SAFE_STOP/FAULT_HALT, transitions T0..T12, the
 * actuation-authority invariants, and the canonical fault-flag bitmask that
 * telemetry encodes into the HEARTBEAT/DATA records (PROTOCOL_SPEC §3).
 *
 * The SM is a pure function of (current state, event, elapsed ticks): it never
 * allocates, never blocks, and one step is bounded work. Time is injected as a
 * monotonic millisecond tick so the whole thing is unit-testable on the host.
 */
#ifndef GOV_SAFETY_H
#define GOV_SAFETY_H

#include <stdbool.h>
#include <stdint.h>

/* Canonical fault-flag bitmask, shared verbatim with lib/telem so both agree
 * (reconciled at ). The SM raises these; telemetry encodes them. Build
 * include paths add -I lib/common. */
#include "gov_faults.h"

/* ---- States (docs/SAFETY_SM.md §2) ------------------------------------- */
typedef enum {
	GOV_ST_INIT = 0,
	GOV_ST_RUN,
	GOV_ST_DEGRADED,
	GOV_ST_SAFE_STOP,
	GOV_ST_FAULT_HALT,
	GOV_ST_COUNT
} gov_state_t;

/* ---- Events / triggers (1:1 with the T# rows) -------------------------- */
typedef enum {
	GOV_EV_POWER_ON = 0,       /* T0  */
	GOV_EV_SELFTEST_OK,        /* T1  */
	GOV_EV_SELFTEST_FAIL,      /* T2  */
	GOV_EV_SENSOR_FAULT,       /* T3  (recoverable sensor fault) */
	GOV_EV_LINK_FAULT,         /* T4  (retransmit exhausted) */
	GOV_EV_DIVERGE,            /* T5  (plant diverging, critical) */
	GOV_EV_CAUSE_CLEARED,      /* T6  (fault cause cleared) */
	GOV_EV_SECOND_FAULT,       /* T7  (independent fault while degraded) */
	GOV_EV_OPERATOR_STOP,      /* T8  (operator e-stop) */
	GOV_EV_OPERATOR_CLEAR,     /* T9  (operator clear from SAFE_STOP) */
	GOV_EV_DEADLINE_MISS,      /* T10 (control deadline misses) */
	GOV_EV_WATCHDOG,           /* T11 (hardware watchdog fired) */
	GOV_EV_INTERNAL_VIOLATION, /* T12 (SM invariant violation) */
	GOV_EV_TICK,               /* time-only event: drive timeouts (T6/T7 dwell) */
	GOV_EV_COUNT
} gov_event_t;

/* Fault-flag bits are defined in the shared gov_faults.h (included above). */

/* ---- Timing / output bounds (config/registry.md §1) -------------------- */
#define GOV_INIT_TIMEOUT_MS   2000u
#define GOV_RECOVER_MS         500u
#define GOV_DEGRADE_MAX_MS    5000u
#define GOV_SAFE_OUTPUT          0   /* safe actuator value */
#define GOV_DEGRADED_CLAMP_NUM   1   /* clamp = 50% => num/den */
#define GOV_DEGRADED_CLAMP_DEN   2

/* ---- SM instance (all static; no allocation) --------------------------- */
typedef struct {
	gov_state_t state;
	uint32_t    faults;        /* sticky-ish bitmask of active fault flags */
	uint32_t    sticky;        /* flags that persist across a reset (watchdog) */
	uint32_t    now_ms;        /* last tick fed in */
	uint32_t    state_since_ms;/* when we entered the current state */
	uint32_t    cause_clear_ms;/* when the recover dwell (T6) started; 0=not */
	uint32_t    init_start_ms; /* when INIT began (T2 timeout) */
	uint32_t    rejected_events;/* stat_sm_rejected_event (SAFETY_SM §3) */
	uint32_t    transitions;   /* count of accepted transitions (telemetry) */
} gov_safety_t;

/* ---- API (frozen names — docs/SAFETY_SM.md §5) ------------------------- */
void        gov_safety_init(gov_safety_t *s);
/* Reboot init that carries a sticky fault read from persistent storage (only
 * GOV_FAULT_WATCHDOG is honored). Boot code uses this after a watchdog reset so
 * telemetry reports the reset cause (SAFETY_SM T11). */
void        gov_safety_init_sticky(gov_safety_t *s, uint32_t sticky);
/* Step the SM with an event at time now_ms (monotonic). Returns the new state.
 * GOV_EV_TICK advances time only and services dwell timers (T2/T6/T7). Every
 * non-tick event also updates now_ms. */
gov_state_t gov_safety_step(gov_safety_t *s, gov_event_t ev, uint32_t now_ms);
bool        gov_safety_actuation_allowed(const gov_safety_t *s);
int32_t     gov_safety_clamp_output(const gov_safety_t *s, int32_t desired);
uint32_t    gov_safety_fault_flags(const gov_safety_t *s);
gov_state_t gov_safety_state(const gov_safety_t *s);
const char *gov_safety_state_name(gov_state_t st);

#endif /* GOV_SAFETY_H */
