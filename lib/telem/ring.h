/*
 * ring.h -- fixed-capacity SPSC ring buffer of telemetry records.
 *
 * Backs the `telem_ring` (depth GOV_TELEM_RING_DEPTH = 8, TASKS sec 2): the
 * telemetry producer (control/health context) pushes records; the link_tx
 * consumer pops them. Single-producer/single-consumer, lock-free by contract
 * (the producer owns `head`, the consumer owns `tail`; `drops` is
 * producer-owned).
 *
 * Overflow policy (enforces the design notes "never fail silent"): when full, a push
 * is REJECTED and a dropped-count is incremented -- it never overwrites unread
 * data and never blocks. The dropped count is telemetry-visible so loss is
 * always flagged, never silent (registry F-anti-goal). No allocation; all
 * storage is inline and static-sized.
 */
#ifndef GOV_TELEM_RING_H
#define GOV_TELEM_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telem.h"
#include "telem_config.h"

/*
 * Capacity is GOV_TELEM_RING_DEPTH usable slots. One slot is intentionally left
 * unused (classic full/empty disambiguation) so head==tail unambiguously means
 * empty; the backing array therefore holds DEPTH+1 records.
 */
#define GOV_TELEM_RING_SLOTS (GOV_TELEM_RING_DEPTH + 1u)

struct gov_telem_ring {
	struct gov_telem_record buf[GOV_TELEM_RING_SLOTS];
	uint32_t head; /* producer writes here next (producer-owned) */
	uint32_t tail; /* consumer reads here next (consumer-owned)  */
	uint32_t drops; /* count of pushes rejected due to full (producer-owned) */
};

/* Reset to empty. Call once at init, before producer/consumer run. */
void gov_telem_ring_init(struct gov_telem_ring *r);

/*
 * Push one record (producer side). Returns true if enqueued; false if the ring
 * was full -- in which case `rec` is dropped and `drops` is incremented (never
 * overwrites unread data, never blocks).
 */
bool gov_telem_ring_push(struct gov_telem_ring *r, const struct gov_telem_record *rec);

/*
 * Pop the oldest record into *out (consumer side, FIFO order). Returns true if
 * a record was dequeued; false if the ring was empty (*out untouched).
 */
bool gov_telem_ring_pop(struct gov_telem_ring *r, struct gov_telem_record *out);

/* Number of records currently queued (0..GOV_TELEM_RING_DEPTH). */
uint32_t gov_telem_ring_count(const struct gov_telem_ring *r);

/* True if no records are queued. */
bool gov_telem_ring_empty(const struct gov_telem_ring *r);

/* True if the ring is at capacity (next push would be dropped). */
bool gov_telem_ring_full(const struct gov_telem_ring *r);

/* Total pushes rejected due to overflow since init (telemetry loss counter). */
uint32_t gov_telem_ring_drops(const struct gov_telem_ring *r);

#endif /* GOV_TELEM_RING_H */
