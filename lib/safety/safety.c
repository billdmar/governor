/*
 * safety.c — governor safety state machine implementation.
 *
 * Implements docs/SAFETY_SM.md exactly. Design notes:
 *  - Single authority on actuation (invariant 1): only RUN (full) and DEGRADED
 *    (clamped) allow output; INIT/SAFE_STOP/FAULT_HALT force GOV_SAFE_OUTPUT.
 *  - SAFE_STOP and FAULT_HALT latch (invariant 2): only the specified operator
 *    clear / reset leaves them. No timeout auto-exits a safe state.
 *  - Every accepted transition sets a fault flag (invariant 4). An event with
 *    no matching row is NOT silently ignored: it is counted, and a fault event
 *    that should have matched is escalated to an internal violation (T12).
 *  - Pure logic: no allocation, no blocking, no OS/RTOS calls. Time is injected.
 */
#include "safety.h"

#include <stddef.h> /* NULL */

/* Enter a new state, stamping the entry time and bumping the transition count.
 * Kept tiny and side-effect-explicit so transitions are auditable. */
static void enter(gov_safety_t *s, gov_state_t next, uint32_t now_ms)
{
	s->state = next;
	s->state_since_ms = now_ms;
	s->transitions++;
	/* Leaving DEGRADED cancels any in-progress recover dwell. */
	if (next != GOV_ST_DEGRADED) {
		s->cause_clear_ms = 0;
	}
}

/* Latch into SAFE_STOP with the given fault flag (T2/T5/T7/T8). */
static void to_safe_stop(gov_safety_t *s, uint32_t flag, uint32_t now_ms)
{
	s->faults |= flag;
	enter(s, GOV_ST_SAFE_STOP, now_ms);
}

/* Latch into FAULT_HALT (T12). Terminal until reset. */
static void to_fault_halt(gov_safety_t *s, uint32_t now_ms)
{
	s->faults |= GOV_FAULT_INTERNAL;
	enter(s, GOV_ST_FAULT_HALT, now_ms);
}

void gov_safety_init(gov_safety_t *s)
{
	/* Clean power-on init with no carried-over sticky faults. Does NOT read
	 * any field before writing it (the struct may be uninitialized). To model
	 * a reboot that must carry a sticky bit (e.g. a watchdog reset recorded in
	 * persistent storage), use gov_safety_init_sticky(). */
	gov_safety_init_sticky(s, GOV_FAULT_NONE);
}

void gov_safety_init_sticky(gov_safety_t *s, uint32_t sticky)
{
	if (s == NULL) {
		return;
	}
	/* Only genuine sticky bits are allowed to survive a reset. */
	sticky &= GOV_FAULT_WATCHDOG;
	s->state = GOV_ST_INIT;
	s->faults = sticky; /* sticky faults are visible in telemetry at boot */
	s->sticky = sticky;
	s->now_ms = 0u;
	s->state_since_ms = 0u;
	s->cause_clear_ms = 0u;
	s->init_start_ms = 0u;
	s->rejected_events = 0u;
	s->transitions = 0u;
}

/* Count a rejected/unhandled event (SAFETY_SM §3 self-loop policy). */
static void reject(gov_safety_t *s)
{
	s->rejected_events++;
}

/* Service time-driven transitions for the current state (T2 timeout, T6 recover
 * dwell, T7 degrade-max). Called on every step after now_ms is updated. */
static void service_timers(gov_safety_t *s, uint32_t now_ms)
{
	switch (s->state) {
	case GOV_ST_INIT:
		/* T2: init that never completes self-test within the timeout. */
		if ((now_ms - s->init_start_ms) >= GOV_INIT_TIMEOUT_MS) {
			to_safe_stop(s, GOV_FAULT_INIT, now_ms);
		}
		break;
	case GOV_ST_DEGRADED:
		/* T7: a degraded condition that persists too long escalates. */
		if ((now_ms - s->state_since_ms) >= GOV_DEGRADE_MAX_MS) {
			to_safe_stop(s, GOV_FAULT_ESCALATED, now_ms);
			break;
		}
		/* T6: if a recover dwell is running and it has elapsed, go RUN. */
		if (s->cause_clear_ms != 0u &&
		    (now_ms - s->cause_clear_ms) >= GOV_RECOVER_MS) {
			/* Clear the recoverable (non-sticky) faults on recovery. */
			s->faults &= (GOV_FAULT_WATCHDOG | s->sticky);
			enter(s, GOV_ST_RUN, now_ms);
		}
		break;
	default:
		break; /* RUN/SAFE_STOP/FAULT_HALT have no time-driven exits */
	}
}

