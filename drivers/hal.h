/*
 * hal.h — abstract sensor-bus HAL for the governor node.
 *
 * A hardware-agnostic bus interface (function-pointer "vtable") so the sensor
 * driver's logic/validation layer has NO Zephyr dependency and is unit-tested
 * on the host. On target this binds to Zephyr's I2C API; in tests it binds to
 * the scriptable fake in tests/drivers/fake_bus.c. This separation is what
 * makes the fault matrix rows F04 (I2C NAK) and F05 (I2C bus error) injectable
 * purely in C on the host (config/registry.md).
 *
 * Contract notes (drivers do NOT own state — SAFETY_SM.md §1):
 *   - The bus reports transport-level status only (OK / NAK / bus-error /
 *     no-data). The sensor driver turns that into sensor *health*; the health
 *     and safety layers decide state transitions.
 */
#ifndef GOV_HAL_H
#define GOV_HAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Transport-level status returned by every bus op. Ordered so GOV_BUS_OK == 0
 * (the "success" convention). Each non-OK code maps to a fault-matrix row:
 *   GOV_BUS_NAK    → F04 (device did not ACK; retryable transient)
 *   GOV_BUS_ERR    → F05 (bus-level error, e.g. SDA stuck low; NOT retryable)
 *   GOV_BUS_NODATA → F01 input (data-ready not asserted; dropout when sustained)
 */
typedef enum {
	GOV_BUS_OK = 0,
	GOV_BUS_NAK,    /* addressed device returned no acknowledge */
	GOV_BUS_ERR,    /* bus fault: SDA/SCL stuck, arbitration lost, timeout */
	GOV_BUS_NODATA, /* conversion / data-ready not yet available */
} gov_bus_status_t;

/*
 * Abstract bus. `ctx` is the implementation's opaque handle (a Zephyr
 * `const struct device *` on target, a `struct gov_fake_bus *` in tests).
 * Every op is a plain register-style transfer and returns a gov_bus_status_t;
 * no op blocks indefinitely or allocates (RULES.md R1).
 */
struct gov_bus {
	/* Read `len` bytes from `reg` of device `addr` into `buf`. */
	gov_bus_status_t (*read)(void *ctx, uint8_t addr, uint8_t reg,
				 uint8_t *buf, size_t len);
	/* Write `len` bytes from `buf` to `reg` of device `addr`. */
	gov_bus_status_t (*write)(void *ctx, uint8_t addr, uint8_t reg,
				  const uint8_t *buf, size_t len);
	/* Probe for the presence of device `addr` (OK / NAK / ERR). */
	gov_bus_status_t (*probe)(void *ctx, uint8_t addr);
	void *ctx;
};

/* ---------------------------------------------------------------------------
 * TARGET BINDING ( integration — sketch only, no Zephyr here).
 *
 * On target a thin adapter (e.g. drivers/hal_zephyr.c, ) fills a
 * `struct gov_bus` whose `ctx` is the I2C `const struct device *` and whose
 * fn-pointers wrap `i2c_burst_read` / `i2c_burst_write` / `i2c_reg_read_byte`,
 * translating Zephyr return codes to gov_bus_status_t:
 *     0        -> GOV_BUS_OK
 *     -EIO     -> GOV_BUS_NAK   (no ACK from device)
 *     -ETIMEDOUT / bus-fault flags -> GOV_BUS_ERR
 *     (no data-ready GPIO/status bit) -> GOV_BUS_NODATA
 * The driver logic below is unchanged between host and target; only this
 * adapter differs. Implemented for Zephyr in `drivers/hal_zephyr.c` (); the
 * host tests use the `fake_bus` implementation in `tests/`.
 * ------------------------------------------------------------------------- */

#endif /* GOV_HAL_H */
