/*
 * fake_bus.h — scriptable fake HAL bus for host tests.
 *
 * Implements the abstract `struct gov_bus` (drivers/hal.h) against an
 * in-memory script of responses, with no hardware and no Zephyr. This is what
 * makes the driver fault-matrix rows F01-F05 (config/registry.md)
 * deterministically injectable on the host:
 *
 *   NAK        (F04): a response with status GOV_BUS_NAK
 *   bus error  (F05): a response with status GOV_BUS_ERR (SDA stuck low)
 *   dropout    (F01): a response with status GOV_BUS_NODATA
 *   stuck      (F02): repeated OK responses carrying an identical value
 *   garbage    (F03): an OK response carrying an out-of-range value
 *   nominal         : an OK response carrying an in-range value
 *
 * Model: a linear script of {status, value} entries consumed one per read()
 * call. When the script is exhausted the LAST entry sticks (so "always NAK" is
 * one NAK entry; "NAK then recover" is a NAK entry followed by an OK entry).
 * `read_calls` counts every read() so a test can assert the F04 retry policy
 * actually engaged. On an OK response the `value` is encoded big-endian into
 * the caller's buffer (matching sensor.c's decode).
 */
#ifndef GOV_FAKE_BUS_H
#define GOV_FAKE_BUS_H

#include <stddef.h>
#include <stdint.h>

#include "hal.h"

/* Max scripted responses. Ample for the deterministic test scenarios. */
#define GOV_FAKE_MAX_STEPS 64

struct gov_fake_step {
	gov_bus_status_t status;
	int32_t value; /* used only when status == GOV_BUS_OK */
};

struct gov_fake_bus {
	struct gov_bus bus; /* HAL view; .ctx points back at this object */
	struct gov_fake_step steps[GOV_FAKE_MAX_STEPS];
	size_t n;   /* number of scripted steps */
	size_t idx; /* next step to consume (sticks at n-1 when exhausted) */
	unsigned read_calls;  /* total read() invocations (retry assertions) */
	unsigned write_calls; /* total write() invocations */
};

/* Initialise the fake and wire up the HAL vtable. After this, pass
 * &fb->bus as the sensor's `struct gov_bus *`. */
void gov_fake_bus_init(struct gov_fake_bus *fb);

/* Append one scripted response. Ignored (saturates) past GOV_FAKE_MAX_STEPS. */
void gov_fake_push(struct gov_fake_bus *fb, gov_bus_status_t status, int32_t value);

/* Convenience: append `count` identical OK responses carrying `value`
 * (e.g. to drive the F02 stuck window). */
void gov_fake_push_repeat(struct gov_fake_bus *fb, int32_t value, size_t count);

/* Replace the whole script with a single sticky response (the common
 * "the bus is now in state X forever" case). */
void gov_fake_set(struct gov_fake_bus *fb, gov_bus_status_t status, int32_t value);

#endif /* GOV_FAKE_BUS_H */
