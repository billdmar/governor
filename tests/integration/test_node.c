/*
 * test_node.c — end-to-end integration test of the node coordinator.
 * Exercises the full portable stack (PID + plant + safety + telemetry) as one
 * loop, then injects faults and asserts the required safety-state outcomes.
 * This is the host-side mirror of the native_sim end-to-end demo (the qemu
 * build proves the same code compiles + boots on the target).
 */
#include "../gov_test.h"
#include "../../app/node_core.h"

#include <math.h>

/* Run the closed loop for `steps` 10 ms ticks starting at t0; returns final
 * measurement. Feeds the plant output back as the sensor measurement. */
static float run_loop(struct gov_node *n, int steps, uint32_t t0)
{
	uint32_t t = t0;
	float meas = gov_plant_output(&n->plant);
	for (int i = 0; i < steps; i++) {
		gov_node_control_step(n, meas, t);
		meas = gov_plant_output(&n->plant);
		t += 10u;
	}
	return meas;
}

/* End-to-end nominal: node boots, reaches RUN, PID drives plant to setpoint. */
static void test_e2e_reaches_setpoint(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);

	float meas = run_loop(&n, 200, 10); /* 2 s of virtual time */
	GOV_CHECK(fabsf(meas - 50.0f) < 1.0f); /* within ±1 of setpoint */
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);
	GOV_CHECK_EQ(gov_node_faults(&n), GOV_FAULT_NONE);
}

/* End-to-end telemetry: the record reflects real loop state. */
static void test_e2e_telemetry(void)
{
	struct gov_node n;
	gov_node_init(&n, 40.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 200, 10);

	struct gov_telem_record rec;
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK_EQ(rec.setpoint, 40);
	GOV_CHECK(rec.measurement > 38 && rec.measurement < 42);
	GOV_CHECK_EQ(rec.state, (int)GOV_ST_RUN);
	GOV_CHECK_EQ(rec.fault_flags, GOV_FAULT_NONE);

	/* And it survives the frozen wire encoding. */
	uint8_t buf[GOV_TELEM_DATA_LEN];
	size_t nbytes = gov_telem_encode(&rec, buf, sizeof buf);
	GOV_CHECK(nbytes > 0 && nbytes <= sizeof buf);
	struct gov_telem_record back;
	GOV_CHECK(gov_telem_decode(buf, nbytes, &back));
	GOV_CHECK_EQ(back.setpoint, rec.setpoint);
	GOV_CHECK_EQ(back.fault_flags, rec.fault_flags);
	GOV_CHECK_EQ(back.state, rec.state);
}

/* Fault injection: a sensor fault drives RUN→DEGRADED (output clamped), and the
 * fault is flagged in telemetry — the required outcome for F01-F03. */
static void test_e2e_sensor_fault_degrades(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);

	gov_node_post_event(&n, GOV_EV_SENSOR_FAULT, 1010);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);

	struct gov_heartbeat hb;
	gov_node_fill_heartbeat(&n, &hb);
	GOV_CHECK(hb.fault_flags & GOV_FAULT_SENSOR_ACTIVE); /* fault flagged */
	GOV_CHECK_EQ(hb.state, (int)GOV_ST_DEGRADED);
}

/* Fault injection: sustained divergence drives RUN→SAFE_STOP, latched, output
 * forced safe — the required outcome for F12. Injected via a plant disturbance. */
static void test_e2e_divergence_safe_stops(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);

	/* Force the plant far out of band so |error| exceeds the diverge limit
	 * for longer than GOV_DIVERGE_MS. */
	gov_plant_set_disturbance(&n.plant, 500.0f);
	run_loop(&n, 100, 1010);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_DIVERGE);

	/* Latched: further ticks do not leave SAFE_STOP. */
	run_loop(&n, 50, 3000);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
}

/* Fault injection: operator e-stop from RUN → SAFE_STOP (F13). */
static void test_e2e_operator_estop(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 50, 10);
	gov_node_post_event(&n, GOV_EV_OPERATOR_STOP, 600);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_OPERATOR_STOP);
}

/* F01/F02/F04 recovery leg: a recoverable sensor fault degrades the node, the
 * driver's SPECIFIC cause bit reaches telemetry (D7), and once the source reads
 * healthy again the node auto-recovers DEGRADED→RUN after GOV_RECOVER_MS (T6) —
 * WITHOUT escalating to SAFE_STOP. This is the integrated recovery the fault
 * matrix requires; before the coordinator wired GOV_EV_CAUSE_CLEARED it never
 * happened (the node escalated at GOV_DEGRADE_MAX_MS instead). */
