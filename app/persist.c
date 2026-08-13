/*
 * persist.c — Zephyr settings/NVS adapter for lib/config (target-only).
 *
 * The host-portable A/B config module (lib/config) owns the torn-write-safe
 * logic (registry F15); this adapter is the thin Zephyr binding: the config
 * store's two slots map to two settings keys ("gov/slot0", "gov/slot1") in NVS
 * on the STM32 internal-flash storage_partition. Compiled only when
 * CONFIG_SETTINGS is present.
 */
#include "persist.h"

#if defined(CONFIG_SETTINGS)

#include <string.h>
#include <zephyr/settings/settings.h>
#include <zephyr/kernel.h>

/* Staging buffers filled by the settings load callback, one per slot. */
static struct gov_config_slot g_slot[2];
static bool g_slot_present[2];

/* settings_load callback: capture each stored slot verbatim. */
static int gov_set_cb(const char *name, size_t len, settings_read_cb read_cb,
		      void *cb_arg)
{
	const char *next;
	int idx = -1;
	if (settings_name_steq(name, "slot0", &next) && !next) {
		idx = 0;
	} else if (settings_name_steq(name, "slot1", &next) && !next) {
		idx = 1;
	} else {
		return -ENOENT;
	}
	if (len != sizeof(struct gov_config_slot)) {
		return -EINVAL;
	}
	ssize_t rc = read_cb(cb_arg, &g_slot[idx], sizeof(struct gov_config_slot));
	if (rc < 0) {
		return -EIO;
	}
	g_slot_present[idx] = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(gov, "gov", NULL, gov_set_cb, NULL, NULL);

/* Store adapter: reads come from the staged buffers (populated at init);
 * writes go straight to NVS via settings_save_one. */
static bool store_read(void *ctx, uint8_t idx, struct gov_config_slot *out)
{
	ARG_UNUSED(ctx);
	if (idx > 1u || !g_slot_present[idx]) {
		return false;
	}
	*out = g_slot[idx];
	return true;
}

static bool store_write(void *ctx, uint8_t idx, const struct gov_config_slot *slot)
{
	ARG_UNUSED(ctx);
	if (idx > 1u) {
		return false;
	}
	const char *key = (idx == 0u) ? "gov/slot0" : "gov/slot1";
	int rc = settings_save_one(key, slot, sizeof(*slot));
	if (rc != 0) {
		return false;
	}
	/* Keep the staged copy coherent so an in-session reload is consistent. */
	g_slot[idx] = *slot;
	g_slot_present[idx] = true;
	return true;
}

bool gov_persist_init(gov_config_ctx_t *c, struct gov_config *out)
{
	if (c == NULL) {
		return false;
	}
	memset(g_slot, 0, sizeof g_slot);
	g_slot_present[0] = false;
	g_slot_present[1] = false;

	if (settings_subsys_init() != 0) {
		return false;
	}
	/* Populates g_slot[]/g_slot_present[] via gov_set_cb. */
	if (settings_load() != 0) {
		return false;
	}

	static struct gov_config_store store;
	store.read = store_read;
	store.write = store_write;
	store.ctx = NULL;
	gov_config_init(c, &store);
	(void)gov_config_load(c, out);
	return true;
}

bool gov_persist_save(gov_config_ctx_t *c, const struct gov_config *in)
{
	return gov_config_save(c, in);
}

#endif /* CONFIG_SETTINGS */
