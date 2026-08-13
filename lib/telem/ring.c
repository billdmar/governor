/*
 * ring.c -- fixed-capacity SPSC ring buffer (see ring.h).
 *
 * Indices are free-running mod GOV_TELEM_RING_SLOTS. The producer advances
 * `head`, the consumer advances `tail`; neither writes the other's index, so
 * no lock is required for one producer + one consumer. No allocation (R1), no
 * recursion (R7), all loops/bounds fixed (R8).
 */
#include "ring.h"

static uint32_t next(uint32_t idx)
{
	uint32_t n = idx + 1u;

	return (n == GOV_TELEM_RING_SLOTS) ? 0u : n;
}

void gov_telem_ring_init(struct gov_telem_ring *r)
{
	if (r == NULL) {
		return;
	}
	r->head = 0u;
	r->tail = 0u;
	r->drops = 0u;
}

bool gov_telem_ring_push(struct gov_telem_ring *r, const struct gov_telem_record *rec)
{
	uint32_t h;

	if (r == NULL || rec == NULL) {
		return false;
	}

	h = r->head;
	if (next(h) == r->tail) {
		/* Full: reject and FLAG the loss -- never overwrite, never block. */
		r->drops++;
		return false;
	}

	r->buf[h] = *rec;
	r->head = next(h);
	return true;
}

bool gov_telem_ring_pop(struct gov_telem_ring *r, struct gov_telem_record *out)
{
	uint32_t t;

	if (r == NULL || out == NULL) {
		return false;
	}

	t = r->tail;
	if (t == r->head) {
		return false; /* empty */
	}

	*out = r->buf[t];
	r->tail = next(t);
	return true;
}

uint32_t gov_telem_ring_count(const struct gov_telem_ring *r)
{
	if (r == NULL) {
		return 0u;
	}
	return (r->head - r->tail + GOV_TELEM_RING_SLOTS) % GOV_TELEM_RING_SLOTS;
}

bool gov_telem_ring_empty(const struct gov_telem_ring *r)
{
	if (r == NULL) {
		return true;
	}
	return r->head == r->tail;
}

bool gov_telem_ring_full(const struct gov_telem_ring *r)
{
	if (r == NULL) {
		return false;
	}
	return next(r->head) == r->tail;
}

uint32_t gov_telem_ring_drops(const struct gov_telem_ring *r)
{
	if (r == NULL) {
		return 0u;
	}
	return r->drops;
}
