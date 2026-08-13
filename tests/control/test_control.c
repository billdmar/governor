/*
 * test_control.c — host unit + property tests for lib/control (PID + plant).
 * Uses the shared gov_test.h harness and tests/host.mk ( infra).
 * Covers: PID output direction on hand-built vectors, closed-loop step response
 * (settling + ~zero steady-state error, no sustained oscillation), bounded
 * anti-windup on setpoint reversal, the sustained-divergence report latch/clear
 * (registry GOV_DIVERGE_LIMIT / GOV_DIVERGE_MS), and the DEGRADED clamp helper.
 */
#include "../gov_test.h"
#include "control_config.h"
#include "pid.h"
#include "plant.h"

/* Absolute value for floats — local to the test (no <math.h>). */
static float afabs(float x)
{
	return x < 0.0f ? -x : x;
}

/* A reasonable PID config for the reference plant (tau=0.1s, gain=1, 0..100). */
static struct gov_pid_cfg default_pid_cfg(void)
{
	struct gov_pid_cfg c = {
		/*
		 * Tuned for the reference plant (tau=0.1s, gain=1, 0..100) at
		 * 100 Hz: settles to the ±1 band in ~0.47 s with no overshoot
		 * and a quiet steady state. kd is small on purpose — the
		 * derivative acts on measurement with dt=10 ms, so a large kd
		 * would amplify step-to-step noise into a limit cycle.
		 */
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
	return c;
}

static struct gov_plant_cfg default_plant_cfg(void)
{
	struct gov_plant_cfg c = {
		.tau_s = 0.1f, /* 100 ms dominant pole */
		.gain = 1.0f,
		.y0 = 0.0f,
	};
	return c;
}

/*
 * Hand-built vectors: with measurement below setpoint the command should be
 * positive (drive up); above setpoint, driven toward the floor. Sign/direction
 * only — no closed loop here.
 */
static void test_pid_direction(void)
{
	struct gov_pid p;
	struct gov_pid_cfg c = default_pid_cfg();
	gov_pid_init(&p, &c);

	/* Setpoint 50, measurement 0 => large positive error => positive output. */
	float out = gov_pid_step(&p, 50.0f, 0.0f, GOV_CTRL_PERIOD_MS);
	GOV_CHECK(out > 0.0f);

	/* Now measurement above setpoint => negative error => clamped to floor. */
	gov_pid_reset(&p);
	out = gov_pid_step(&p, 10.0f, 80.0f, GOV_CTRL_PERIOD_MS);
	GOV_CHECK(out <= GOV_CTRL_OUT_MIN + 0.001f);

	/* Zero error, fresh state => output ~ 0 (no P, no D kick, integ = 0). */
	gov_pid_reset(&p);
	out = gov_pid_step(&p, 30.0f, 30.0f, GOV_CTRL_PERIOD_MS);
	GOV_CHECK(afabs(out) < 0.001f);
}

/* Output is never outside the configured saturation limits. */
static void test_pid_saturation_bounds(void)
{
	struct gov_pid p;
	struct gov_pid_cfg c = default_pid_cfg();
	gov_pid_init(&p, &c);

	for (int i = 0; i < 100; i++) {
		float out = gov_pid_step(&p, 1000.0f, 0.0f, GOV_CTRL_PERIOD_MS);
		GOV_CHECK(out <= GOV_CTRL_OUT_MAX);
		GOV_CHECK(out >= GOV_CTRL_OUT_MIN);
	}
	GOV_CHECK(gov_pid_saturated(&p)); /* commanded far beyond range */
}

/*
 * Closed-loop step response: PID + plant should drive the measurement to within
 * tolerance of the setpoint within N steps, with ~zero steady-state error
 * (integral term) and no sustained oscillation at the end.
 */
static void test_closed_loop_step(void)
{
	struct gov_pid p;
	struct gov_plant pl;
	struct gov_pid_cfg pc = default_pid_cfg();
	struct gov_plant_cfg lc = default_plant_cfg();
	gov_pid_init(&p, &pc);
	gov_plant_init(&pl, &lc);

	const float setpoint = 60.0f;
	float meas = gov_plant_output(&pl);
	const int settle_steps = 200; /* 2 s of virtual time at 100 Hz */
	int settled_at = -1;

	for (int i = 0; i < settle_steps; i++) {
		float u = gov_pid_step(&p, setpoint, meas, GOV_CTRL_PERIOD_MS);
		meas = gov_plant_step(&pl, u, GOV_CTRL_PERIOD_MS);
		if (settled_at < 0 && afabs(setpoint - meas) < 1.0f) {
			settled_at = i;
		}
	}

	GOV_CHECK(settled_at >= 0);       /* reached the band */
	GOV_CHECK(settled_at < 150);      /* within 1.5 s */
	GOV_CHECK(afabs(setpoint - meas) < 0.5f); /* ~zero steady-state error */

	/*
	 * No sustained oscillation: over the last 50 steps the peak-to-peak of
	 * the measurement stays small. Run 50 more steps and track min/max.
	 */
	float ymin = meas;
	float ymax = meas;
	for (int i = 0; i < 50; i++) {
		float u = gov_pid_step(&p, setpoint, meas, GOV_CTRL_PERIOD_MS);
		meas = gov_plant_step(&pl, u, GOV_CTRL_PERIOD_MS);
		if (meas < ymin) {
			ymin = meas;
		}
		if (meas > ymax) {
			ymax = meas;
		}
	}
	GOV_CHECK((ymax - ymin) < 1.0f); /* steady, not oscillating */
}

/*
 * Anti-windup: hold the output saturated high for a long time (unreachable
 * setpoint), then reverse the setpoint low. A wound-up integrator would keep the
 * output pinned high for many steps (huge overshoot / slow recovery). Assert the
 * output starts dropping promptly after the reversal.
 */
static void test_anti_windup(void)
{
	struct gov_pid p;
	struct gov_plant pl;
	struct gov_pid_cfg pc = default_pid_cfg();
	struct gov_plant_cfg lc = default_plant_cfg();
	gov_pid_init(&p, &pc);
	gov_plant_init(&pl, &lc);

	/*
	 * Drive toward a setpoint the plant physically cannot reach (max output
	 * is gain*out_max = 100 plant-units) => the command stays pinned at the
	 * ceiling and the integrator would wind up without the anti-windup.
	 */
	float meas = gov_plant_output(&pl);
	for (int i = 0; i < 300; i++) {
		float u = gov_pid_step(&p, 150.0f, meas, GOV_CTRL_PERIOD_MS);
		meas = gov_plant_step(&pl, u, GOV_CTRL_PERIOD_MS);
	}
	GOV_CHECK(gov_pid_saturated(&p));

	/* Reverse the setpoint low; output must come off the ceiling promptly. */
	float u_after = gov_pid_step(&p, 5.0f, meas, GOV_CTRL_PERIOD_MS);
	GOV_CHECK(u_after < GOV_CTRL_OUT_MAX); /* not still pinned high */

	/* Within a few steps the command should reach the floor (prompt recovery). */
	int reached_floor = -1;
	for (int i = 0; i < 20; i++) {
		u_after = gov_pid_step(&p, 5.0f, meas, GOV_CTRL_PERIOD_MS);
		meas = gov_plant_step(&pl, u_after, GOV_CTRL_PERIOD_MS);
		if (reached_floor < 0 && u_after <= GOV_CTRL_OUT_MIN + 0.001f) {
			reached_floor = i;
		}
	}
	GOV_CHECK(reached_floor >= 0);
	GOV_CHECK(reached_floor < 10); /* bounded windup => prompt */
}

/*
 * Divergence report: force |error| beyond GOV_DIVERGE_LIMIT and hold it. The
 * flag must latch only after >= GOV_DIVERGE_MS of accumulated ticks (not before)
 * and must clear once the error returns in-band. This is the signal the safety
 * SM consumes for T5; control does NOT latch a state itself.
 */
static void test_divergence_signal(void)
{
	struct gov_pid p;
	struct gov_pid_cfg c = default_pid_cfg();
	/* Default config: we drive the detector open-loop by holding measurement at
	 * 0 against a setpoint of 500, so |error| = 500 stays well above the 150
	 * diverge limit every tick regardless of the (unchanged) output range —
	 * this test exercises the divergence detector, not the closed loop. */
	gov_pid_init(&p, &c);

	const uint32_t period = GOV_CTRL_PERIOD_MS;
	const uint32_t ticks_to_latch = GOV_DIVERGE_MS / period; /* 200/10 = 20 */

	/* Error = 500 (> 150 limit). Just under the threshold => not latched. */
	for (uint32_t i = 0; i < ticks_to_latch - 1u; i++) {
		(void)gov_pid_step(&p, 500.0f, 0.0f, period);
		GOV_CHECK(!gov_pid_diverging(&p));
	}
	/* One more tick reaches GOV_DIVERGE_MS => latches. */
	(void)gov_pid_step(&p, 500.0f, 0.0f, period);
	GOV_CHECK(gov_pid_diverging(&p));

	/* Error returns in-band => report clears immediately. */
	(void)gov_pid_step(&p, 10.0f, 10.0f, period);
	GOV_CHECK(!gov_pid_diverging(&p));

	/* And a transient breach shorter than the window never latches. */
	gov_pid_reset(&p);
	for (uint32_t i = 0; i < ticks_to_latch - 1u; i++) {
		(void)gov_pid_step(&p, 500.0f, 0.0f, period);
	}
	(void)gov_pid_step(&p, 0.0f, 0.0f, period); /* in-band, resets accum */
	for (uint32_t i = 0; i < ticks_to_latch - 1u; i++) {
		(void)gov_pid_step(&p, 500.0f, 0.0f, period);
		GOV_CHECK(!gov_pid_diverging(&p)); /* accumulator restarted */
	}
}

/* Plant sanity: steps monotonically toward gain*u for a constant input. */
static void test_plant_monotonic(void)
{
	struct gov_plant pl;
	struct gov_plant_cfg lc = default_plant_cfg();
	gov_plant_init(&pl, &lc);

	float prev = gov_plant_output(&pl);
	const float u = 40.0f; /* target steady state = gain*u = 40 */
	for (int i = 0; i < 200; i++) {
		float y = gov_plant_step(&pl, u, GOV_CTRL_PERIOD_MS);
		GOV_CHECK(y >= prev - 0.001f); /* non-decreasing approach */
		prev = y;
	}
	GOV_CHECK(afabs(prev - 40.0f) < 0.5f); /* settles at gain*u */

	/* Injected disturbance forces the output away deterministically (F12). */
	gov_plant_reset(&pl);
	gov_plant_set_disturbance(&pl, 10.0f);
	float y0 = gov_plant_step(&pl, 0.0f, GOV_CTRL_PERIOD_MS);
	float y1 = gov_plant_step(&pl, 0.0f, GOV_CTRL_PERIOD_MS);
	GOV_CHECK(y1 > y0); /* runs away under sustained forcing */
}

/* DEGRADED clamp helper matches registry semantics: [0, 50] for this plant. */
static void test_degraded_clamp(void)
{
	GOV_CHECK(afabs(gov_ctrl_degraded_clamp(80.0f) - 50.0f) < 0.001f);
	GOV_CHECK(afabs(gov_ctrl_degraded_clamp(30.0f) - 30.0f) < 0.001f);
	GOV_CHECK(afabs(gov_ctrl_degraded_clamp(-5.0f) -
			(float)GOV_SAFE_OUTPUT) < 0.001f);
}

int main(void)
{
	GOV_RUN(test_pid_direction);
	GOV_RUN(test_pid_saturation_bounds);
	GOV_RUN(test_closed_loop_step);
	GOV_RUN(test_anti_windup);
	GOV_RUN(test_divergence_signal);
	GOV_RUN(test_plant_monotonic);
	GOV_RUN(test_degraded_clamp);
	return GOV_TEST_SUMMARY();
}
