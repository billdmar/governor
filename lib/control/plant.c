/*
 * plant.c — first-order simulated plant (forward-Euler). See plant.h for the
 * model and its stability envelope. Host-portable, deterministic, no allocation,
 * no <math.h> dependency. One gov_plant_step() is bounded work (RULES R7).
 */
#include "plant.h"

void gov_plant_reset(struct gov_plant *pl)
{
	pl->y = pl->cfg.y0;
	pl->disturbance = 0.0f;
}

void gov_plant_init(struct gov_plant *pl, const struct gov_plant_cfg *cfg)
{
	pl->cfg = *cfg;
	gov_plant_reset(pl);
}

float gov_plant_step(struct gov_plant *pl, float u, uint32_t dt_ms)
{
	const float dt = (float)dt_ms / 1000.0f; /* ms -> s; explicit (R5) */

	/*
	 * tau > 0 is a caller precondition (documented in plant.h). Guard anyway
	 * so a misconfigured tau cannot divide-by-zero (UBSan / R10): with a
	 * non-positive tau the plant simply holds (plus any disturbance) rather
	 * than producing undefined behavior.
	 */
	if (pl->cfg.tau_s > 0.0f) {
		const float drive = pl->cfg.gain * u - pl->y;
		pl->y += (dt / pl->cfg.tau_s) * drive;
	}
	pl->y += pl->disturbance;

	return pl->y;
}

float gov_plant_output(const struct gov_plant *pl)
{
	return pl->y;
}

void gov_plant_set_disturbance(struct gov_plant *pl, float d)
{
	pl->disturbance = d;
}
