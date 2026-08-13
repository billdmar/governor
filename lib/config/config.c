/*
 * config.c — host-portable persistent config (A/B double-buffer). See config.h.
 * Torn-write safety (registry F15) is the whole point: save() writes only the
 * inactive slot, so an interrupted write never damages the currently-active one.
 */
#include "config.h"

#include <string.h>

#include "crc16.h" /* gov_crc16_ccitt — the same CRC the link layer uses */

/* Sensible power-on defaults (match the app's runtime defaults). */
const struct gov_config GOV_CONFIG_DEFAULTS = {
	.setpoint = 50,
	.kp_milli = 3500, /* 3.5  */
	.ki_milli = 18000, /* 18.0 */
	.kd_milli = 60,   /* 0.06 */
	.boot_count = 0,
};

/* CRC covers everything from `magic` up to (not including) the crc field. */
uint16_t gov_config_slot_crc(const struct gov_config_slot *slot)
{
	const size_t crc_len =
		offsetof(struct gov_config_slot, crc);
	return gov_crc16_ccitt(0xFFFFu, (const uint8_t *)slot, crc_len);
}

bool gov_config_slot_valid(const struct gov_config_slot *slot)
{
	if (slot == NULL) {
		return false;
	}
	if (slot->magic != GOV_CONFIG_MAGIC || slot->version != GOV_CONFIG_VERSION) {
		return false;
	}
	return slot->crc == gov_config_slot_crc(slot);
}

void gov_config_init(gov_config_ctx_t *c, const struct gov_config_store *store)
{
	if (c == NULL || store == NULL) {
		return;
	}
	c->store = *store;
	c->cfg = GOV_CONFIG_DEFAULTS;
	c->last_seq = 0u;
	c->active_idx = 0u;
	c->valid = false;
}

/* Wrap-aware "is a strictly newer than b" for monotonic uint32 sequence nums. */
static bool seq_newer(uint32_t a, uint32_t b)
{
	return (uint32_t)(a - b) < 0x80000000u && a != b;
}

gov_config_status_t gov_config_load(gov_config_ctx_t *c, struct gov_config *out)
{
	if (c == NULL) {
		return GOV_CONFIG_DEFAULTED;
	}

	struct gov_config_slot s0, s1;
	bool r0 = c->store.read != NULL && c->store.read(c->store.ctx, 0u, &s0);
	bool r1 = c->store.read != NULL && c->store.read(c->store.ctx, 1u, &s1);
	bool v0 = r0 && gov_config_slot_valid(&s0);
	bool v1 = r1 && gov_config_slot_valid(&s1);

	gov_config_status_t status;
	if (v0 && v1) {
		/* Both valid: newest seq wins. */
		if (seq_newer(s1.seq, s0.seq)) {
			c->cfg = s1.payload; c->last_seq = s1.seq; c->active_idx = 1u;
		} else {
			c->cfg = s0.payload; c->last_seq = s0.seq; c->active_idx = 0u;
		}
		status = GOV_CONFIG_LOADED;
	} else if (v0) {
		c->cfg = s0.payload; c->last_seq = s0.seq; c->active_idx = 0u;
		/* If slot 1 was present-but-invalid, we recovered from a torn write. */
		status = r1 ? GOV_CONFIG_RECOVERED : GOV_CONFIG_LOADED;
	} else if (v1) {
		c->cfg = s1.payload; c->last_seq = s1.seq; c->active_idx = 1u;
		status = r0 ? GOV_CONFIG_RECOVERED : GOV_CONFIG_LOADED;
	} else {
		/* Neither valid: clean first boot (or both erased) → defaults. */
		c->cfg = GOV_CONFIG_DEFAULTS;
		c->last_seq = 0u;
		c->active_idx = 0u;
		status = GOV_CONFIG_DEFAULTED;
	}

	c->valid = true;
	if (out != NULL) {
		*out = c->cfg;
	}
	return status;
}

bool gov_config_save(gov_config_ctx_t *c, const struct gov_config *in)
{
	if (c == NULL || in == NULL || c->store.write == NULL) {
		return false;
	}

	/* Write to the INACTIVE slot so a torn write can't damage the active one.
	 * If we have never established an active slot, treat slot 0 as active so we
	 * first write slot 1 (both-invalid case still lands somewhere valid). */
	uint8_t target = c->valid ? (uint8_t)(c->active_idx ^ 1u) : 1u;

	struct gov_config_slot slot;
	memset(&slot, 0, sizeof slot);
	slot.magic = GOV_CONFIG_MAGIC;
	slot.version = GOV_CONFIG_VERSION;
	slot.seq = c->last_seq + 1u; /* strictly newer than the active slot */
	slot.payload = *in;
	slot.crc = gov_config_slot_crc(&slot);

	if (!c->store.write(c->store.ctx, target, &slot)) {
		/* Torn/failed write: the active slot is untouched — no corruption. */
		return false;
	}

	/* Commit: the freshly written slot is now active. */
	c->cfg = *in;
	c->last_seq = slot.seq;
	c->active_idx = target;
	c->valid = true;
	return true;
}
