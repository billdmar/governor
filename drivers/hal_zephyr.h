/*
 * hal_zephyr.h — target-only Zephyr I2C binding for the gov_bus HAL.
 * Declared separately from hal.h so the host-portable sensor logic never sees
 * Zephyr. Only available when CONFIG_I2C (the STM32 emulated-hardware target).
 */
#ifndef GOV_HAL_ZEPHYR_H
#define GOV_HAL_ZEPHYR_H

#include <stdbool.h>

#include "hal.h"

/* Emulated telemetry sensor address on i2c1 (boards/stm32f103_mini.overlay). */
#define GOV_SENSOR_I2C_ADDR 0x48u
#define GOV_SENSOR_I2C_REG  0x00u

/* Bind `bus` to the Zephyr i2c1 controller. Returns false if not ready. */
bool gov_hal_zephyr_init(struct gov_bus *bus);

#endif /* GOV_HAL_ZEPHYR_H */
