/*
 * persist.h — target-only persistence adapter binding lib/config (host-portable
 * A/B config) to Zephyr settings/NVS on the STM32 internal flash. Available
 * only when CONFIG_SETTINGS (the stm32f103_mini emulated-hardware target); on
 * qemu/native_sim (no flash storage) the node uses compile-time defaults.
 */
#ifndef GOV_PERSIST_H
#define GOV_PERSIST_H

#include <stdbool.h>

#include "config.h"

/* Initialize settings/NVS and bind the config store. Returns true on success.
 * On success `out` holds the loaded config (or defaults on first boot). */
bool gov_persist_init(gov_config_ctx_t *c, struct gov_config *out);

/* Persist `in` (writes the inactive A/B slot via NVS). Returns true on a
 * completed write; false if the medium tore/failed (old slot stays valid). */
bool gov_persist_save(gov_config_ctx_t *c, const struct gov_config *in);

#endif /* GOV_PERSIST_H */