static void test_e2e_sensor_fault_recovers(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);

	/* Dropout begins → DEGRADED, and the SPECIFIC bit is in telemetry (D7). */
	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_DROP, false, 1010);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);
	struct gov_telem_record rec;
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(rec.fault_flags & GOV_FAULT_SENSOR_ACTIVE); /* coarse SM bit */
	GOV_CHECK(rec.fault_flags & GOV_FAULT_SENSOR_DROP);   /* specific driver bit */

	/* Data resumes: healthy read clears the cause and starts the T6 dwell. */
	gov_node_note_sensor(&n, 0u, true, 1100);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED); /* dwell not yet elapsed */

	/* After GOV_RECOVER_MS of stable-clear ticks → RUN, driver bit cleared. */
	run_loop(&n, 80, 1110); /* 800 ms > GOV_RECOVER_MS (500) */
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(!(rec.fault_flags & GOV_FAULT_SENSOR_DROP));
	GOV_CHECK(!(rec.fault_flags & GOV_FAULT_SENSOR_ACTIVE));
}

/* Second-independent-fault escalation (SAFETY_SM T7): once DEGRADED by one
 * recoverable cause, a DIFFERENT recoverable cause is a second independent fault
 * and must escalate to latched SAFE_STOP (FAULT_ESCALATED) — the node does not
 * keep clamping through a compounding failure. The coordinator maps the second
 * distinct cause to GOV_EV_SECOND_FAULT. */
static void test_e2e_second_independent_fault_escalates(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);

	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_STUCK, false, 1010);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);

	/* A second, independent cause (link) while already degraded → SAFE_STOP. */
	gov_node_note_link(&n, false, 1020);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_ESCALATED);
}

/* Recovery is gated on the SAME cause staying clear for the dwell: a single
 * recoverable cause that flaps (clears then re-asserts before GOV_RECOVER_MS)
 * must NOT recover — the re-assert cancels the dwell. */
static void test_e2e_flapping_cause_does_not_recover(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);

	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_DROP, false, 1010);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);

	gov_node_note_sensor(&n, 0u, true, 1100);      /* clears → dwell starts */
	run_loop(&n, 20, 1110);                         /* 200 ms < GOV_RECOVER_MS */
	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_DROP, false, 1310); /* flaps back */
	run_loop(&n, 40, 1320);                         /* dwell was cancelled */
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);
}

/* Shadow-state resync across a full recovery cycle: after a T7 escalation to
 * SAFE_STOP and an operator clear back through INIT→RUN, the coordinator's cause
 * shadow must be reset — otherwise the FIRST fault afterward would look like a
 * "second independent fault", post GOV_EV_SECOND_FAULT, hit no RUN transition,
 * and be SILENTLY REJECTED (an unflagged fault — a safety-invariant violation),
 * and a stale driver bit would linger in telemetry. Regression test for that. */
static void test_e2e_shadow_resync_after_operator_clear(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);

	/* Two independent faults → T7 escalation → SAFE_STOP. The coordinator's
	 * cause shadow now holds {SENSOR, LINK}. */
	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_DROP, false, 1010);
	gov_node_note_link(&n, false, 1020);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);

	/* Operator clears directly (the real path: a CMD_CLEAR while the shadow
	 * still holds the escalation's causes — the coordinator never saw a healthy
	 * read). Without a resync this leaves active_causes non-empty. */
	gov_node_post_event(&n, GOV_EV_OPERATOR_CLEAR, 1040);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_INIT);
	/* Stale specific-cause bit must NOT linger in telemetry after the clear. */
	struct gov_telem_record rec;
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(!(rec.fault_flags & GOV_FAULT_SENSOR_DROP));

	gov_node_selftest_ok(&n, 1050);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_RUN);
	run_loop(&n, 20, 1060);

	/* A single, genuine fault after recovery must DEGRADE (not be swallowed as a
	 * bogus second fault, not re-escalate). */
	gov_node_note_timing(&n, false, 1300);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_TIMING); /* flagged, never silent */
}

/* F05 escalation leg as an integrated outcome: a persistent bus error degrades
 * the node, and if it stays past GOV_DEGRADE_MAX_MS the node escalates to a
 * latched SAFE_STOP (T7 degrade-max timer). The driver test proves the BUS_ERR
 * flag in isolation; this proves the required end-to-end escalation + that the
 * specific BUS_ERR bit is in telemetry throughout (D7). */
