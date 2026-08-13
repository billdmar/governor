/*
 * control_config.h — compile-time constants for the host-portable control
 * module (PID + simulated plant). Single source of truth per config/registry.md
 * (FROZEN @ ). These MUST mirror the registry; they are consumed by pid.c and
 * the tests. Values are never widened here to pass a test (the design notes §8).
 *
 * Plant-unit convention (owned by control, referenced by registry
 * GOV_DIVERGE_LIMIT = "plant-units (see control)"): the simulated plant's
 * operating envelope is 0..100 plant-units (e.g. motor-speed % or normalized
 * temperature). The actuator authority is GOV_CTRL_OUT_MIN..GOV_CTRL_OUT_MAX.
 */
#ifndef GOV_CONTROL_CONFIG_H
#define GOV_CONTROL_CONFIG_H

/* Control-loop period — 100 Hz (registry: GOV_CTRL_PERIOD_MS = 10). */
#define GOV_CTRL_PERIOD_MS 10u

/*
 * Allowed control-loop scheduling jitter (registry §3 timing bounds): a period
 * is "on time" if it lands within ±GOV_CTRL_JITTER_MS of GOV_CTRL_PERIOD_MS.
 * Used by the  virtual-time timing measurement (tools/timing) — an EMULATION
 * logic/structure bound, never a silicon-performance claim.
 */
#define GOV_CTRL_JITTER_MS 1u

/*
 * Divergence detection (registry: GOV_DIVERGE_LIMIT "plant-units", GOV_DIVERGE_MS
 * = 200). The limit is set by control: 150 plant-units = 1.5x the 0..100
 * operating envelope. A tracking error this large cannot arise from legitimate
 * setpoint tracking within actuator authority — only from a runaway/unstable
 * plant or an unreachable commanded setpoint — so a *sustained* breach
 * (GOV_DIVERGE_MS worth of ticks) is a genuine divergence, not a transient.
 */
#define GOV_DIVERGE_LIMIT 150.0f
#define GOV_DIVERGE_MS 200u

/*
 * Safe actuator value and DEGRADED output clamp (registry: GOV_SAFE_OUTPUT = 0,
 * GOV_DEGRADED_CLAMP = 50%). The safety SM (lib/safety, owned by ) is the
 * SOLE authority that applies these in the datapath; control exposes a separate
 * reference helper (gov_ctrl_degraded_clamp, see its comment below) that
 * expresses the same "~half" intent on a different input — the two are related
 * but not identical.
 */
#define GOV_SAFE_OUTPUT 0
#define GOV_DEGRADED_CLAMP_PCT 50

/* Actuator authority for the reference plant (unidirectional: de-energized at 0). */
#define GOV_CTRL_OUT_MIN 0.0f
#define GOV_CTRL_OUT_MAX 100.0f

/*
 * Degraded-output clamp helper — a control-side REFERENCE only.
 *
 * IMPORTANT: the safety SM's gov_safety_clamp_output() is the SOLE actuation
 * authority in the datapath (SAFETY_SM invariant 1; called from app/node_core).
 * This helper is NOT wired into that path — it exists so control-side tests and
 * readers have a documented view of the DEGRADED limit. The two differ by
 * design and are NOT identical functions:
 *   - gov_safety_clamp_output() clamps to GOV_DEGRADED_CLAMP_PCT (50%) of the
 *     *commanded* value's offset from GOV_SAFE_OUTPUT (a fraction of `desired`).
 *   - gov_ctrl_degraded_clamp() clamps to a fixed 50% of the *actuator authority
 *     span* [GOV_CTRL_OUT_MIN, GOV_CTRL_OUT_MAX] measured from GOV_SAFE_OUTPUT,
 *     i.e. the absolute window [0, 50] plant-units.
 * Both express "limit output in DEGRADED to ~half," but on different inputs; the
 * safety SM's version is authoritative for what the node actually applies.
 */
static inline float gov_ctrl_degraded_clamp(float desired)
{
	const float safe = (float)GOV_SAFE_OUTPUT;
	const float span = GOV_CTRL_OUT_MAX - GOV_CTRL_OUT_MIN;
	const float half = ((float)GOV_DEGRADED_CLAMP_PCT / 100.0f) * span;
	const float lo = safe - half < GOV_CTRL_OUT_MIN ? GOV_CTRL_OUT_MIN
							 : safe - half;
	const float hi = safe + half > GOV_CTRL_OUT_MAX ? GOV_CTRL_OUT_MAX
							 : safe + half;
	if (desired < lo) {
		return lo;
	}
	if (desired > hi) {
		return hi;
	}
	return desired;
}

#endif /* GOV_CONTROL_CONFIG_H */
