/*
 * hal_zephyr.c — Zephyr I2C binding for the abstract gov_bus HAL (drivers/hal.h).
 *
 *  target adapter — the concrete Zephyr implementation of the
 * `struct gov_bus` HAL specified in hal.h. It fills a
 * `struct gov_bus` whose ctx is the i2c1 controller `const struct device *`
 * and translates Zephyr return codes to gov_bus_status_t. The host-portable
 * sensor logic (drivers/sensor.c) is unchanged between host and target — only
 * this adapter differs.
 *
 * Compiled only when CONFIG_I2C is present (the STM32 emulated-hardware target);
 * on qemu_cortex_m3 / native_sim (no I2C controller) the sensor path is exercised
 * by the host fake_bus instead, so this file is excluded from those builds.
 */
#include "hal_zephyr.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <errno.h>

/* i2c1 controller from the device tree. */
#define GOV_I2C_NODE DT_NODELABEL(i2c1)
static const struct device *const gov_i2c_dev = DEVICE_DT_GET(GOV_I2C_NODE);

/* Map a Zephyr I2C errno to the transport-level gov_bus_status_t.
 * -EIO → NAK (device didn't ACK); timeout/other bus faults → ERR. */
static gov_bus_status_t map_status(int rc)
{
	switch (rc) {
	case 0:
		return GOV_BUS_OK;
	case -EIO:
		return GOV_BUS_NAK;
	case -ETIMEDOUT:
	case -EBUSY:
	default:
		return (rc == 0) ? GOV_BUS_OK : GOV_BUS_ERR;
	}
}

/* cppcheck-suppress constParameterCallback ; ctx must stay non-const to match
 * the struct gov_bus vtable signature (drivers/hal.h) — the fake_bus impl and
 * the ABI require it. Deviation logged in docs/RULES.md. */
static gov_bus_status_t zbus_read(void *ctx, uint8_t addr, uint8_t reg,
				  uint8_t *buf, size_t len)
{
	const struct device *dev = (const struct device *)ctx;
	if (dev == NULL || !device_is_ready(dev)) {
		return GOV_BUS_ERR;
	}
	int rc = i2c_burst_read(dev, addr, reg, buf, len);
	return map_status(rc);
}

/* cppcheck-suppress constParameterCallback ; see zbus_read (vtable signature). */
static gov_bus_status_t zbus_write(void *ctx, uint8_t addr, uint8_t reg,
				   const uint8_t *buf, size_t len)
{
	const struct device *dev = (const struct device *)ctx;
	if (dev == NULL || !device_is_ready(dev)) {
		return GOV_BUS_ERR;
	}
	int rc = i2c_burst_write(dev, addr, reg, buf, len);
	return map_status(rc);
}

/* cppcheck-suppress constParameterCallback ; see zbus_read (vtable signature). */
static gov_bus_status_t zbus_probe(void *ctx, uint8_t addr)
{
	const struct device *dev = (const struct device *)ctx;
	if (dev == NULL || !device_is_ready(dev)) {
		return GOV_BUS_ERR;
	}
	uint8_t dummy = 0;
	/* A zero-length write is the canonical I2C presence probe. */
	int rc = i2c_write(dev, &dummy, 0, addr);
	return map_status(rc);
}

/* Fill `bus` with the Zephyr i2c1 binding. Returns false if the controller is
 * not ready (caller treats that as a hard init fault → SAFE_STOP via T2). */
bool gov_hal_zephyr_init(struct gov_bus *bus)
{
	if (bus == NULL || !device_is_ready(gov_i2c_dev)) {
		return false;
	}
	bus->read = zbus_read;
	bus->write = zbus_write;
	bus->probe = zbus_probe;
	bus->ctx = (void *)gov_i2c_dev;
	return true;
}