gov_state_t gov_safety_step(gov_safety_t *s, gov_event_t ev, uint32_t now_ms)
{
	if (s == NULL) {
		return GOV_ST_FAULT_HALT;
	}
	/* Monotonic-time guard: time must not go backwards. A violation is a real
	 * internal fault, not something to paper over. */
	if (now_ms < s->now_ms) {
		to_fault_halt(s, s->now_ms);
		return s->state;
	}
	s->now_ms = now_ms;

	/* An internal-violation event forces FAULT_HALT from anywhere (T12). */
	if (ev == GOV_EV_INTERNAL_VIOLATION) {
		to_fault_halt(s, now_ms);
		return s->state;
	}
	/* Watchdog (T11): fires from any operational state. Sets a sticky bit and
	 * (models the reset by) re-entering INIT; telemetry reports the cause. */
	if (ev == GOV_EV_WATCHDOG) {
		s->sticky |= GOV_FAULT_WATCHDOG;
		s->faults |= GOV_FAULT_WATCHDOG;
		s->init_start_ms = now_ms;
		enter(s, GOV_ST_INIT, now_ms);
		return s->state;
	}
	/* Operator e-stop (T8): from any operational state → latched SAFE_STOP. */
	if (ev == GOV_EV_OPERATOR_STOP &&
	    (s->state == GOV_ST_RUN || s->state == GOV_ST_DEGRADED ||
	     s->state == GOV_ST_INIT)) {
		to_safe_stop(s, GOV_FAULT_OPERATOR_STOP, now_ms);
		return s->state;
	}

	switch (s->state) {
	case GOV_ST_INIT:
		switch (ev) {
		case GOV_EV_POWER_ON: /* T0 — (re)stamp the init/self-test timer */
			s->init_start_ms = now_ms;
			s->state_since_ms = now_ms;
			break;
		case GOV_EV_SELFTEST_OK: /* T1 */
			enter(s, GOV_ST_RUN, now_ms);
			break;
		case GOV_EV_SELFTEST_FAIL: /* T2 */
			to_safe_stop(s, GOV_FAULT_INIT, now_ms);
			break;
		case GOV_EV_TICK:
			break; /* timers serviced below */
		default:
			reject(s);
			break;
		}
		break;

	case GOV_ST_RUN:
		switch (ev) {
		case GOV_EV_SENSOR_FAULT: /* T3 */
			s->faults |= GOV_FAULT_SENSOR_ACTIVE;
			enter(s, GOV_ST_DEGRADED, now_ms);
			break;
		case GOV_EV_LINK_FAULT: /* T4 */
			s->faults |= GOV_FAULT_LINK;
			enter(s, GOV_ST_DEGRADED, now_ms);
			break;
		case GOV_EV_DEADLINE_MISS: /* T10 */
			s->faults |= GOV_FAULT_TIMING;
			enter(s, GOV_ST_DEGRADED, now_ms);
			break;
		case GOV_EV_DIVERGE: /* T5 */
			to_safe_stop(s, GOV_FAULT_DIVERGE, now_ms);
			break;
		case GOV_EV_TICK:
			break;
		default:
			reject(s);
			break;
		}
		break;

	case GOV_ST_DEGRADED:
		switch (ev) {
		case GOV_EV_CAUSE_CLEARED: /* T6 — start the recover dwell */
			if (s->cause_clear_ms == 0u) {
				s->cause_clear_ms = now_ms;
			}
			break;
		case GOV_EV_SENSOR_FAULT: /* re-assert; restart dwell */
		case GOV_EV_LINK_FAULT:
		case GOV_EV_DEADLINE_MISS:
			s->cause_clear_ms = 0u; /* cause is back; cancel recovery */
			if (ev == GOV_EV_LINK_FAULT) {
				s->faults |= GOV_FAULT_LINK;
			} else if (ev == GOV_EV_DEADLINE_MISS) {
				s->faults |= GOV_FAULT_TIMING;
			} else {
				s->faults |= GOV_FAULT_SENSOR_ACTIVE;
			}
			break;
		case GOV_EV_SECOND_FAULT: /* T7 — independent second fault escalates */
			to_safe_stop(s, GOV_FAULT_ESCALATED, now_ms);
			break;
		case GOV_EV_DIVERGE: /* T5 still applies while degraded */
			to_safe_stop(s, GOV_FAULT_DIVERGE, now_ms);
			break;
		case GOV_EV_TICK:
			break;
		default:
			reject(s);
			break;
		}
		break;

	case GOV_ST_SAFE_STOP:
		switch (ev) {
		case GOV_EV_OPERATOR_CLEAR: /* T9 — only exit; re-run bring-up */
			s->faults = s->sticky; /* clear latched faults; keep sticky */
			s->init_start_ms = now_ms;
			enter(s, GOV_ST_INIT, now_ms);
			break;
		case GOV_EV_TICK:
			break; /* latched: no timeout exit */
		default:
			reject(s);
			break;
		}
		break;

	case GOV_ST_FAULT_HALT:
		/* Terminal: only a real reset (gov_safety_init) leaves it. */
		if (ev != GOV_EV_TICK) {
			reject(s);
		}
		break;

	default:
		/* Impossible state => invariant violation (T12). */
		to_fault_halt(s, now_ms);
		return s->state;
	}

	service_timers(s, now_ms);
	return s->state;
}

