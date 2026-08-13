/*
 * sensor.c — host-portable sensor driver logic (see sensor.h).
 *
 * Pure C, no Zephyr. Reads through the abstract HAL, validates, and reports
 * health via GOV_SFLAG_* flags. The driver reports; it never decides safety
 * state (SAFETY_SM.md §1). No allocation (RULES.md R1), no recursion (R7).
 */
#include <stddef.h>

#include "sensor.h"

/*
 * Decode `len` big-endian bytes into a signed sample. This is a deliberately
 * simple unsigned assemble; a real sensor's sign/scale handling is
 * sensor-specific and expressed through the [range_min,range_max] config. The
 * cast is explicit and value-preserving for the sensor's declared range
 * (RULES.md R5).
 */
static int32_t decode_sample(const uint8_t *buf, size_t len)
{
	uint32_t raw = 0;

	for (size_t i = 0; i < len; i++) {
		raw = (raw << 8) | (uint32_t)buf[i];
	}

	return (int32_t)raw;
}

/*
 * Read the sample register with the F04 NAK retry policy: one initial attempt
 * plus up to cfg.nak_retries retries, stopping early on any non-NAK result.
 * Returns the final transport status; `buf` holds the last read's bytes.
 */
static gov_bus_status_t read_with_retry(struct gov_sensor *s, uint8_t *buf)
{
	gov_bus_status_t st;
	uint8_t tries = 0;

	do {
		st = s->cfg.bus->read(s->cfg.bus->ctx, s->cfg.addr, s->cfg.reg,
				      buf, s->cfg.read_len);
		if (st != GOV_BUS_NAK) {
			break;
		}
		tries = (uint8_t)(tries + 1);
	} while (tries <= s->cfg.nak_retries);

	return st;
}

/*
 * Handle a successful transfer: a working bus clears transport + dropout
 * faults, then the decoded sample runs range (F03) and stuck (F02) checks.
 */
static void gov_sensor_on_sample(struct gov_sensor *s, const uint8_t *buf,
				 struct gov_sample_slot *slot)
{
	int32_t sample = decode_sample(buf, s->cfg.read_len);

	/* A successful read means the bus is alive and data is flowing. */
	s->flags &= (uint32_t) ~(GOV_SFLAG_BUS_ERR | GOV_SFLAG_BUS_NAK |
				 GOV_SFLAG_DROPOUT);
	s->nodata_cnt = 0;

	if (sample < s->cfg.range_min || sample > s->cfg.range_max) {
		/* F03: garbage. Reject the sample, hold last-good, flag it.
		 * Do NOT publish and do NOT touch stuck tracking. */
		s->flags |= GOV_SFLAG_RANGE;
		return;
	}

	/* In-range sample accepted → any prior range fault recovers. */
	s->flags &= (uint32_t)~GOV_SFLAG_RANGE;

	/* F02 stuck detection over consecutive accepted in-range samples. */
	if (s->has_last_good && sample == s->last_good) {
		if (s->stuck_cnt < s->cfg.stuck_limit) {
			s->stuck_cnt = (uint16_t)(s->stuck_cnt + 1);
		}
	} else {
		s->stuck_cnt = 0;
	}

	if (s->stuck_cnt >= s->cfg.stuck_limit) {
		s->flags |= GOV_SFLAG_STUCK;
	} else {
		/* Variance returned within the window → stuck fault clears. */
		s->flags &= (uint32_t)~GOV_SFLAG_STUCK;
	}

	s->last_good = sample;
	s->has_last_good = true;

	if (slot != NULL) {
		gov_slot_push(slot, sample);
	}
}

void gov_sensor_init(struct gov_sensor *s, const struct gov_sensor_cfg *cfg)
{
	s->cfg = *cfg;

	/* Clamp the read length into the static buffer bounds (RULES.md R8). */
	if (s->cfg.read_len == 0) {
		s->cfg.read_len = 1;
	}
	if (s->cfg.read_len > GOV_SENSOR_MAX_READ) {
		s->cfg.read_len = GOV_SENSOR_MAX_READ;
	}

	s->flags = GOV_SFLAG_NONE;
	s->last_good = 0;
	s->has_last_good = false;
	s->nodata_cnt = 0;
	s->stuck_cnt = 0;
}

gov_sensor_health_t gov_sensor_poll(struct gov_sensor *s, struct gov_sample_slot *slot)
{
	uint8_t buf[GOV_SENSOR_MAX_READ];
	gov_bus_status_t st = read_with_retry(s, buf);

	switch (st) {
	case GOV_BUS_OK:
		gov_sensor_on_sample(s, buf, slot);
		break;
	case GOV_BUS_NAK:
		/* F04: NAK persisted past retries. Flag, hold last-good. */
		s->flags |= GOV_SFLAG_BUS_NAK;
		break;
	case GOV_BUS_ERR:
		/* F05: bus-level fault, not retryable. Flag, hold last-good.
		 * Escalation to SAFE_STOP is the safety SM's job (T7). */
		s->flags |= GOV_SFLAG_BUS_ERR;
		break;
	case GOV_BUS_NODATA:
		/* F01: no data-ready this poll; DROPOUT once sustained N polls. */
		if (s->nodata_cnt < s->cfg.dropout_limit) {
			s->nodata_cnt = (uint16_t)(s->nodata_cnt + 1);
		}
		if (s->nodata_cnt >= s->cfg.dropout_limit) {
			s->flags |= GOV_SFLAG_DROPOUT;
		}
		break;
	default:
		/* gov_bus_status_t is a closed enum; an unknown value is a
		 * contract violation. Fail loud (RULES.md R4): flag bus error
		 * rather than silently ignoring it. */
		s->flags |= GOV_SFLAG_BUS_ERR;
		break;
	}

	return gov_sensor_health(s);
}

uint32_t gov_sensor_faults(const struct gov_sensor *s)
{
	return s->flags;
}

gov_sensor_health_t gov_sensor_health(const struct gov_sensor *s)
{
	return (s->flags == GOV_SFLAG_NONE) ? GOV_SENSOR_HEALTHY : GOV_SENSOR_FAULTED;
}

int32_t gov_sensor_value(const struct gov_sensor *s)
{
	return s->last_good;
}

bool gov_sensor_has_value(const struct gov_sensor *s)
{
	return s->has_last_good;
}

void gov_slot_init(struct gov_sample_slot *slot)
{
	slot->value = 0;
	slot->fresh = false;
}

void gov_slot_push(struct gov_sample_slot *slot, int32_t value)
{
	/* Newest-wins 1-deep mailbox: overwrite unconditionally. */
	slot->value = value;
	slot->fresh = true;
}

bool gov_slot_get(struct gov_sample_slot *slot, int32_t *out)
{
	if (!slot->fresh) {
		return false;
	}

	*out = slot->value;
	slot->fresh = false;
	return true;
}
