/*
 * test_safety.c — table-driven tests for the safety state machine.
 * Covers every transition T0..T12 and the four invariants in docs/SAFETY_SM.md.
 * Host-portable: no Zephyr, injected time. Built with -Werror + ASan/UBSan.
 */
#include "../gov_test.h"
#include "../../lib/safety/safety.h"

/* Bring the SM up to RUN cleanly at t=0. */
static void bring_up_to_run(gov_safety_t *s)
{
	gov_safety_init(s);
	gov_safety_step(s, GOV_EV_POWER_ON, 0);
	gov_safety_step(s, GOV_EV_SELFTEST_OK, 10);
}

/* T0/T1: power-on → INIT → RUN on self-test OK. */
static void test_t0_t1_init_to_run(void)
{
	gov_safety_t s;
	gov_safety_init(&s);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
	GOV_CHECK(!gov_safety_actuation_allowed(&s)); /* inhibited in INIT */
	gov_safety_step(&s, GOV_EV_SELFTEST_OK, 5);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_RUN);
	GOV_CHECK(gov_safety_actuation_allowed(&s));
}

/* T2: self-test fail → SAFE_STOP, FAULT_INIT, latched. */
static void test_t2_selftest_fail(void)
{
	gov_safety_t s;
	gov_safety_init(&s);
	gov_safety_step(&s, GOV_EV_SELFTEST_FAIL, 5);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_INIT);
	GOV_CHECK(!gov_safety_actuation_allowed(&s));
}

/* T2 (timeout variant): INIT that never completes → SAFE_STOP after timeout. */
static void test_t2_init_timeout(void)
{
	gov_safety_t s;
	gov_safety_init(&s);
	gov_safety_step(&s, GOV_EV_POWER_ON, 0);
	gov_safety_step(&s, GOV_EV_TICK, GOV_INIT_TIMEOUT_MS - 1);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
	gov_safety_step(&s, GOV_EV_TICK, GOV_INIT_TIMEOUT_MS);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_INIT);
}

/* T3: recoverable sensor fault → DEGRADED, sensor flag set, clamped output. */
static void test_t3_sensor_fault(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_SENSOR_ACTIVE);
	GOV_CHECK(gov_safety_actuation_allowed(&s)); /* still actuating, clamped */
	GOV_CHECK_EQ(gov_safety_clamp_output(&s, 100), 50); /* 50% clamp */
}

/* T4: link retransmit exhausted → DEGRADED, FAULT_LINK. */
static void test_t4_link_fault(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_LINK_FAULT, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_LINK);
}

/* T5: divergence → SAFE_STOP, FAULT_DIVERGE, latched, output safe. */
static void test_t5_diverge(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_DIVERGE, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_DIVERGE);
	GOV_CHECK_EQ(gov_safety_clamp_output(&s, 100), GOV_SAFE_OUTPUT);
}

/* T6: DEGRADED → RUN after cause cleared and stable for GOV_RECOVER_MS. */
static void test_t6_recover(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	gov_safety_step(&s, GOV_EV_CAUSE_CLEARED, 30); /* start dwell */
	gov_safety_step(&s, GOV_EV_TICK, 30 + GOV_RECOVER_MS - 1);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED); /* not yet */
	gov_safety_step(&s, GOV_EV_TICK, 30 + GOV_RECOVER_MS);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_RUN);
	GOV_CHECK_EQ(gov_safety_fault_flags(&s) & GOV_FAULT_SENSOR_ACTIVE, 0);
}

/* T6 negative: if the cause re-asserts during dwell, recovery is cancelled. */
static void test_t6_recover_cancelled(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 20);
	gov_safety_step(&s, GOV_EV_CAUSE_CLEARED, 30);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 100); /* cause returns */
	gov_safety_step(&s, GOV_EV_TICK, 30 + GOV_RECOVER_MS + 5);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED); /* stayed degraded */
}

/* T7: second independent fault while degraded → SAFE_STOP, FAULT_ESCALATED. */
static void test_t7_second_fault(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 20);
	gov_safety_step(&s, GOV_EV_SECOND_FAULT, 25);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_ESCALATED);
}

/* T7 (timeout variant): degraded too long → SAFE_STOP. */
static void test_t7_degrade_max(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 20);
	gov_safety_step(&s, GOV_EV_TICK, 20 + GOV_DEGRADE_MAX_MS);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_ESCALATED);
}

/* T8: operator e-stop from RUN, DEGRADED, and INIT (bring-up) → SAFE_STOP.
 * SAFETY_SM.md T8 requires the e-stop be honored even during INIT — it must
 * never be ignored, whatever the phase (fault row F13). */
