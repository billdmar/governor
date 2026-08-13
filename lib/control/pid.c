/*
 * pid.c — host-portable PID controller. See pid.h for the contract (no Zephyr,
 * no allocation, no OS calls, deterministic). float representation, justified in
 * pid.h. Single-writer state; one gov_pid_step() is bounded work (no loops, no
 * recursion — RULES R7).
 */
#include "pid.h"

/* Inline abs for float — avoids an <math.h>/-lm dependency (pid.h rationale). */
static float gov_fabsf(float x)
{
	return x < 0.0f ? -x : x;
}

/* Clamp x to [lo, hi]. lo <= hi is a caller precondition (cfg validated at init). */
static float gov_clampf(float x, float lo, float hi)
{
	if (x < lo) {
		return lo;
	}
	if (x > hi) {
		return hi;
	}
	return x;
}

void gov_pid_reset(struct gov_pid *p)
{
	p->integ = 0.0f;
	p->prev_meas = 0.0f;
	p->has_prev = false;
	p->saturated = false;
	p->div_accum_ms = 0u;
	p->diverging = false;
}

void gov_pid_init(struct gov_pid *p, const struct gov_pid_cfg *cfg)
{
	p->cfg = *cfg;
	gov_pid_reset(p);
}

float gov_pid_step(struct gov_pid *p, float setpoint, float measurement,
		   uint32_t dt_ms)
{
	const float dt = (float)dt_ms / 1000.0f; /* ms -> s; explicit (R5) */
	const float error = setpoint - measurement;

	/* Proportional. */
	const float p_term = p->cfg.kp * error;

	/*
	 * Derivative on measurement (not on error): d/dt of the setpoint is
	 * ignored so a setpoint step does not produce a derivative kick. Sign
	 * negated because we differentiate measurement, not error. First step
	 * has no history, so the derivative contribution is zero.
	 */
	float d_term = 0.0f;
	if (p->has_prev && dt > 0.0f) {
		const float d_meas = (measurement - p->prev_meas) / dt;
		d_term = -p->cfg.kd * d_meas;
	}

	/*
	 * Integral with conditional integration (anti-windup): tentatively
	 * accumulate, form the unsaturated output, and if that output is beyond
	 * a limit AND the integral is pushing further into that limit, roll the
	 * integrator back. A hard clamp to [integ_min, integ_max] bounds any
	 * residual accumulation. Together these keep windup bounded so a
	 * setpoint reversal after long saturation recovers promptly.
	 */
	const float integ_candidate =
		gov_clampf(p->integ + p->cfg.ki * error * dt,
			   p->cfg.integ_min, p->cfg.integ_max);
	float out_unsat = p_term + integ_candidate + d_term;

	bool commit_integ = true;
	if (out_unsat > p->cfg.out_max && error > 0.0f) {
		commit_integ = false; /* saturated high, integral pushing higher */
	} else if (out_unsat < p->cfg.out_min && error < 0.0f) {
		commit_integ = false; /* saturated low, integral pushing lower */
	}

	if (commit_integ) {
		p->integ = integ_candidate;
	} else {
		out_unsat = p_term + p->integ + d_term; /* recompute w/ held integ */
	}

	const float out = gov_clampf(out_unsat, p->cfg.out_min, p->cfg.out_max);
	p->saturated = (out != out_unsat);

	/*
	 * Sustained-divergence report (SAFETY_SM T5 input). Accumulate ms while
	 * |error| exceeds the limit; latch the report once the accumulation
	 * reaches diverge_ms. Reset the moment the error returns in-band — the
	 * safety SM, not this module, owns any latched SAFE_STOP.
	 */
	if (gov_fabsf(error) > p->cfg.diverge_limit) {
		p->div_accum_ms += dt_ms;
		if (p->div_accum_ms >= p->cfg.diverge_ms) {
			p->diverging = true;
		}
	} else {
		p->div_accum_ms = 0u;
		p->diverging = false;
	}

	p->prev_meas = measurement;
	p->has_prev = true;

	return out;
}

bool gov_pid_saturated(const struct gov_pid *p)
{
	return p->saturated;
}

bool gov_pid_diverging(const struct gov_pid *p)
{
	return p->diverging;
}
