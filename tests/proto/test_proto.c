/*
 * test_proto.c — host unit + property tests for lib/proto (crc16, frame,
 * reliable). Single translation unit on purpose: gov_test.h keeps its counters
 * in file-static storage, so all GOV_CHECK/GOV_RUN and the final
 * GOV_TEST_SUMMARY must live in one TU to aggregate correctly.
 *
 * Coverage map (per SA-proto task + PROTOCOL_SPEC):
 *  - CRC known-answer vectors (§6)
 *  - round-trip decode(encode(x)) over many type/seq/len incl 0 and MAX (§2/§4)
 *  - single-bit-flip rejection => no delivery, crc/len stat increments (§4)
 *  - resync: garbage ++ frame still delivers (§4)
 *  - concatenation: encode(A)++encode(B) delivers A then B (§4)
 *  - unknown TYPE counted, not delivered (§3)
 *  - LEN > MAX rejected without overread (§2/§4, ASan-verified)
 *  - dedup: duplicate SEQ re-ACKed, delivered once (§5)
 *  - ARQ: SEQ assignment/wrap, retransmit x MAX_RETRIES, LINK_FAULT (§5)
 */
#include "gov_test.h"
#include "crc16.h"
#include "proto.h"
#include "reliable.h"

/* ================= deterministic PRNG (reproducible replays) ============== */
static uint32_t g_rng = 0x12345678u;

static uint32_t rng_next(void)
{
	uint32_t x = g_rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_rng = x;
	return x;
}

static uint8_t rng_byte(void)
{
	return (uint8_t)(rng_next() & 0xFFu);
}

/* ============================ CRC-16 vectors ============================== */