static void test_t8_operator_stop(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_OPERATOR_STOP, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_OPERATOR_STOP);

	gov_safety_t s2;
	bring_up_to_run(&s2);
	gov_safety_step(&s2, GOV_EV_SENSOR_FAULT, 20);
	gov_safety_step(&s2, GOV_EV_OPERATOR_STOP, 25);
	GOV_CHECK_EQ(gov_safety_state(&s2), GOV_ST_SAFE_STOP);

	/* From INIT (before self-test completes): e-stop still latches SAFE_STOP. */
	gov_safety_t s3;
	gov_safety_init(&s3);
	gov_safety_step(&s3, GOV_EV_POWER_ON, 0);
	GOV_CHECK_EQ(gov_safety_state(&s3), GOV_ST_INIT);
	gov_safety_step(&s3, GOV_EV_OPERATOR_STOP, 5);
	GOV_CHECK_EQ(gov_safety_state(&s3), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s3) & GOV_FAULT_OPERATOR_STOP);
	GOV_CHECK(!gov_safety_actuation_allowed(&s3));
}

/* T9: operator clear from SAFE_STOP → INIT (never directly to RUN). */
static void test_t9_operator_clear(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_DIVERGE, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	gov_safety_step(&s, GOV_EV_OPERATOR_CLEAR, 30);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT); /* re-run bring-up */
	GOV_CHECK(!gov_safety_actuation_allowed(&s));
}

/* T10: control deadline misses → DEGRADED, FAULT_TIMING. */
static void test_t10_deadline_miss(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_DEADLINE_MISS, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_TIMING);
}

/* T11: watchdog from RUN → INIT with sticky FAULT_WATCHDOG that survives. */
static void test_t11_watchdog(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_WATCHDOG, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_WATCHDOG);
	/* A plain re-init is a clean power-on: no carried-over sticky bit. */
	gov_safety_init(&s);
	GOV_CHECK_EQ(gov_safety_fault_flags(&s) & GOV_FAULT_WATCHDOG, 0);
	/* Reboot that records the watchdog cause carries the sticky bit into INIT. */
	gov_safety_init_sticky(&s, GOV_FAULT_WATCHDOG);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_WATCHDOG);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
}

/* T12: internal violation from anywhere → FAULT_HALT, latched. */
static void test_t12_internal(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_INTERNAL_VIOLATION, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_FAULT_HALT);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_INTERNAL);
	/* Terminal: no event except a full re-init leaves it. */
	gov_safety_step(&s, GOV_EV_OPERATOR_CLEAR, 30);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_FAULT_HALT);
}

/* Invariant 1: actuation ONLY in RUN/DEGRADED; safe value elsewhere. */
static void test_inv1_actuation_authority(void)
{
	gov_safety_t s;
	gov_safety_init(&s);
	GOV_CHECK_EQ(gov_safety_clamp_output(&s, 100), GOV_SAFE_OUTPUT); /* INIT */
	gov_safety_step(&s, GOV_EV_SELFTEST_OK, 5);
	GOV_CHECK_EQ(gov_safety_clamp_output(&s, 100), 100); /* RUN full */
	gov_safety_step(&s, GOV_EV_DIVERGE, 10);
	GOV_CHECK_EQ(gov_safety_clamp_output(&s, 100), GOV_SAFE_OUTPUT); /* SAFE */
}

/* Invariant 2: SAFE_STOP does not auto-exit on any timeout/tick. */
static void test_inv2_safe_stop_latches(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_DIVERGE, 20);
	for (uint32_t t = 100; t < 100000; t += 5000) {
		gov_safety_step(&s, GOV_EV_TICK, t);
		GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	}
}

/* Invariant 3+ / SAFETY_SM §3: unhandled events are counted, not silent. */
static void test_inv3_rejected_events_counted(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	uint32_t before = s.rejected_events;
	/* CAUSE_CLEARED has no meaning in RUN → must be counted, state unchanged. */
	gov_safety_step(&s, GOV_EV_CAUSE_CLEARED, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_RUN);
	GOV_CHECK(s.rejected_events > before);
}

/* Invariant: monotonic time — time going backwards is an internal violation. */
static void test_time_backwards_halts(void)
{
	gov_safety_t s;
	bring_up_to_run(&s); /* now_ms = 10 */
	gov_safety_step(&s, GOV_EV_TICK, 5); /* backwards */
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_FAULT_HALT);
}

/* No-deadlock reachability: every non-terminal state can reach a safe state and
 * every state we can enter has at least one defined exit exercised above. This
 * test walks the full nominal + fault lifecycle in one pass. */
static void test_full_lifecycle(void)
{
	gov_safety_t s;
	gov_safety_init(&s);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
	gov_safety_step(&s, GOV_EV_SELFTEST_OK, 10);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_RUN);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	gov_safety_step(&s, GOV_EV_CAUSE_CLEARED, 30);
	gov_safety_step(&s, GOV_EV_TICK, 30 + GOV_RECOVER_MS);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_RUN);
	gov_safety_step(&s, GOV_EV_OPERATOR_STOP, 2000);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	gov_safety_step(&s, GOV_EV_OPERATOR_CLEAR, 2100);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
}

