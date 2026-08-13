/*
 * reliable.h — stop-and-wait ARQ (window = 1) + receiver dedup.
 *
 * Sits above framing (frame.c). Host-portable, NO Zephyr includes and NO clock
 * calls: time is injected as a millisecond tick by the caller (RTOS/host owns
 * the clock), keeping this fully unit-testable. No dynamic allocation
 * (RULES R1); each side keeps exactly one bounded frame slot (PROTOCOL_SPEC §5
 * "bounded state, one retry slot, no dynamic queue").
 *
 * Semantics (PROTOCOL_SPEC §5):
 *  - Sender assigns an incrementing SEQ per reliable frame, keeps ONE
 *    outstanding, retransmits the identical bytes on ACK timeout up to
 *    GOV_MAX_RETRIES, then latches LINK_FAULT (never silently drops).
 *  - Receiver tracks the last accepted SEQ; a repeat SEQ is a duplicate:
 *    re-ACK without re-delivery (idempotent delivery). First frame after
 *    reset accepts any SEQ.
 */
#ifndef GOV_PROTO_RELIABLE_H
#define GOV_PROTO_RELIABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

/* Registered timing/retry bounds (config/registry.md §1) — never widened. */
#define GOV_ACK_TIMEOUT_MS 100u
#define GOV_MAX_RETRIES 3u

/* Emit fully-encoded frame bytes onto the wire (no allocation on our side). */
typedef void (*gov_wire_cb)(const uint8_t *frame, size_t len, void *ctx);

/* Deliver a de-duplicated application frame (same shape as gov_frame_cb). */
typedef void (*gov_deliver_cb)(uint8_t type, uint8_t seq, const uint8_t *payload,
			       uint16_t len, void *ctx);

/* ---- Sender: stop-and-wait ARQ ---- */

enum gov_tx_state {
	GOV_TX_IDLE = 0,   /* no outstanding frame; ready to send */
	GOV_TX_WAIT_ACK,   /* one frame outstanding, awaiting ACK */
	GOV_TX_LINK_FAULT, /* retries exhausted — LINK_FAULT latched */
};

enum gov_tx_status {
	GOV_TX_OK = 0,   /* frame accepted and emitted */
	GOV_TX_BUSY,     /* window full (already awaiting ACK) or faulted */
	GOV_TX_ENCODE_ERR, /* bad type/len/payload — nothing emitted */
};

struct gov_tx {
	gov_wire_cb wire;
	void *ctx;
	enum gov_tx_state state;
	uint8_t next_seq;   /* SEQ to assign to the next new frame */
	uint8_t cur_seq;    /* SEQ of the outstanding frame */
	uint8_t retries;    /* retransmits performed for the outstanding frame */
	uint32_t last_tx_ms; /* tick at which the frame was last (re)sent */
	size_t frame_len;
	uint8_t frame[GOV_FRAME_MAX]; /* buffered bytes for identical retransmit */
};

typedef struct gov_tx gov_tx_t;

/* Initialize a sender. `wire` must be non-NULL (that is the only output path). */
void gov_tx_init(gov_tx_t *tx, gov_wire_cb wire, void *ctx);

/*
 * Send a new reliable frame. Assigns the next SEQ, encodes, emits, and starts
 * the ACK timer at `now_ms`. Only valid from GOV_TX_IDLE (window = 1) — returns
 * GOV_TX_BUSY otherwise. Returns GOV_TX_ENCODE_ERR on a bad type/len/payload.
 */
enum gov_tx_status gov_tx_send(gov_tx_t *tx, uint8_t type, const uint8_t *payload,
			       uint16_t len, uint32_t now_ms);

/*
 * Feed a received ACK's acknowledged SEQ. If it matches the outstanding frame,
 * the slot clears (GOV_TX_IDLE). A non-matching or unexpected ACK is ignored.
 */
void gov_tx_on_ack(gov_tx_t *tx, uint8_t acked_seq);

/*
 * Advance the ACK timer. Call periodically with the current tick. On timeout it
 * retransmits the identical frame (up to GOV_MAX_RETRIES) or latches
 * GOV_TX_LINK_FAULT on exhaustion. Returns the (possibly new) state.
 */
enum gov_tx_state gov_tx_poll(gov_tx_t *tx, uint32_t now_ms);

/* Clear a latched LINK_FAULT back to IDLE (e.g. after operator recovery). */
void gov_tx_clear_fault(gov_tx_t *tx);

/* ---- Receiver: dedup + auto-ACK ---- */

struct gov_rx {
	gov_wire_cb wire;        /* emits the ACK frames */
	gov_deliver_cb deliver;  /* de-duplicated app delivery */
	void *ctx;
	uint8_t last_seq;        /* last accepted SEQ */
	bool have_last;          /* false until the first accepted frame */
	uint32_t dup;            /* stat_dup: duplicates seen (re-ACKed) */
};

typedef struct gov_rx gov_rx_t;

/*
 * Initialize a receiver. `wire` must be non-NULL (ACKs go out through it);
 * `deliver` may be NULL (frames are then only ACKed/deduped, not delivered).
 */
void gov_rx_init(gov_rx_t *rx, gov_wire_cb wire, gov_deliver_cb deliver, void *ctx);

/*
 * Process a decoded reliable frame (route only reliable TYPEs here; ACK/NAK go
 * to gov_tx_on_ack). Delivers first-seen SEQs and ACKs them; a duplicate SEQ is
 * re-ACKed and counted in `dup` but NOT re-delivered.
 */
void gov_rx_on_frame(gov_rx_t *rx, uint8_t type, uint8_t seq,
		     const uint8_t *payload, uint16_t len);

#endif /* GOV_PROTO_RELIABLE_H */