static void test_e2e_bus_error_persists_escalates(void)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	run_loop(&n, 100, 10);

	gov_node_note_sensor(&n, GOV_FAULT_BUS_ERR, false, 1010);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);
	struct gov_telem_record rec;
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(rec.fault_flags & GOV_FAULT_BUS_ERR); /* specific cause in telem (D7) */

	/* Keep re-asserting the bus error while advancing time past degrade-max. */
	uint32_t t = 1020;
	for (int i = 0; i < 12; i++) {
		gov_node_note_sensor(&n, GOV_FAULT_BUS_ERR, false, t);
		t += (GOV_DEGRADE_MAX_MS / 8u);
	}
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_ESCALATED);
	/* The specific root-cause bit MUST persist in the latched SAFE_STOP so an
	 * operator can see why the node stopped (registry F05 names FAULT_BUS_ERR;
	 * D7 dual-report). It is only cleared once the node re-runs bring-up. */
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(rec.fault_flags & GOV_FAULT_BUS_ERR);

	/* Even if the bus physically recovers while still latched, a healthy read
	 * must NOT wipe the cause bit — the clear is gated on actuation (RUN/
	 * DEGRADED), and SAFE_STOP is latched. The operator still sees BUS_ERR. */
	gov_node_note_sensor(&n, 0u, true, t + 100u);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(rec.fault_flags & GOV_FAULT_BUS_ERR);

	/* Only after an operator clear (→ INIT, re-run bring-up) is it dropped. */
	gov_node_post_event(&n, GOV_EV_OPERATOR_CLEAR, t + 200u);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_INIT);
	gov_node_fill_telemetry(&n, &rec);
	GOV_CHECK(!(rec.fault_flags & GOV_FAULT_BUS_ERR));
}

/* Exhaustive shadow↔SM coherence invariant: from EVERY SM state, injecting a
 * single recoverable fault via the note_* API must never be silently swallowed —
 * the node must either flag it (RUN/DEGRADED → the fault bit is set) or be in a
 * state where actuation is inhibited and the fault is moot (INIT re-runs, latched
 * states stay latched). This is the property the three shadow bugs all violated;
 * it drives each of the 5 states and asserts no unflagged-fault escape exists. */
static void reach_run(struct gov_node *n)
{
	gov_node_init(n, 50.0f);
	gov_node_selftest_ok(n, 0);
	run_loop(n, 60, 10);
}
static void test_e2e_shadow_coherence_all_states(void)
{
	/* From RUN: a lone timing fault must DEGRADE + flag (never swallowed). */
	struct gov_node n;
	reach_run(&n);
	gov_node_note_timing(&n, false, 1000);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_TIMING);

	/* From DEGRADED: the SAME cause clearing then a fresh independent one still
	 * routes correctly (second independent → escalate, flagged). */
	gov_node_note_link(&n, false, 1010);           /* 2nd independent → SAFE_STOP */
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_ESCALATED);

	/* From latched SAFE_STOP: any note_* must NOT leave SAFE_STOP (latched). */
	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_DROP, false, 1020);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);
	gov_node_note_timing(&n, false, 1030);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_SAFE_STOP);

	/* Operator clear → INIT: shadow reset; a fresh single fault after bring-up
	 * degrades + flags (the original desync bug's exact failure point). */
	gov_node_post_event(&n, GOV_EV_OPERATOR_CLEAR, 1040);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_INIT);
	gov_node_selftest_ok(&n, 1050);
	run_loop(&n, 20, 1060);
	gov_node_note_sensor(&n, GOV_FAULT_SENSOR_STUCK, false, 1300);
	GOV_CHECK_EQ(gov_node_state(&n), GOV_ST_DEGRADED);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_SENSOR_ACTIVE);
	GOV_CHECK(gov_node_faults(&n) & GOV_FAULT_SENSOR_STUCK); /* specific bit (D7) */

	/* From FAULT_HALT (terminal): note_* cannot leave it. */
	struct gov_node h;
	reach_run(&h);
	gov_node_post_event(&h, GOV_EV_INTERNAL_VIOLATION, 2000); /* T12 → FAULT_HALT */
	GOV_CHECK_EQ(gov_node_state(&h), GOV_ST_FAULT_HALT);
	gov_node_note_sensor(&h, GOV_FAULT_SENSOR_DROP, false, 2010);
	GOV_CHECK_EQ(gov_node_state(&h), GOV_ST_FAULT_HALT); /* terminal, no escape */
}

int main(void)
{
	GOV_RUN(test_e2e_reaches_setpoint);
	GOV_RUN(test_e2e_telemetry);
	GOV_RUN(test_e2e_sensor_fault_degrades);
	GOV_RUN(test_e2e_divergence_safe_stops);
	GOV_RUN(test_e2e_operator_estop);
	GOV_RUN(test_e2e_sensor_fault_recovers);
	GOV_RUN(test_e2e_second_independent_fault_escalates);
	GOV_RUN(test_e2e_flapping_cause_does_not_recover);
	GOV_RUN(test_e2e_shadow_resync_after_operator_clear);
	GOV_RUN(test_e2e_bus_error_persists_escalates);
	GOV_RUN(test_e2e_shadow_coherence_all_states);
	return GOV_TEST_SUMMARY();
}
