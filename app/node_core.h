/*
 * node_core.h — governor node coordinator (host-portable integration glue).
 *
 * . Ties the portable modules into one deterministic step function:
 *   sensor sample → PID → safety clamp → plant → telemetry/heartbeat → safety.
 * It owns NO new policy — it wires the frozen contracts together:
 *   - PID computes desired actuator output (lib/control)
 *   - the safety SM decides whether/how much of it is applied (lib/safety)
 *   - divergence / link / sensor / timing faults are posted to the SM as events
 *   - telemetry records + heartbeats are produced from the resulting state
 * Time is injected (now_ms) so the whole node loop is unit-testable on the host
 * with no Zephyr. The Zephyr app (app/main.c) is a thin driver over this.
 */
#ifndef GOV_NODE_CORE_H
#define GOV_NODE_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "safety.h"
#include "pid.h"
#include "plant.h"
#include "telem.h"

struct gov_node {
	gov_safety_t   safety;
	struct gov_pid pid;
	struct gov_plant plant;
	float          setpoint;
	uint32_t       tick_ms;    /* monotonic virtual/real time */
	uint32_t       seq;        /* telemetry DATA sequence */
	float          last_output;
	float          last_measurement;
	/* Driver-owned specific fault bits (GOV_FAULT_SENSOR_DROP/STUCK/RANGE/
	 * BUS_*), OR-merged into the telemetry fault word per DESIGN D7. The SM
	 * owns the coarse GOV_FAULT_SENSOR_ACTIVE bit; the driver layer reports
	 * the specific cause here without either layer guessing. */
	uint32_t       driver_faults;
	/* Which recoverable causes are currently active (sensor/link/timing),
	 * tracked so GOV_EV_CAUSE_CLEARED (T6) is posted only when the LAST one
	 * clears — see gov_node_note_* below. */
	uint32_t       active_causes;
};

/* Recoverable fault sources the coordinator tracks for T6 recovery. */
enum gov_cause {
	GOV_CAUSE_SENSOR = (1u << 0),
	GOV_CAUSE_LINK   = (1u << 1),
	GOV_CAUSE_TIMING = (1u << 2),
};

/* Bring the node up: safety=INIT, PID/plant reset. setpoint is the control
 * target in plant-units. */
void gov_node_init(struct gov_node *n, float setpoint);

/* Signal self-test complete → safety attempts INIT→RUN (T1). */
void gov_node_selftest_ok(struct gov_node *n, uint32_t now_ms);

/* One control step at now_ms: reads the (already-validated) measurement,
 * computes PID, applies the safety clamp, advances the plant, and posts a
 * divergence event if the controller reports sustained divergence. Returns the
 * actuator output actually applied (post-clamp). */
float gov_node_control_step(struct gov_node *n, float measurement, uint32_t now_ms);

/* Post an external fault event to the safety SM (sensor/link/timing/operator).
 * Thin pass-through so the app's driver/link/health layers drive the SM. */
void gov_node_post_event(struct gov_node *n, gov_event_t ev, uint32_t now_ms);

/* Edge-triggered fault-source reporting (registry F01-F05/F07/F11 recovery +
 * DESIGN D7 telemetry merge). Each note_* is called with the CURRENT health of
 * one subsystem; the coordinator posts the RUN→DEGRADED fault event on the
 * rising edge and — only when the LAST recoverable cause clears — posts
 * GOV_EV_CAUSE_CLEARED so the SM's T6 dwell can restore RUN. sflags is the
 * driver's GOV_SFLAG_* / GOV_FAULT_SENSOR_* specific bitmask, merged into the
 * telemetry word (the SM only knows the coarse SENSOR_ACTIVE bit).
 *   healthy == true  → the source reads OK this cycle
 *   healthy == false → the source is faulted (sflags names the specific cause) */
void gov_node_note_sensor(struct gov_node *n, uint32_t sflags, bool healthy,
			  uint32_t now_ms);
void gov_node_note_link(struct gov_node *n, bool healthy, uint32_t now_ms);
void gov_node_note_timing(struct gov_node *n, bool healthy, uint32_t now_ms);

/* Fill a telemetry DATA record from the current node state. */
void gov_node_fill_telemetry(const struct gov_node *n, struct gov_telem_record *rec);

/* Fill a HEARTBEAT (state byte + fault flags + uptime tick). */
void gov_node_fill_heartbeat(const struct gov_node *n, struct gov_heartbeat *hb);

/* Convenience accessors. */
gov_state_t gov_node_state(const struct gov_node *n);
uint32_t    gov_node_faults(const struct gov_node *n);

#endif /* GOV_NODE_CORE_H */
