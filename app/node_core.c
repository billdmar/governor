/*
 * node_core.c — governor node coordinator implementation (host-portable).
 *
 * See node_core.h. This is integration glue: it composes the frozen modules
 * and enforces the ordering, but delegates every decision to the module that
 * owns it (safety owns actuation authority; control owns the PID/divergence
 * report; telem owns encoding).
 */
#include "node_core.h"

#include "control_config.h"

/* PID gains tuned by lib/control (see control_config.h / DESIGN): settle to a
 * ±1 band in ~0.47 s with zero overshoot on the reference plant. */
static const struct gov_pid_cfg PID_CFG = {
	.kp = 3.5f,
	.ki = 18.0f,
	.kd = 0.06f,
	.out_min = GOV_CTRL_OUT_MIN,
	.out_max = GOV_CTRL_OUT_MAX,
	.integ_min = -100.0f,
	.integ_max = 100.0f,
	.diverge_limit = GOV_DIVERGE_LIMIT,
	.diverge_ms = GOV_DIVERGE_MS,
};

/* Reference plant: first-order lag, tau=0.1 s, unit gain, starts at 0. */
static const struct gov_plant_cfg PLANT_CFG = {
	.tau_s = 0.1f,
	.gain = 1.0f,
	.y0 = 0.0f,
};

void gov_node_init(struct gov_node *n, float setpoint)
{
	if (n == NULL) {
		return;
	}
	gov_safety_init(&n->safety);
	gov_pid_init(&n->pid, &PID_CFG);
	gov_plant_init(&n->plant, &PLANT_CFG);
	n->setpoint = setpoint;
	n->tick_ms = 0u;
	n->seq = 0u;
	n->last_output = 0.0f;
	n->last_measurement = 0.0f;
	n->driver_faults = 0u;
	n->active_causes = 0u;
}

/* Resync the coordinator's fault shadow at the recovery boundary (INIT).
 * `active_causes` (which drives first-fault-vs-second-independent-fault routing)
 * and `driver_faults` (the D7 specific-cause bits merged into telemetry) both
 * describe faults tied to the *current* actuation episode. When the SM returns
 * to INIT (a reboot, watchdog reset, or operator CMD_CLEAR out of SAFE_STOP), a
 * fresh bring-up begins, so the shadow must be cleared — otherwise the first
 * fault after recovery would look like a "second independent fault", post
 * GOV_EV_SECOND_FAULT, hit no RUN transition, and be silently rejected (an
 * unflagged fault), and a stale specific bit would leak into the recovered RUN.
 *
 * NOTE: we deliberately do NOT clear in a latched SAFE_STOP/FAULT_HALT — the
 * specific cause bit (e.g. FAULT_BUS_ERR, registry F05) must remain in telemetry
 * so an operator can see *why* the node latched. It is cleared only once the
 * node actually re-runs bring-up (INIT). Call after every SM step. */
static void resync_shadow(struct gov_node *n)
{
	if (gov_safety_state(&n->safety) == GOV_ST_INIT) {
		n->active_causes = 0u;
		n->driver_faults = 0u;
	}
}

/* Shared edge logic for a recoverable fault source (registry F01-F05/F07/F11).
 * Drives the safety SM per the frozen SAFETY_SM transitions:
 *   - rising edge with NO other cause active  → post the fault event (RUN→DEGRADED, T3/T4/T10)
 *   - rising edge with ANOTHER cause already active → post GOV_EV_SECOND_FAULT
 *       (a *second independent* fault while degraded escalates to SAFE_STOP, T7)
 *   - still-faulted (already set): re-assert so any recover dwell is cancelled
 *   - falling edge: clear the cause; post CAUSE_CLEARED only when the LAST cause
 *       clears, so the T6 dwell restores RUN exactly once the node is fully
 *       healthy again (one source clearing while another is still faulted must
 *       NOT recover). */
static void note_cause(struct gov_node *n, uint32_t cause, gov_event_t fault_ev,
		       bool healthy, uint32_t now_ms)
{
	if (!healthy) {
		bool rising = (n->active_causes & cause) == 0u;
		bool other_active = (n->active_causes & ~cause) != 0u;
		n->active_causes |= cause;
		if (rising && other_active) {
			/* T7: a distinct second cause while already degraded. */
			gov_safety_step(&n->safety, GOV_EV_SECOND_FAULT, now_ms);
		} else {
			/* First cause (RUN→DEGRADED) or the same cause persisting
			 * (re-assert cancels a recover dwell). */
			gov_safety_step(&n->safety, fault_ev, now_ms);
		}
	} else if ((n->active_causes & cause) != 0u) {
		/* falling edge: clear this cause; recover only when it was the last */
		n->active_causes &= ~cause;
		if (n->active_causes == 0u) {
			gov_safety_step(&n->safety, GOV_EV_CAUSE_CLEARED, now_ms);
		}
	}
	/* If the step landed in a non-actuating state (e.g. T7 escalation →
	 * SAFE_STOP), drop the shadow so a later recovery starts consistent. */
	resync_shadow(n);
}