static void test_crc_check_value(void)
{
	const uint8_t msg[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

	/* Canonical CCITT-FALSE check value. */
	GOV_CHECK_EQ(gov_crc16_ccitt(0xFFFFu, msg, sizeof(msg)), 0x29B1);
}

static void test_crc_empty_is_seed(void)
{
	GOV_CHECK_EQ(gov_crc16_ccitt(0xFFFFu, NULL, 0u), 0xFFFF);
}

static void test_crc_single_byte(void)
{
	const uint8_t a = 0x00u;

	GOV_CHECK_EQ(gov_crc16_ccitt(0xFFFFu, &a, 1u), 0xE1F0);
}

static void test_crc_incremental_equals_bulk(void)
{
	const uint8_t msg[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
	uint16_t bulk = gov_crc16_ccitt(0xFFFFu, msg, sizeof(msg));
	uint16_t inc = 0xFFFFu;

	for (size_t i = 0u; i < sizeof(msg); i++) {
		inc = gov_crc16_ccitt(inc, &msg[i], 1u);
	}
	GOV_CHECK_EQ(inc, bulk);
}

/* ============================ framing helpers ============================= */

struct capture {
	int count;
	uint8_t type;
	uint8_t seq;
	uint16_t len;
	uint8_t payload[GOV_MAX_PAYLOAD];
};

static void cap_cb(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
		   void *ctx)
{
	struct capture *c = (struct capture *)ctx;

	c->count++;
	c->type = type;
	c->seq = seq;
	c->len = len;
	if (len > 0u) {
		memcpy(c->payload, payload, len);
	}
}

static void feed(gov_decoder_t *d, const uint8_t *buf, size_t n)
{
	for (size_t i = 0u; i < n; i++) {
		gov_decoder_push(d, buf[i]);
	}
}

/* ============================ framing tests =============================== */

static void test_roundtrip_many(void)
{
	const uint8_t types[] = { GOV_TYPE_DATA, GOV_TYPE_ACK, GOV_TYPE_NAK,
				  GOV_TYPE_CMD, GOV_TYPE_HEARTBEAT,
				  GOV_TYPE_CFG_WRITE };
	uint8_t payload[GOV_MAX_PAYLOAD];
	uint8_t frame[GOV_FRAME_MAX];

	for (size_t ti = 0u; ti < sizeof(types); ti++) {
		const uint16_t lens[] = { 0u, 1u, 63u, GOV_MAX_PAYLOAD };

		for (size_t li = 0u; li < (sizeof(lens) / sizeof(lens[0])); li++) {
			uint16_t len = lens[li];
			uint8_t seq = (uint8_t)(rng_next() & 0xFFu);
			struct capture cap = { 0 };
			gov_decoder_t dec;
			size_t n;

			for (uint16_t k = 0u; k < len; k++) {
				payload[k] = rng_byte();
			}

			n = gov_frame_encode(types[ti], seq, payload, len, frame,
					     sizeof(frame));
			GOV_CHECK(n != 0u);

			gov_decoder_init(&dec, cap_cb, &cap);
			feed(&dec, frame, n);

			GOV_CHECK_EQ(cap.count, 1);
			GOV_CHECK_EQ(cap.type, types[ti]);
			GOV_CHECK_EQ(cap.seq, seq);
			GOV_CHECK_EQ(cap.len, len);
			GOV_CHECK(memcmp(cap.payload, payload, len) == 0);
			GOV_CHECK_EQ(dec.stats.crc_err, 0);
			GOV_CHECK_EQ(dec.stats.len_err, 0);
		}
	}
}

static void test_single_bit_flip_rejected(void)
{
	uint8_t payload[16];
	uint8_t frame[GOV_FRAME_MAX];
	size_t n;

	for (size_t i = 0u; i < sizeof(payload); i++) {
		payload[i] = rng_byte();
	}
	n = gov_frame_encode(GOV_TYPE_DATA, 0x11u, payload, sizeof(payload),
			     frame, sizeof(frame));
	GOV_CHECK(n != 0u);

	/*
	 * Flip every bit of every byte from VER onward (index >= 1; SOF is the
	 * resync anchor, not CRC-covered — flipping it is framing loss, tested
	 * by the resync case). The load-bearing safety invariant (PROTOCOL_SPEC
	 * §4, matrix F06) is NO BAD DELIVERY — asserted for EVERY flip below.
	 *
	 * Which mechanism *detects* the flip depends on the field (honest scope,
	 * not a blanket "CRC always fires"):
	 *  - TYPE/SEQ/PAYLOAD/CRC: framing length is unchanged and the byte is
	 *    CRC-covered ⇒ caught by the CRC ⇒ stat_crc_err == 1.
	 *  - LEN (idx 4,5): a flip either pushes LEN out of range (stat_len_err),
	 *    shrinks it (CRC then reads the wrong bytes ⇒ stat_crc_err), or
	 *    inflates it in-range (the length-driven decoder correctly stalls
	 *    awaiting more bytes ⇒ no delivery, no counter). No delivery holds.
	 *  - VER (idx 1): rejected by the §4 version check; the spec defines no
	 *    version-error counter, so only the no-delivery guarantee applies.
	 */
	for (size_t byte = 1u; byte < n; byte++) {
		for (unsigned bit = 0u; bit < 8u; bit++) {
			uint8_t corrupt[GOV_FRAME_MAX];
			struct capture cap = { 0 };
			gov_decoder_t dec;

			memcpy(corrupt, frame, n);
			corrupt[byte] ^= (uint8_t)(1u << bit);

			gov_decoder_init(&dec, cap_cb, &cap);
			feed(&dec, corrupt, n);

			/* Universal: a single-bit flip never yields a delivery. */
			GOV_CHECK_EQ(cap.count, 0);

			/* CRC-covered, framing-length-preserving fields: the CRC
			 * is the detector. (VER and LEN have their own paths,
			 * documented above; no-delivery already covers them.) */
			if ((byte == 2u) || (byte == 3u) || (byte >= GOV_FRAME_HDR)) {
				GOV_CHECK_EQ(dec.stats.crc_err, 1);
			}
		}
	}
}

static void test_resync_after_garbage(void)
{
	uint8_t frame[GOV_FRAME_MAX];
	uint8_t stream[128];
	const uint8_t payload[] = { 0xA1, 0xB2, 0xC3 };
	struct capture cap = { 0 };
	gov_decoder_t dec;
	size_t n;
	size_t g;

	n = gov_frame_encode(GOV_TYPE_CMD, 0x7Du, payload, sizeof(payload), frame,
			     sizeof(frame));
	GOV_CHECK(n != 0u);

	/* Garbage prefix deliberately laced with stray 0x7E to exercise false
	 * SOF handling before the real frame arrives. */
	g = 0u;
	stream[g++] = 0x00;
	stream[g++] = 0x7E; /* false SOF */
	stream[g++] = 0xFF; /* bad VER */
	stream[g++] = 0x7E; /* another false SOF */
	stream[g++] = 0x13;
	stream[g++] = 0x37;
	memcpy(&stream[g], frame, n);
	g += n;

	gov_decoder_init(&dec, cap_cb, &cap);
	feed(&dec, stream, g);

	GOV_CHECK_EQ(cap.count, 1);
	GOV_CHECK_EQ(cap.type, GOV_TYPE_CMD);
	GOV_CHECK_EQ(cap.seq, 0x7D);
	GOV_CHECK_EQ(cap.len, sizeof(payload));
	GOV_CHECK(memcmp(cap.payload, payload, sizeof(payload)) == 0);

	/* Back-to-back SOF: a second 0x7E arriving where VER is expected must
	 * re-anchor the frame (not be consumed as a version byte), so the frame
	 * that follows still decodes. */
	{
		uint8_t stream2[1u + GOV_FRAME_MAX];
		struct capture cap2 = { 0 };
		gov_decoder_t dec2;

		stream2[0] = (uint8_t)GOV_SOF; /* first SOF, then real frame */
		memcpy(&stream2[1], frame, n);
		gov_decoder_init(&dec2, cap_cb, &cap2);
		feed(&dec2, stream2, 1u + n);
		GOV_CHECK_EQ(cap2.count, 1);
		GOV_CHECK_EQ(cap2.seq, 0x7D);
	}
}

static void test_concatenation(void)
{
	uint8_t fa[GOV_FRAME_MAX];
	uint8_t fb[GOV_FRAME_MAX];
	uint8_t stream[2u * GOV_FRAME_MAX];
	const uint8_t pa[] = { 1, 2, 3, 4 };
	const uint8_t pb[] = { 9, 8, 7 };
	struct capture cap = { 0 };
	gov_decoder_t dec;
	size_t na;
	size_t nb;

	na = gov_frame_encode(GOV_TYPE_DATA, 10u, pa, sizeof(pa), fa, sizeof(fa));
	nb = gov_frame_encode(GOV_TYPE_HEARTBEAT, 11u, pb, sizeof(pb), fb,
			      sizeof(fb));
	GOV_CHECK(na != 0u);
	GOV_CHECK(nb != 0u);

	memcpy(stream, fa, na);
	memcpy(&stream[na], fb, nb);

	gov_decoder_init(&dec, cap_cb, &cap);

	/* Feed A then assert A; feed B then assert B delivered next (order). */
	feed(&dec, stream, na);
	GOV_CHECK_EQ(cap.count, 1);
	GOV_CHECK_EQ(cap.type, GOV_TYPE_DATA);
	GOV_CHECK_EQ(cap.seq, 10);
	GOV_CHECK(cap.len == sizeof(pa) && memcmp(cap.payload, pa, sizeof(pa)) == 0);

	feed(&dec, &stream[na], nb);
	GOV_CHECK_EQ(cap.count, 2);
	GOV_CHECK_EQ(cap.type, GOV_TYPE_HEARTBEAT);
	GOV_CHECK_EQ(cap.seq, 11);
	GOV_CHECK(cap.len == sizeof(pb) && memcmp(cap.payload, pb, sizeof(pb)) == 0);
}

static void test_unknown_type_counted(void)
{
	uint8_t frame[GOV_FRAME_MAX];
	struct capture cap = { 0 };
	gov_decoder_t dec;
	size_t n;

	/* 0x77 is not a defined TYPE (§3); encoder does not police TYPE, so a
	 * CRC-valid unknown-type frame is buildable. */
	n = gov_frame_encode(0x77u, 0x01u, NULL, 0u, frame, sizeof(frame));
	GOV_CHECK(n != 0u);

	gov_decoder_init(&dec, cap_cb, &cap);
	feed(&dec, frame, n);

	GOV_CHECK_EQ(cap.count, 0);
	GOV_CHECK_EQ(dec.stats.unknown_type, 1);
	GOV_CHECK_EQ(dec.stats.crc_err, 0);
}

static void test_len_over_max_rejected(void)
{
	uint8_t frame[GOV_FRAME_MAX + 8u];
	struct capture cap = { 0 };
	gov_decoder_t dec;
	size_t g;

	/* Hand-built header claiming LEN=65 (> GOV_MAX_PAYLOAD). Must be rejected
	 * at header time (len_err) with no payload byte buffered — ASan/UBSan
	 * proves no overread of the 64-byte static buffer. */
	g = 0u;
	frame[g++] = (uint8_t)GOV_SOF;
	frame[g++] = (uint8_t)GOV_VER;
	frame[g++] = (uint8_t)GOV_TYPE_DATA;
	frame[g++] = 0x00u;
	frame[g++] = 0x00u; /* LEN high */
	frame[g++] = 0x41u; /* LEN low = 65 */
	for (unsigned i = 0u; i < 70u; i++) {
		frame[g++] = (uint8_t)(0xE0u + i);
	}

	gov_decoder_init(&dec, cap_cb, &cap);
	feed(&dec, frame, g);

	GOV_CHECK_EQ(cap.count, 0);
	GOV_CHECK_EQ(dec.stats.len_err, 1);

	/* Link recovers: a valid frame after the rejected one still decodes. */
	{
		uint8_t good[GOV_FRAME_MAX];
		const uint8_t p[] = { 0x55, 0x66 };
		size_t n = gov_frame_encode(GOV_TYPE_DATA, 5u, p, sizeof(p), good,
					    sizeof(good));
		GOV_CHECK(n != 0u);
		feed(&dec, good, n);
		GOV_CHECK_EQ(cap.count, 1);
		GOV_CHECK_EQ(cap.seq, 5);
	}
}

static void test_encode_error_paths(void)
{
	uint8_t out[GOV_FRAME_MAX];
	uint8_t payload[4] = { 1, 2, 3, 4 };

	GOV_CHECK_EQ(gov_frame_encode(GOV_TYPE_DATA, 0u, payload,
				      (uint16_t)(GOV_MAX_PAYLOAD + 1u), out,
				      sizeof(out)),
		     0);
	GOV_CHECK_EQ(gov_frame_encode(GOV_TYPE_DATA, 0u, NULL, 4u, out, sizeof(out)),
		     0);
	GOV_CHECK_EQ(gov_frame_encode(GOV_TYPE_DATA, 0u, payload, 4u, out, 5u), 0);
	GOV_CHECK_EQ(gov_frame_encode(GOV_TYPE_DATA, 0u, payload, 4u, NULL,
				      sizeof(out)),
		     0);
	GOV_CHECK(gov_frame_encode(GOV_TYPE_ACK, 0u, NULL, 0u, out, sizeof(out)) ==
		  (size_t)(GOV_FRAME_HDR + GOV_FRAME_CRC));
}

static void test_no_delivery_on_pure_garbage(void)
{
	struct capture cap = { 0 };
	gov_decoder_t dec;
	uint8_t stream[256];

	for (size_t i = 0u; i < sizeof(stream); i++) {
		stream[i] = rng_byte();
	}
	gov_decoder_init(&dec, cap_cb, &cap);
	feed(&dec, stream, sizeof(stream));
	/* Property: never crashes; deliveries only from genuinely valid frames
	 * (a random 72+ byte-aligned valid frame is astronomically unlikely). */
	GOV_CHECK(cap.count <= 1);
}

/* ============================ reliability helpers =========================
 * The reliable API shares one void* ctx between the wire and deliver
 * callbacks, so a single combined context struct backs both. Wire emissions
 * are decoded in-line so assertions run against real on-wire frames (exercising
 * the encode+decode path end to end).
 */
struct link_ctx {
	/* wire side */
	int frames;
	uint8_t dec_type;
	uint8_t dec_seq;
	uint8_t dec_payload0;
	uint16_t dec_len;
	/* app-delivery side */
	int delivered;
	uint8_t last_seq;
	uint8_t last_payload0;
};

static void wl_dec_cb(uint8_t type, uint8_t seq, const uint8_t *payload,
		      uint16_t len, void *ctx)
{
	struct link_ctx *c = (struct link_ctx *)ctx;

	c->dec_type = type;
	c->dec_seq = seq;
	c->dec_len = len;
	c->dec_payload0 = (len > 0u) ? payload[0] : 0u;
}

static void wire_cb(const uint8_t *frame, size_t len, void *ctx)
{
	struct link_ctx *c = (struct link_ctx *)ctx;
	gov_decoder_t dec;

	c->frames++;
	gov_decoder_init(&dec, wl_dec_cb, c);
	for (size_t i = 0u; i < len; i++) {
		gov_decoder_push(&dec, frame[i]);
	}
}

static void app_cb(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
		   void *ctx)
{
	struct link_ctx *c = (struct link_ctx *)ctx;

	(void)type;
	c->delivered++;
	c->last_seq = seq;
	c->last_payload0 = (len > 0u) ? payload[0] : 0u;
}

/* ============================ reliability tests =========================== */

static void test_tx_seq_assignment_and_ack(void)
{
	struct link_ctx c = { 0 };
	gov_tx_t tx;
	const uint8_t p1[] = { 0xAA };
	const uint8_t p2[] = { 0xBB };

	gov_tx_init(&tx, wire_cb, &c);

	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p1, 1u, 0u), GOV_TX_OK);
	GOV_CHECK_EQ(c.frames, 1);
	GOV_CHECK_EQ(c.dec_type, GOV_TYPE_DATA);
	GOV_CHECK_EQ(c.dec_seq, 0);
	GOV_CHECK_EQ(c.dec_payload0, 0xAA);

	/* Window=1: refuse a second send while awaiting ACK. */
	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p2, 1u, 1u), GOV_TX_BUSY);
	GOV_CHECK_EQ(c.frames, 1);

	gov_tx_on_ack(&tx, 0u);
	GOV_CHECK_EQ(gov_tx_poll(&tx, 5u), GOV_TX_IDLE);

	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p2, 1u, 10u), GOV_TX_OK);
	GOV_CHECK_EQ(c.dec_seq, 1);
}