/* DEGRADED: link/timing re-assertion + DIVERGE-while-degraded + rejected event. */
static void test_degraded_branches(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_LINK_FAULT, 20); /* → DEGRADED via link */
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	/* Re-assert link + timing while degraded (cancels any dwell, sets flags). */
	gov_safety_step(&s, GOV_EV_CAUSE_CLEARED, 25);
	gov_safety_step(&s, GOV_EV_LINK_FAULT, 30);
	gov_safety_step(&s, GOV_EV_DEADLINE_MISS, 35);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_LINK);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_TIMING);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	/* An unhandled event in DEGRADED is counted, not silent. */
	uint32_t before = s.rejected_events;
	gov_safety_step(&s, GOV_EV_SELFTEST_OK, 40); /* nonsensical here */
	GOV_CHECK(s.rejected_events > before);
	/* DIVERGE still applies while degraded → SAFE_STOP (T5). */
	gov_safety_step(&s, GOV_EV_DIVERGE, 45);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_SAFE_STOP);
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_DIVERGE);
}

/* DEGRADED via deadline-miss path, then sensor re-assert branch. */
static void test_degraded_sensor_reassert(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_DEADLINE_MISS, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_DEGRADED);
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 25); /* sensor re-assert branch */
	GOV_CHECK(gov_safety_fault_flags(&s) & GOV_FAULT_SENSOR_ACTIVE);
}

/* INIT rejects a nonsensical event (counted), and POWER_ON restamps timer. */
static void test_init_branches(void)
{
	gov_safety_t s;
	gov_safety_init(&s);
	uint32_t before = s.rejected_events;
	gov_safety_step(&s, GOV_EV_CAUSE_CLEARED, 5); /* meaningless in INIT */
	GOV_CHECK(s.rejected_events > before);
	gov_safety_step(&s, GOV_EV_POWER_ON, 10); /* restamp init timer */
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_INIT);
}

/* FAULT_HALT rejects non-tick events but tolerates ticks. */
static void test_fault_halt_branches(void)
{
	gov_safety_t s;
	bring_up_to_run(&s);
	gov_safety_step(&s, GOV_EV_INTERNAL_VIOLATION, 20);
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_FAULT_HALT);
	uint32_t before = s.rejected_events;
	gov_safety_step(&s, GOV_EV_SENSOR_FAULT, 25); /* rejected */
	GOV_CHECK(s.rejected_events > before);
	gov_safety_step(&s, GOV_EV_TICK, 30); /* tolerated, no reject */
	GOV_CHECK_EQ(gov_safety_state(&s), GOV_ST_FAULT_HALT);
}

/* NULL-guard defensive paths (RULES R6: no crash on bad input). */
static void test_null_guards(void)
{
	gov_safety_init(NULL); /* no crash */
	gov_safety_init_sticky(NULL, GOV_FAULT_WATCHDOG);
	GOV_CHECK_EQ(gov_safety_step(NULL, GOV_EV_TICK, 0), GOV_ST_FAULT_HALT);
	GOV_CHECK(!gov_safety_actuation_allowed(NULL));
	GOV_CHECK_EQ(gov_safety_clamp_output(NULL, 100), GOV_SAFE_OUTPUT);
	GOV_CHECK_EQ(gov_safety_fault_flags(NULL), GOV_FAULT_INTERNAL);
	GOV_CHECK_EQ(gov_safety_state(NULL), GOV_ST_FAULT_HALT);
}

/* State-name mapping (telemetry/logging helper) for every state. */
static void test_state_names(void)
{
	GOV_CHECK(gov_safety_state_name(GOV_ST_INIT)[0] == 'I');
	GOV_CHECK(gov_safety_state_name(GOV_ST_RUN)[0] == 'R');
	GOV_CHECK(gov_safety_state_name(GOV_ST_DEGRADED)[0] == 'D');
	GOV_CHECK(gov_safety_state_name(GOV_ST_SAFE_STOP)[0] == 'S');
	GOV_CHECK(gov_safety_state_name(GOV_ST_FAULT_HALT)[0] == 'F');
	GOV_CHECK(gov_safety_state_name(GOV_ST_COUNT)[0] == '?');
}

int main(void)
{
	GOV_RUN(test_t0_t1_init_to_run);
	GOV_RUN(test_t2_selftest_fail);
	GOV_RUN(test_t2_init_timeout);
	GOV_RUN(test_t3_sensor_fault);
	GOV_RUN(test_t4_link_fault);
	GOV_RUN(test_t5_diverge);
	GOV_RUN(test_t6_recover);
	GOV_RUN(test_t6_recover_cancelled);
	GOV_RUN(test_t7_second_fault);
	GOV_RUN(test_t7_degrade_max);
	GOV_RUN(test_t8_operator_stop);
	GOV_RUN(test_t9_operator_clear);
	GOV_RUN(test_t10_deadline_miss);
	GOV_RUN(test_t11_watchdog);
	GOV_RUN(test_t12_internal);
	GOV_RUN(test_inv1_actuation_authority);
	GOV_RUN(test_inv2_safe_stop_latches);
	GOV_RUN(test_inv3_rejected_events_counted);
	GOV_RUN(test_time_backwards_halts);
	GOV_RUN(test_full_lifecycle);
	GOV_RUN(test_degraded_branches);
	GOV_RUN(test_degraded_sensor_reassert);
	GOV_RUN(test_init_branches);
	GOV_RUN(test_fault_halt_branches);
	GOV_RUN(test_null_guards);
	GOV_RUN(test_state_names);
	return GOV_TEST_SUMMARY();
}