bool gov_safety_actuation_allowed(const gov_safety_t *s)
{
	if (s == NULL) {
		return false;
	}
	return (s->state == GOV_ST_RUN) || (s->state == GOV_ST_DEGRADED);
}

int32_t gov_safety_clamp_output(const gov_safety_t *s, int32_t desired)
{
	if (s == NULL) {
		return GOV_SAFE_OUTPUT;
	}
	switch (s->state) {
	case GOV_ST_RUN:
		return desired; /* full authority */
	case GOV_ST_DEGRADED: {
		/* Clamp magnitude to GOV_DEGRADED_CLAMP (50%) around the safe value.
		 * Symmetric clamp about GOV_SAFE_OUTPUT. */
		int32_t span = desired - GOV_SAFE_OUTPUT;
		int32_t limited = (span * GOV_DEGRADED_CLAMP_NUM) /
				  GOV_DEGRADED_CLAMP_DEN;
		return GOV_SAFE_OUTPUT + limited;
	}
	case GOV_ST_INIT:
	case GOV_ST_SAFE_STOP:
	case GOV_ST_FAULT_HALT:
	default:
		return GOV_SAFE_OUTPUT; /* actuation inhibited */
	}
}

uint32_t gov_safety_fault_flags(const gov_safety_t *s)
{
	return (s == NULL) ? GOV_FAULT_INTERNAL : s->faults;
}

gov_state_t gov_safety_state(const gov_safety_t *s)
{
	return (s == NULL) ? GOV_ST_FAULT_HALT : s->state;
}

const char *gov_safety_state_name(gov_state_t st)
{
	switch (st) {
	case GOV_ST_INIT:      return "INIT";
	case GOV_ST_RUN:       return "RUN";
	case GOV_ST_DEGRADED:  return "DEGRADED";
	case GOV_ST_SAFE_STOP: return "SAFE_STOP";
	case GOV_ST_FAULT_HALT:return "FAULT_HALT";
	case GOV_ST_COUNT:
	default:               return "?";
	}
}