static void test_tx_wrong_ack_ignored(void)
{
	struct link_ctx c = { 0 };
	gov_tx_t tx;
	const uint8_t p[] = { 0x01 };

	gov_tx_init(&tx, wire_cb, &c);
	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_CMD, p, 1u, 0u), GOV_TX_OK);
	gov_tx_on_ack(&tx, 99u); /* not the outstanding SEQ */
	GOV_CHECK_EQ(gov_tx_poll(&tx, 1u), GOV_TX_WAIT_ACK);
}

static void test_tx_retransmit_then_link_fault(void)
{
	struct link_ctx c = { 0 };
	gov_tx_t tx;
	const uint8_t p[] = { 0x42 };
	uint32_t t = 0u;

	gov_tx_init(&tx, wire_cb, &c);
	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p, 1u, t), GOV_TX_OK);
	GOV_CHECK_EQ(c.frames, 1);

	GOV_CHECK_EQ(gov_tx_poll(&tx, t + GOV_ACK_TIMEOUT_MS - 1u),
		     GOV_TX_WAIT_ACK);
	GOV_CHECK_EQ(c.frames, 1);

	for (uint32_t r = 1u; r <= GOV_MAX_RETRIES; r++) {
		t += GOV_ACK_TIMEOUT_MS;
		GOV_CHECK_EQ(gov_tx_poll(&tx, t), GOV_TX_WAIT_ACK);
		GOV_CHECK_EQ(c.frames, (int)(1u + r));
		GOV_CHECK_EQ(c.dec_seq, 0); /* identical SEQ retransmit */
	}

	t += GOV_ACK_TIMEOUT_MS;
	GOV_CHECK_EQ(gov_tx_poll(&tx, t), GOV_TX_LINK_FAULT);
	GOV_CHECK_EQ(c.frames, (int)(1u + GOV_MAX_RETRIES));

	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p, 1u, t), GOV_TX_BUSY);
	gov_tx_clear_fault(&tx);
	GOV_CHECK_EQ(gov_tx_poll(&tx, t), GOV_TX_IDLE);
}

