/*
 * reliable.c — stop-and-wait ARQ (window = 1) + receiver dedup.
 *
 * See reliable.h for the contract. Time is injected (now_ms); this file makes
 * NO clock calls and does NO allocation (RULES R1). Timeout comparison uses
 * unsigned tick subtraction so it is correct across a 32-bit millisecond wrap.
 */
#include <string.h>

#include "reliable.h"

/* ---- Sender ---- */

void gov_tx_init(gov_tx_t *tx, gov_wire_cb wire, void *ctx)
{
	if (tx == NULL) {
		return;
	}
	memset(tx, 0, sizeof(*tx));
	tx->wire = wire;
	tx->ctx = ctx;
	tx->state = GOV_TX_IDLE;
}

enum gov_tx_status gov_tx_send(gov_tx_t *tx, uint8_t type, const uint8_t *payload,
			       uint16_t len, uint32_t now_ms)
{
	size_t n;

	if ((tx == NULL) || (tx->wire == NULL)) {
		return GOV_TX_BUSY;
	}
	if (tx->state != GOV_TX_IDLE) {
		return GOV_TX_BUSY; /* window = 1, or LINK_FAULT latched */
	}

	n = gov_frame_encode(type, tx->next_seq, payload, len, tx->frame,
			     sizeof(tx->frame));
	if (n == 0u) {
		return GOV_TX_ENCODE_ERR;
	}

	tx->frame_len = n;
	tx->cur_seq = tx->next_seq;
	tx->next_seq = (uint8_t)(tx->next_seq + 1u); /* wraps mod 256 (§5) */
	tx->retries = 0u;
	tx->last_tx_ms = now_ms;
	tx->state = GOV_TX_WAIT_ACK;

	tx->wire(tx->frame, tx->frame_len, tx->ctx);
	return GOV_TX_OK;
}

void gov_tx_on_ack(gov_tx_t *tx, uint8_t acked_seq)
{
	if (tx == NULL) {
		return;
	}
	if ((tx->state == GOV_TX_WAIT_ACK) && (acked_seq == tx->cur_seq)) {
		tx->state = GOV_TX_IDLE;
	}
	/* Stale/unexpected ACK: ignore (idempotent, no state churn). */
}

enum gov_tx_state gov_tx_poll(gov_tx_t *tx, uint32_t now_ms)
{
	if (tx == NULL) {
		return GOV_TX_LINK_FAULT;
	}
	if (tx->state != GOV_TX_WAIT_ACK) {
		return tx->state;
	}

	/* Unsigned subtraction => wrap-safe elapsed time. */
	if ((uint32_t)(now_ms - tx->last_tx_ms) < GOV_ACK_TIMEOUT_MS) {
		return tx->state; /* still within the ACK window */
	}

	if (tx->retries >= GOV_MAX_RETRIES) {
		/* Retries exhausted: raise LINK_FAULT, never silently drop. */
		tx->state = GOV_TX_LINK_FAULT;
		return tx->state;
	}

	tx->retries++;
	tx->last_tx_ms = now_ms;
	tx->wire(tx->frame, tx->frame_len, tx->ctx); /* identical bytes, same SEQ */
	return tx->state;
}

void gov_tx_clear_fault(gov_tx_t *tx)
{
	if (tx == NULL) {
		return;
	}
	if (tx->state == GOV_TX_LINK_FAULT) {
		tx->state = GOV_TX_IDLE;
	}
}

/* ---- Receiver ---- */

void gov_rx_init(gov_rx_t *rx, gov_wire_cb wire, gov_deliver_cb deliver, void *ctx)
{
	if (rx == NULL) {
		return;
	}
	memset(rx, 0, sizeof(*rx));
	rx->wire = wire;
	rx->deliver = deliver;
	rx->ctx = ctx;
	rx->have_last = false;
}

/* Emit an ACK frame carrying `seq` in its 1-byte payload (PROTOCOL_SPEC §3). */
static void send_ack(gov_rx_t *rx, uint8_t seq)
{
	uint8_t out[GOV_FRAME_MAX];
	size_t n;

	/* ACKs are not sequenced (§5); SEQ field carries 0, payload carries the
	 * acked SEQ. */
	n = gov_frame_encode((uint8_t)GOV_TYPE_ACK, 0u, &seq, 1u, out, sizeof(out));
	if (n != 0u) {
		rx->wire(out, n, rx->ctx);
	}
}

void gov_rx_on_frame(gov_rx_t *rx, uint8_t type, uint8_t seq,
		     const uint8_t *payload, uint16_t len)
{
	bool duplicate;

	if ((rx == NULL) || (rx->wire == NULL)) {
		return;
	}

	duplicate = rx->have_last && (seq == rx->last_seq);

	if (duplicate) {
		rx->dup++;         /* stat_dup++ */
		send_ack(rx, seq); /* re-ACK, but do NOT re-deliver (§5) */
		return;
	}

	rx->last_seq = seq;
	rx->have_last = true;

	if (rx->deliver != NULL) {
		rx->deliver(type, seq, payload, len, rx->ctx);
	}
	send_ack(rx, seq);
}
