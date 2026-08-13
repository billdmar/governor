/*
 * plant.h — host-portable simulated plant the PID drives. NO Zephyr includes,
 * no allocation, no OS calls, deterministic (the project docs). This is the "simulated
 * plant" of the project: emulation-first verification of the control logic, not
 * a silicon model.
 *
 * Model: first-order linear discrete system (one dominant pole), the canonical
 * motor-speed / thermal lag. Continuous form  tau * y' + y = k * u  advanced by
 * forward-Euler integration (no <math.h>/-lm dependency; the exact ZOH pole
 * would need exp()):
 *
 *     y[n+1] = y[n] + (dt / tau) * (k * u[n] - y[n]) + disturbance
 *
 * Stable while dt/tau < 2; the project point (dt = 10 ms, tau ~ 100 ms →
 * dt/tau = 0.1) is deep in the stable region and 100 Hz control sits a decade
 * above the dominant pole (TASKS §2). Because the model is stable, a *diverging*
 * plant in the fault matrix (F12) is produced by an external forcing term
 * (gov_plant_set_disturbance) or an unreachable setpoint, never by the model
 * itself going unstable — divergence is injected deterministically.
 */
#ifndef GOV_PLANT_H
#define GOV_PLANT_H

#include <stdint.h>

struct gov_plant_cfg {
	float tau_s;      /* time constant (s), > 0 */
	float gain;       /* DC gain k: steady-state output per unit input */
	float y0;         /* initial output (plant-units) */
};

struct gov_plant {
	struct gov_plant_cfg cfg;
	float y;           /* current output (plant-units) */
	float disturbance; /* additive forcing per step (default 0) */
};

/* Initialize from cfg; output starts at cfg.y0, disturbance cleared. cfg copied. */
void gov_plant_init(struct gov_plant *pl, const struct gov_plant_cfg *cfg);

/* Reset output to cfg.y0 and clear the disturbance. */
void gov_plant_reset(struct gov_plant *pl);

/*
 * Advance the plant one step under actuator input u for dt_ms milliseconds and
 * return the new measured output. Deterministic forward-Euler integration.
 */
float gov_plant_step(struct gov_plant *pl, float u, uint32_t dt_ms);

/* Current measured output without advancing the model. */
float gov_plant_output(const struct gov_plant *pl);

/*
 * Inject an additive per-step disturbance (plant-units added each step). Used by
 * the divergence scenario (registry F12) to force the output out of band
 * deterministically; 0 restores nominal behavior.
 */
void gov_plant_set_disturbance(struct gov_plant *pl, float d);

#endif /* GOV_PLANT_H */