static void test_tx_ack_before_timeout_no_retransmit(void)
{
	struct link_ctx c = { 0 };
	gov_tx_t tx;
	const uint8_t p[] = { 0x7 };

	gov_tx_init(&tx, wire_cb, &c);
	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p, 1u, 100u), GOV_TX_OK);
	gov_tx_on_ack(&tx, 0u);
	GOV_CHECK_EQ(gov_tx_poll(&tx, 100u + 10u * GOV_ACK_TIMEOUT_MS),
		     GOV_TX_IDLE);
	GOV_CHECK_EQ(c.frames, 1);
}

static void test_tx_seq_wraps(void)
{
	struct link_ctx c = { 0 };
	gov_tx_t tx;
	const uint8_t p[] = { 0x1 };

	gov_tx_init(&tx, wire_cb, &c);
	for (uint32_t i = 0u; i < 257u; i++) {
		GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p, 1u, i), GOV_TX_OK);
		GOV_CHECK_EQ(c.dec_seq, (uint8_t)(i & 0xFFu));
		gov_tx_on_ack(&tx, (uint8_t)(i & 0xFFu));
	}
}

static void test_rx_dedup_reacks_once(void)
{
	struct link_ctx c = { 0 };
	gov_rx_t rx;
	const uint8_t p[] = { 0xC0 };

	gov_rx_init(&rx, wire_cb, app_cb, &c);

	gov_rx_on_frame(&rx, GOV_TYPE_DATA, 7u, p, 1u);
	GOV_CHECK_EQ(c.delivered, 1);
	GOV_CHECK_EQ(c.last_seq, 7);
	GOV_CHECK_EQ(c.frames, 1);
	GOV_CHECK_EQ(c.dec_type, GOV_TYPE_ACK);
	GOV_CHECK_EQ(c.dec_payload0, 7);
	GOV_CHECK_EQ(rx.dup, 0);

	/* Duplicate SEQ: re-ACK, no re-delivery. */
	gov_rx_on_frame(&rx, GOV_TYPE_DATA, 7u, p, 1u);
	GOV_CHECK_EQ(c.delivered, 1);
	GOV_CHECK_EQ(rx.dup, 1);
	GOV_CHECK_EQ(c.frames, 2);
	GOV_CHECK_EQ(c.dec_payload0, 7);

	/* New SEQ delivered again. */
	gov_rx_on_frame(&rx, GOV_TYPE_DATA, 8u, p, 1u);
	GOV_CHECK_EQ(c.delivered, 2);
	GOV_CHECK_EQ(c.last_seq, 8);
	GOV_CHECK_EQ(rx.dup, 1);
}

