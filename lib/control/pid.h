/*
 * pid.h — host-portable PID controller for the governor control loop.
 *
 * Contract: NO Zephyr includes, no dynamic allocation, no OS calls, fully
 * deterministic (the project docs portability + memory discipline). The Zephyr control
 * task (TASKS §2, prio 4, 100 Hz) calls gov_pid_step() once per period; the time
 * step dt is passed in as integer milliseconds (the task supplies
 * GOV_CTRL_PERIOD_MS) so this module owns no clock.
 *
 * Numeric representation: single-precision float. Justification — the control
 * math (integral accumulation, derivative, saturation) is inherently
 * fractional; float keeps the reference implementation readable and the plant
 * model exact, and both Cortex-M targets in scope (qemu_cortex_m3 emulates the
 * ISA; a real STM32 with FPU) handle it. A fixed-point port is a documented
 * follow-up (DESIGN.md) if a hard-float-less MCU is ever targeted; the API here
 * is representation-agnostic. No <math.h> dependency (avoids -lm): the only
 * nonlinear op needed is |x|, done inline.
 *
 * Responsibilities kept OUT of this module by contract: this controller only
 * *reports* a sustained-divergence signal (gov_pid_diverging); it does NOT own
 * the safety state machine. lib/safety (, SAFETY_SM.md) consumes the
 * signal and decides RUN/DEGRADED/SAFE_STOP.
 */
#ifndef GOV_PID_H
#define GOV_PID_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Immutable configuration for one PID instance. out_min/out_max are the actuator
 * saturation limits; integ_min/integ_max bound the integrator (anti-windup
 * clamp, second line of defense behind conditional integration). diverge_limit
 * and diverge_ms mirror config/registry.md (GOV_DIVERGE_LIMIT / GOV_DIVERGE_MS).
 */
struct gov_pid_cfg {
	float kp;
	float ki;
	float kd;
	float out_min;
	float out_max;
	float integ_min;
	float integ_max;
	float diverge_limit;
	uint32_t diverge_ms;
};

struct gov_pid {
	struct gov_pid_cfg cfg;
	float integ;      /* accumulated integral term (state) */
	float prev_meas;  /* last measurement, for derivative-on-measurement */
	bool has_prev;    /* false until first step (suppresses derivative kick) */
	bool saturated;   /* output was clamped on the most recent step */
	uint32_t div_accum_ms; /* consecutive ms with |error| > diverge_limit */
	bool diverging;   /* sustained-divergence report for the safety SM */
};

/* Initialize an instance from cfg and clear all runtime state. cfg is copied. */
void gov_pid_init(struct gov_pid *p, const struct gov_pid_cfg *cfg);

/* Clear runtime state (integrator, derivative history, flags); keep cfg. */
void gov_pid_reset(struct gov_pid *p);

/*
 * Advance the controller one step. dt_ms is the elapsed time in milliseconds
 * (the control task passes GOV_CTRL_PERIOD_MS). Returns the actuator command,
 * saturated to [out_min, out_max]. Uses derivative-on-measurement to avoid
 * derivative kick on setpoint changes, and conditional integration + integrator
 * clamping for bounded anti-windup.
 */
float gov_pid_step(struct gov_pid *p, float setpoint, float measurement,
		   uint32_t dt_ms);

/* True if the most recent gov_pid_step() output hit a saturation limit. */
bool gov_pid_saturated(const struct gov_pid *p);

/*
 * True once |error| has stayed above diverge_limit for >= diverge_ms of injected
 * ticks; clears when the error returns in-band. This is a *report* only — the
 * safety SM owns the latched SAFE_STOP (SAFETY_SM T5).
 */
bool gov_pid_diverging(const struct gov_pid *p);

#endif /* GOV_PID_H */