void gov_node_note_sensor(struct gov_node *n, uint32_t sflags, bool healthy,
			  uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}
	if (!healthy) {
		/* D7: merge the driver's specific cause bit into telemetry. */
		n->driver_faults |= sflags;
	} else if (gov_safety_actuation_allowed(&n->safety)) {
		/* Clear the specific bits on a healthy read only while actuating
		 * (RUN/DEGRADED) — i.e. a genuine recovery in progress. In a latched
		 * SAFE_STOP/FAULT_HALT the cause must persist so an operator can see
		 * why the node stopped (D21/F05); it is dropped only when the SM
		 * re-runs bring-up (resync_shadow at INIT). */
		n->driver_faults &= ~GOV_FAULT_SENSOR_ANY_DRIVER;
	}
	note_cause(n, GOV_CAUSE_SENSOR, GOV_EV_SENSOR_FAULT, healthy, now_ms);
}

void gov_node_note_link(struct gov_node *n, bool healthy, uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}
	note_cause(n, GOV_CAUSE_LINK, GOV_EV_LINK_FAULT, healthy, now_ms);
}

void gov_node_note_timing(struct gov_node *n, bool healthy, uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}
	note_cause(n, GOV_CAUSE_TIMING, GOV_EV_DEADLINE_MISS, healthy, now_ms);
}

void gov_node_selftest_ok(struct gov_node *n, uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}
	gov_safety_step(&n->safety, GOV_EV_SELFTEST_OK, now_ms);
}

float gov_node_control_step(struct gov_node *n, float measurement, uint32_t now_ms)
{
	if (n == NULL) {
		return (float)GOV_SAFE_OUTPUT;
	}
	n->tick_ms = now_ms;
	n->last_measurement = measurement;

	/* 1) Controller computes the desired actuator command. */
	float desired = gov_pid_step(&n->pid, n->setpoint, measurement,
				     GOV_CTRL_PERIOD_MS);

	/* 2) Sustained divergence is a safety event (SAFETY_SM T5). The PID only
	 *    reports; the SM decides the latched SAFE_STOP. */
	if (gov_pid_diverging(&n->pid)) {
		gov_safety_step(&n->safety, GOV_EV_DIVERGE, now_ms);
	} else {
		gov_safety_step(&n->safety, GOV_EV_TICK, now_ms);
	}
	resync_shadow(n); /* DIVERGE → SAFE_STOP must drop the fault shadow */

	/* 3) The safety SM is the sole actuation authority: clamp/inhibit. */
	int32_t applied_i = gov_safety_clamp_output(&n->safety, (int32_t)desired);
	float applied = (float)applied_i;

	/* 4) Advance the plant under the *applied* output (not the desired). */
	(void)gov_plant_step(&n->plant, applied, GOV_CTRL_PERIOD_MS);
	n->last_output = applied;
	return applied;
}

void gov_node_post_event(struct gov_node *n, gov_event_t ev, uint32_t now_ms)
{
	if (n == NULL) {
		return;
	}
	gov_safety_step(&n->safety, ev, now_ms);
	resync_shadow(n); /* e.g. OPERATOR_CLEAR/WATCHDOG → INIT clears the shadow */
}

void gov_node_fill_telemetry(const struct gov_node *n, struct gov_telem_record *rec)
{
	if (n == NULL || rec == NULL) {
		return;
	}
	rec->tick = n->tick_ms;
	rec->setpoint = (int32_t)n->setpoint;
	rec->measurement = (int32_t)n->last_measurement;
	rec->output = (int32_t)n->last_output;
	rec->state = (uint8_t)gov_safety_state(&n->safety);
	/* D7: SM's coarse bits OR the driver's specific cause bits. */
	rec->fault_flags = gov_safety_fault_flags(&n->safety) | n->driver_faults;
}

void gov_node_fill_heartbeat(const struct gov_node *n, struct gov_heartbeat *hb)
{
	if (n == NULL || hb == NULL) {
		return;
	}
	hb->state = (uint8_t)gov_safety_state(&n->safety);
	/* D7: SM's coarse bits OR the driver's specific cause bits. */
	hb->fault_flags = gov_safety_fault_flags(&n->safety) | n->driver_faults;
	hb->uptime_count = n->tick_ms;
}

gov_state_t gov_node_state(const struct gov_node *n)
{
	return (n == NULL) ? GOV_ST_FAULT_HALT : gov_safety_state(&n->safety);
}

uint32_t gov_node_faults(const struct gov_node *n)
{
	if (n == NULL) {
		return GOV_FAULT_INTERNAL;
	}
	/* Same merged view telemetry reports (D7): SM bits OR driver bits. */
	return gov_safety_fault_flags(&n->safety) | n->driver_faults;
}