static void test_rx_first_frame_any_seq(void)
{
	struct link_ctx c = { 0 };
	gov_rx_t rx;
	const uint8_t p[] = { 0x5 };

	gov_rx_init(&rx, wire_cb, app_cb, &c);
	gov_rx_on_frame(&rx, GOV_TYPE_CMD, 200u, p, 1u);
	GOV_CHECK_EQ(c.delivered, 1);
	GOV_CHECK_EQ(c.last_seq, 200);
	GOV_CHECK_EQ(rx.dup, 0);
}

static void test_tx_encode_error(void)
{
	struct link_ctx c = { 0 };
	gov_tx_t tx;
	uint8_t big[GOV_MAX_PAYLOAD + 1u] = { 0 };

	/* An over-length payload fails encoding: nothing emitted, slot stays
	 * IDLE (the sender is not wedged by a caller error). */
	gov_tx_init(&tx, wire_cb, &c);
	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, big, (uint16_t)sizeof(big), 0u),
		     GOV_TX_ENCODE_ERR);
	GOV_CHECK_EQ(c.frames, 0);
	GOV_CHECK_EQ(gov_tx_poll(&tx, 0u), GOV_TX_IDLE);
}

static void test_rx_deliver_null_ok(void)
{
	struct link_ctx c = { 0 };
	gov_rx_t rx;
	const uint8_t p[] = { 0x9 };

	/* NULL deliver callback: frame is still ACKed + deduped, just not
	 * delivered (a validate-only receiver). */
	gov_rx_init(&rx, wire_cb, NULL, &c);
	gov_rx_on_frame(&rx, GOV_TYPE_DATA, 3u, p, 1u);
	GOV_CHECK_EQ(c.delivered, 0);
	GOV_CHECK_EQ(c.frames, 1); /* ACK still emitted */
	GOV_CHECK_EQ(c.dec_type, GOV_TYPE_ACK);
}

