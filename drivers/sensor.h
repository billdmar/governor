/*
 * sensor.h — host-portable sensor driver logic (validation + fault detection).
 *
 * This is the pure logic/validation layer of a sensor driver (e.g. an I2C
 * temperature/pressure sensor). It has NO Zephyr dependency: it reads through
 * the abstract `struct gov_bus` HAL (drivers/hal.h) and turns raw transport
 * results into *sensor health*. Detection covered here maps 1:1 to the driver
 * rows of the fault matrix (config/registry.md):
 *
 *   F01 dropout : no data-ready for N periods           -> GOV_SFLAG_DROPOUT
 *   F02 stuck   : identical in-range value for a window  -> GOV_SFLAG_STUCK
 *   F03 garbage : out-of-range sample                    -> GOV_SFLAG_RANGE
 *   F04 I2C NAK : device NAK, retried then flagged       -> GOV_SFLAG_BUS_NAK
 *   F05 bus err : SDA stuck / bus fault                  -> GOV_SFLAG_BUS_ERR
 *
 * The driver only *reports* health (SAFETY_SM.md §1): it never decides state.
 * The health task maps these driver flags onto the canonical telemetry
 * FAULT_* codes and posts GOV_EV_SENSOR_FAULT to the safety SM; escalation
 * (e.g. F05 -> SAFE_STOP after GOV_DEGRADE_MAX_MS) is the safety SM's job.
 *
 * No dynamic allocation (RULES.md R1); no recursion (R7); fixed-width types
 * (R3); all buffers length-checked (R8).
 */
#ifndef GOV_SENSOR_H
#define GOV_SENSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"

/* Max register bytes a single read may return (bounds the static read buffer,
 * RULES.md R8). 4 covers a 32-bit sample; larger sensors would raise this. */
#define GOV_SENSOR_MAX_READ 4u

/*
 * Driver-local health flags (bitmask). Namespaced GOV_SFLAG_* to stay distinct
 * from the safety/telemetry layer's canonical FAULT_* enum, which the health
 * task derives from these (see mapping in the file header). Multiple flags may
 * be set at once; GOV_SFLAG_NONE (0) == healthy.
 */
enum {
	GOV_SFLAG_NONE = 0u,
	GOV_SFLAG_DROPOUT = 1u << 0, /* F01: sustained no-data-ready */
	GOV_SFLAG_STUCK = 1u << 1,   /* F02: value unchanging past the window */
	GOV_SFLAG_RANGE = 1u << 2,   /* F03: sample out of [range_min,range_max] */
	GOV_SFLAG_BUS_NAK = 1u << 3, /* F04: device NAK persisted past retries */
	GOV_SFLAG_BUS_ERR = 1u << 4, /* F05: bus-level fault (not retryable) */
};

typedef enum {
	GOV_SENSOR_HEALTHY = 0, /* no flags set */
	GOV_SENSOR_FAULTED,     /* one or more GOV_SFLAG_* set */
} gov_sensor_health_t;

/*
 * Static configuration. Bounds live here (not hard-coded) so tests can drive
 * short windows deterministically; targets set them from *_config.h. All are
 * consumed as-is — never widened to pass a test (registry.md, RULES.md).
 */
struct gov_sensor_cfg {
	struct gov_bus *bus; /* HAL bus (target: Zephyr I2C; test: fake_bus) */
	uint8_t addr;        /* device bus address */
	uint8_t reg;         /* sample register */
	size_t read_len;     /* bytes to read, 1..GOV_SENSOR_MAX_READ */
	int32_t range_min;   /* inclusive valid range for a decoded sample */
	int32_t range_max;
	uint16_t dropout_limit; /* consecutive NODATA polls before DROPOUT (F01, "N periods") */
	uint16_t stuck_limit;   /* consecutive identical in-range samples before STUCK (F02 window) */
	uint8_t nak_retries;    /* extra read attempts on NAK before BUS_NAK (F04) */
};

/*
 * Driver state. Single-writer (the poll caller's context). No allocation; the
 * whole object is caller-owned storage.
 */
struct gov_sensor {
	struct gov_sensor_cfg cfg;
	uint32_t flags;      /* current GOV_SFLAG_* bitmask */
	int32_t last_good;   /* most recent accepted sample (held on fault) */
	bool has_last_good;  /* false until the first accepted sample */
	uint16_t nodata_cnt; /* consecutive NODATA polls */
	uint16_t stuck_cnt;  /* consecutive identical in-range samples */
};

/*
 * ISR-safe latest-value handoff slot (TASKS.md §2/§4): a 1-deep "newest wins"
 * mailbox. On target the sensor data-ready ISR (or the poll) pushes here and
 * the control thread reads it;  guards the pair with a k_spinlock. Single
 * writer / single reader, no queue, no allocation. Host tests exercise it bare.
 */
struct gov_sample_slot {
	int32_t value;
	bool fresh; /* true if pushed since the last get (a staleness/dropout cue) */
};

/* --- lifecycle --- */
void gov_sensor_init(struct gov_sensor *s, const struct gov_sensor_cfg *cfg);

/*
 * Poll once (call per sampling period). Reads via the HAL with NAK retry,
 * validates (range/stuck/dropout), updates health flags and last_good, and —
 * when a value is available — publishes last_good into `slot` if non-NULL.
 * Returns the aggregate health. Bounded work, never blocks (RULES.md R5-invariant).
 */
gov_sensor_health_t gov_sensor_poll(struct gov_sensor *s, struct gov_sample_slot *slot);

/* --- accessors (const-correct, RULES.md R9) --- */
uint32_t gov_sensor_faults(const struct gov_sensor *s);          /* GOV_SFLAG_* bitmask */
gov_sensor_health_t gov_sensor_health(const struct gov_sensor *s);
int32_t gov_sensor_value(const struct gov_sensor *s);            /* last accepted sample */
bool gov_sensor_has_value(const struct gov_sensor *s);

/* --- latest-value slot --- */
void gov_slot_init(struct gov_sample_slot *slot);
void gov_slot_push(struct gov_sample_slot *slot, int32_t value);
/* Returns true and writes *out if a fresh value is present; clears freshness. */
bool gov_slot_get(struct gov_sample_slot *slot, int32_t *out);

#endif /* GOV_SENSOR_H */
