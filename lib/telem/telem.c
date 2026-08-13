/*
 * telem.c -- DATA + HEARTBEAT payload encode/decode (big-endian, fixed layout).
 *
 * See telem.h for the on-wire layouts. All puts/gets are length-checked against
 * the caller's buffer before any access (RULES R8) and use explicit shifts, so
 * there is no dependence on host endianness or unaligned-access behavior
 * (RULES R10). No allocation, no recursion (R1/R7).
 */
#include "telem.h"

/* --- big-endian scalar put/get helpers (advance the cursor) --- */

static void put_u32(uint8_t *buf, size_t *pos, uint32_t v)
{
	buf[*pos + 0] = (uint8_t)((v >> 24) & 0xFFu);
	buf[*pos + 1] = (uint8_t)((v >> 16) & 0xFFu);
	buf[*pos + 2] = (uint8_t)((v >> 8) & 0xFFu);
	buf[*pos + 3] = (uint8_t)(v & 0xFFu);
	*pos += 4u;
}

static uint32_t get_u32(const uint8_t *buf, size_t *pos)
{
	uint32_t v = ((uint32_t)buf[*pos + 0] << 24) | ((uint32_t)buf[*pos + 1] << 16) |
		     ((uint32_t)buf[*pos + 2] << 8) | (uint32_t)buf[*pos + 3];
	*pos += 4u;
	return v;
}

/*
 * i32 is transported as its u32 two's-complement bit pattern. The cast in each
 * direction is value-preserving on the two's-complement targets this project
 * runs on; done via an explicit u32 round-trip so no signed overflow is relied
 * upon (RULES R5/R10).
 */
static void put_i32(uint8_t *buf, size_t *pos, int32_t v)
{
	put_u32(buf, pos, (uint32_t)v);
}

static int32_t get_i32(const uint8_t *buf, size_t *pos)
{
	return (int32_t)get_u32(buf, pos);
}

size_t gov_telem_encode(const struct gov_telem_record *rec, uint8_t *out, size_t out_cap)
{
	size_t pos = 0u;

	if (rec == NULL || out == NULL || out_cap < GOV_TELEM_DATA_LEN) {
		return 0u;
	}

	put_u32(out, &pos, rec->tick);
	put_i32(out, &pos, rec->setpoint);
	put_i32(out, &pos, rec->measurement);
	put_i32(out, &pos, rec->output);
	out[pos] = rec->state;
	pos += 1u;
	put_u32(out, &pos, rec->fault_flags);

	return pos; /* == GOV_TELEM_DATA_LEN */
}

bool gov_telem_decode(const uint8_t *in, size_t len, struct gov_telem_record *rec)
{
	size_t pos = 0u;

	if (in == NULL || rec == NULL || len < GOV_TELEM_DATA_LEN) {
		return false;
	}

	rec->tick = get_u32(in, &pos);
	rec->setpoint = get_i32(in, &pos);
	rec->measurement = get_i32(in, &pos);
	rec->output = get_i32(in, &pos);
	rec->state = in[pos];
	pos += 1u;
	rec->fault_flags = get_u32(in, &pos);

	return true;
}

size_t gov_heartbeat_encode(const struct gov_heartbeat *hb, uint8_t *out, size_t out_cap)
{
	size_t pos = 0u;

	if (hb == NULL || out == NULL || out_cap < GOV_TELEM_HEARTBEAT_LEN) {
		return 0u;
	}

	out[pos] = hb->state;
	pos += 1u;
	put_u32(out, &pos, hb->fault_flags);
	put_u32(out, &pos, hb->uptime_count);

	return pos; /* == GOV_TELEM_HEARTBEAT_LEN */
}

bool gov_heartbeat_decode(const uint8_t *in, size_t len, struct gov_heartbeat *hb)
{
	size_t pos = 0u;

	if (in == NULL || hb == NULL || len < GOV_TELEM_HEARTBEAT_LEN) {
		return false;
	}

	hb->state = in[pos];
	pos += 1u;
	hb->fault_flags = get_u32(in, &pos);
	hb->uptime_count = get_u32(in, &pos);

	return true;
}