static void test_null_guards(void)
{
	gov_tx_t tx;
	gov_rx_t rx;
	const uint8_t p[] = { 0x1 };

	/* Defensive NULL guards (RULES R6): must not crash, and report a safe
	 * status where one is returned. ASan/UBSan proves no bad access. */
	gov_tx_init(NULL, wire_cb, NULL);
	gov_rx_init(NULL, wire_cb, app_cb, NULL);
	GOV_CHECK_EQ(gov_tx_send(NULL, GOV_TYPE_DATA, p, 1u, 0u), GOV_TX_BUSY);
	GOV_CHECK_EQ(gov_tx_poll(NULL, 0u), GOV_TX_LINK_FAULT);
	gov_tx_on_ack(NULL, 0u);
	gov_tx_clear_fault(NULL);
	gov_rx_on_frame(NULL, GOV_TYPE_DATA, 0u, p, 1u);
	gov_decoder_init(NULL, NULL, NULL);
	gov_decoder_push(NULL, 0x00u);

	/* A sender whose wire is NULL refuses to send (no output path). */
	gov_tx_init(&tx, NULL, NULL);
	GOV_CHECK_EQ(gov_tx_send(&tx, GOV_TYPE_DATA, p, 1u, 0u), GOV_TX_BUSY);

	/* A receiver whose wire is NULL is a no-op on a frame (cannot ACK). */
	gov_rx_init(&rx, NULL, app_cb, NULL);
	gov_rx_on_frame(&rx, GOV_TYPE_DATA, 0u, p, 1u);
	GOV_CHECK_EQ(rx.dup, 0);
}

int main(void)
{
	printf("== proto host tests ==\n");

	/* crc16 */
	GOV_RUN(test_crc_check_value);
	GOV_RUN(test_crc_empty_is_seed);
	GOV_RUN(test_crc_single_byte);
	GOV_RUN(test_crc_incremental_equals_bulk);

	/* framing */
	GOV_RUN(test_roundtrip_many);
	GOV_RUN(test_single_bit_flip_rejected);
	GOV_RUN(test_resync_after_garbage);
	GOV_RUN(test_concatenation);
	GOV_RUN(test_unknown_type_counted);
	GOV_RUN(test_len_over_max_rejected);
	GOV_RUN(test_encode_error_paths);
	GOV_RUN(test_no_delivery_on_pure_garbage);

	/* reliability */
	GOV_RUN(test_tx_seq_assignment_and_ack);
	GOV_RUN(test_tx_wrong_ack_ignored);
	GOV_RUN(test_tx_retransmit_then_link_fault);
	GOV_RUN(test_tx_ack_before_timeout_no_retransmit);
	GOV_RUN(test_tx_seq_wraps);
	GOV_RUN(test_rx_dedup_reacks_once);
	GOV_RUN(test_rx_first_frame_any_seq);
	GOV_RUN(test_tx_encode_error);
	GOV_RUN(test_rx_deliver_null_ok);
	GOV_RUN(test_null_guards);

	return GOV_TEST_SUMMARY();
}
