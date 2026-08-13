/*
 * test_telem.c -- host unit tests for lib/telem: record encode/decode, the SPSC
 * ring (overflow-is-counted, FIFO), and the health aggregator (timing-fault and
 * liveness signals). Single translation unit / single main() to match the
 * gov_test.h harness (one set of static counters). Uses tests/host.mk.
 */
#include "gov_test.h"
#include "telem.h"
#include "telem_config.h"
#include "ring.h"
#include "health.h"

/* ============================ record encode/decode ======================== */

static void test_data_roundtrip(void)
{
	struct gov_telem_record in = {
		.tick = 0x01020304u,
		.setpoint = -12345,
		.measurement = 32760,
		.output = -2000000000,
		.state = 2, /* GOV_ST_DEGRADED */
		.fault_flags = GOV_FAULT_LINK | GOV_FAULT_TIMING,
	};
	uint8_t buf[GOV_MAX_PAYLOAD];
	struct gov_telem_record out;
	size_t n;

	n = gov_telem_encode(&in, buf, sizeof(buf));
	GOV_CHECK_EQ(n, GOV_TELEM_DATA_LEN);
	GOV_CHECK(n <= GOV_MAX_PAYLOAD);

	GOV_CHECK(gov_telem_decode(buf, n, &out));
	GOV_CHECK_EQ(out.tick, in.tick);
	GOV_CHECK_EQ(out.setpoint, in.setpoint);
	GOV_CHECK_EQ(out.measurement, in.measurement);
	GOV_CHECK_EQ(out.output, in.output);
	GOV_CHECK_EQ(out.state, in.state);
	GOV_CHECK_EQ(out.fault_flags, in.fault_flags);
}

static void test_sizes_within_payload(void)
{
	GOV_CHECK(GOV_TELEM_DATA_LEN <= GOV_MAX_PAYLOAD);
	GOV_CHECK(GOV_TELEM_HEARTBEAT_LEN <= GOV_MAX_PAYLOAD);
}

static void test_data_big_endian_layout(void)
{
	struct gov_telem_record in = {
		.tick = 0xAABBCCDDu,
		.setpoint = 1,
		.measurement = 0,
		.output = 0,
		.state = 0,
		.fault_flags = 0u,
	};
	uint8_t buf[GOV_MAX_PAYLOAD];

	GOV_CHECK_EQ(gov_telem_encode(&in, buf, sizeof(buf)), GOV_TELEM_DATA_LEN);
	GOV_CHECK_EQ(buf[0], 0xAAu); /* tick MSB-first */
	GOV_CHECK_EQ(buf[1], 0xBBu);
	GOV_CHECK_EQ(buf[2], 0xCCu);
	GOV_CHECK_EQ(buf[3], 0xDDu);
	GOV_CHECK_EQ(buf[7], 0x01u); /* setpoint = 1 -> 00 00 00 01 */
}

/* Every bit position round-trips (encoding is transparent to any u32). */
static void test_every_flag_bit_roundtrips(void)
{
	uint32_t bit;

	for (bit = 0u; bit < 32u; bit++) {
		uint32_t flag = (1u << bit);
		struct gov_telem_record in = {0};
		struct gov_telem_record out = {0};
		uint8_t buf[GOV_MAX_PAYLOAD];

		in.fault_flags = flag;
		GOV_CHECK_EQ(gov_telem_encode(&in, buf, sizeof(buf)), GOV_TELEM_DATA_LEN);
		GOV_CHECK(gov_telem_decode(buf, GOV_TELEM_DATA_LEN, &out));
		GOV_CHECK_EQ(out.fault_flags, flag);
	}
}

static void test_multi_flag_roundtrip(void)
{
	struct gov_telem_record in = {0};
	struct gov_telem_record out = {0};
	uint8_t buf[GOV_MAX_PAYLOAD];
	uint32_t flags = GOV_FAULT_SENSOR_DROP | GOV_FAULT_BUS_ERR | GOV_FAULT_DIVERGE |
			 GOV_FAULT_WATCHDOG | GOV_FAULT_INTERNAL;

	in.fault_flags = flags;
	GOV_CHECK_EQ(gov_telem_encode(&in, buf, sizeof(buf)), GOV_TELEM_DATA_LEN);
	GOV_CHECK(gov_telem_decode(buf, GOV_TELEM_DATA_LEN, &out));
	GOV_CHECK_EQ(out.fault_flags, flags);

	in.fault_flags = GOV_FAULT_ALL;
	GOV_CHECK_EQ(gov_telem_encode(&in, buf, sizeof(buf)), GOV_TELEM_DATA_LEN);
	GOV_CHECK(gov_telem_decode(buf, GOV_TELEM_DATA_LEN, &out));
	GOV_CHECK_EQ(out.fault_flags, GOV_FAULT_ALL);
}

static void test_heartbeat_roundtrip(void)
{
	struct gov_heartbeat in = {
		.state = 3, /* GOV_ST_SAFE_STOP */
		.fault_flags = GOV_FAULT_OPERATOR_STOP | GOV_FAULT_ESCALATED,
		.uptime_count = 0xDEADBEEFu,
	};
	uint8_t buf[GOV_MAX_PAYLOAD];
	struct gov_heartbeat out;
	size_t n;

	n = gov_heartbeat_encode(&in, buf, sizeof(buf));
	GOV_CHECK_EQ(n, GOV_TELEM_HEARTBEAT_LEN);
	GOV_CHECK(gov_heartbeat_decode(buf, n, &out));
	GOV_CHECK_EQ(out.state, in.state);
	GOV_CHECK_EQ(out.fault_flags, in.fault_flags);
	GOV_CHECK_EQ(out.uptime_count, in.uptime_count);
}

static void test_encode_buffer_too_small(void)
{
	struct gov_telem_record rec = {0};
	struct gov_heartbeat hb = {0};
	uint8_t small[4];

	GOV_CHECK_EQ(gov_telem_encode(&rec, small, sizeof(small)), 0u);
	GOV_CHECK_EQ(gov_heartbeat_encode(&hb, small, sizeof(small)), 0u);
}

static void test_decode_rejects_short_and_null(void)
{
	uint8_t buf[GOV_MAX_PAYLOAD] = {0};
	struct gov_telem_record rec;
	struct gov_heartbeat hb;

	GOV_CHECK(!gov_telem_decode(buf, GOV_TELEM_DATA_LEN - 1u, &rec));
	GOV_CHECK(!gov_telem_decode(NULL, GOV_TELEM_DATA_LEN, &rec));
	GOV_CHECK(!gov_telem_decode(buf, GOV_TELEM_DATA_LEN, NULL));
	GOV_CHECK(!gov_heartbeat_decode(buf, GOV_TELEM_HEARTBEAT_LEN - 1u, &hb));
	GOV_CHECK_EQ(gov_telem_encode(NULL, buf, sizeof(buf)), 0u);
}

/* ================================ ring buffer ============================= */

static struct gov_telem_record mk(uint32_t tick)
{
	struct gov_telem_record r = {0};

	r.tick = tick;
	return r;
}

static void test_ring_empty_pop(void)
{
	struct gov_telem_ring r;
	struct gov_telem_record out;

	gov_telem_ring_init(&r);
	GOV_CHECK(gov_telem_ring_empty(&r));
	GOV_CHECK(!gov_telem_ring_full(&r));
	GOV_CHECK_EQ(gov_telem_ring_count(&r), 0u);
	GOV_CHECK(!gov_telem_ring_pop(&r, &out)); /* empty -> false */
	GOV_CHECK_EQ(gov_telem_ring_drops(&r), 0u);
}

/* Fill to capacity, push more -> overflow counted, no overwrite; drain is FIFO. */
static void test_ring_overflow_counted_and_fifo(void)
{
	struct gov_telem_ring r;
	struct gov_telem_record out;
	uint32_t i;

	gov_telem_ring_init(&r);

	/* Fill exactly to capacity (GOV_TELEM_RING_DEPTH usable slots). */
	for (i = 0u; i < GOV_TELEM_RING_DEPTH; i++) {
		struct gov_telem_record rec = mk(i);

		GOV_CHECK(gov_telem_ring_push(&r, &rec));
	}
	GOV_CHECK(gov_telem_ring_full(&r));
	GOV_CHECK_EQ(gov_telem_ring_count(&r), GOV_TELEM_RING_DEPTH);
	GOV_CHECK_EQ(gov_telem_ring_drops(&r), 0u);

	/* Three more pushes must all be rejected AND counted -- never overwrite. */
	for (i = 0u; i < 3u; i++) {
		struct gov_telem_record rec = mk(1000u + i);

		GOV_CHECK(!gov_telem_ring_push(&r, &rec));
	}
	GOV_CHECK_EQ(gov_telem_ring_drops(&r), 3u);
	GOV_CHECK_EQ(gov_telem_ring_count(&r), GOV_TELEM_RING_DEPTH);

	/* Drain: must be the ORIGINAL 0..DEPTH-1 in FIFO order (no overwrite). */
	for (i = 0u; i < GOV_TELEM_RING_DEPTH; i++) {
		GOV_CHECK(gov_telem_ring_pop(&r, &out));
		GOV_CHECK_EQ(out.tick, i);
	}
	GOV_CHECK(gov_telem_ring_empty(&r));
	GOV_CHECK(!gov_telem_ring_pop(&r, &out));
	/* Drops are sticky -- the loss remains reported after draining. */
	GOV_CHECK_EQ(gov_telem_ring_drops(&r), 3u);
}

/* Interleaved push/pop keeps FIFO order and wraps correctly around the buffer. */
static void test_ring_wrap_fifo(void)
{
	struct gov_telem_ring r;
	struct gov_telem_record out;
	uint32_t i;

	gov_telem_ring_init(&r);
	/* Run several times the buffer size through the ring. */
	for (i = 0u; i < GOV_TELEM_RING_SLOTS * 4u; i++) {
		struct gov_telem_record rec = mk(i);

		GOV_CHECK(gov_telem_ring_push(&r, &rec));
		GOV_CHECK(gov_telem_ring_pop(&r, &out));
		GOV_CHECK_EQ(out.tick, i);
	}
	GOV_CHECK(gov_telem_ring_empty(&r));
	GOV_CHECK_EQ(gov_telem_ring_drops(&r), 0u);
}

/* =============================== health =================================== */

static void test_health_timing_fault_flips(void)
{
	struct gov_health h;
	uint32_t i;

	gov_health_init(&h);
	GOV_CHECK(!gov_health_timing_fault(&h));

	/* GOV_MISS_LIMIT-1 misses: not yet a fault. */
	for (i = 0u; i < GOV_MISS_LIMIT - 1u; i++) {
		gov_health_note_deadline(&h, false);
		GOV_CHECK(!gov_health_timing_fault(&h));
	}
	/* The GOV_MISS_LIMIT-th consecutive miss flips it. */
	gov_health_note_deadline(&h, false);
	GOV_CHECK(gov_health_timing_fault(&h));
	GOV_CHECK_EQ(gov_health_consec_misses(&h), GOV_MISS_LIMIT);

	/* A met deadline resets the counter and clears the signal (recovery). */
	gov_health_note_deadline(&h, true);
	GOV_CHECK(!gov_health_timing_fault(&h));
	GOV_CHECK_EQ(gov_health_consec_misses(&h), 0u);
}

static void test_health_liveness(void)
{
	struct gov_health h;
	const uint32_t threshold = 5u;

	gov_health_init(&h);
	/* Nothing seen yet -> not alive. */
	GOV_CHECK(!gov_health_all_alive(&h, 10u, threshold));
	GOV_CHECK(!gov_health_subsys_alive(&h, GOV_HS_CONTROL, 10u, threshold));

	/* All three report alive at tick 10. */
	gov_health_note_alive(&h, GOV_HS_CONTROL, 10u);
	gov_health_note_alive(&h, GOV_HS_LINK, 10u);
	gov_health_note_alive(&h, GOV_HS_SENSOR, 10u);
	GOV_CHECK(gov_health_all_alive(&h, 12u, threshold)); /* age 2 <= 5 */

	/* Advance time so control goes silent past threshold, others refreshed. */
	gov_health_note_alive(&h, GOV_HS_LINK, 16u);
	gov_health_note_alive(&h, GOV_HS_SENSOR, 16u);
	/* now=16: control last_seen=10 -> age 6 > 5 -> not alive. */
	GOV_CHECK(!gov_health_subsys_alive(&h, GOV_HS_CONTROL, 16u, threshold));
	GOV_CHECK(!gov_health_all_alive(&h, 16u, threshold));
	GOV_CHECK(gov_health_subsys_alive(&h, GOV_HS_LINK, 16u, threshold));
}

static void test_health_feed_watchdog(void)
{
	struct gov_health h;
	const uint32_t threshold = 5u;
	uint32_t i;

	gov_health_init(&h);
	/* Not alive yet -> do not feed. */
	GOV_CHECK(!gov_health_feed_watchdog(&h, 0u, threshold));

	gov_health_note_alive(&h, GOV_HS_CONTROL, 0u);
	gov_health_note_alive(&h, GOV_HS_LINK, 0u);
	gov_health_note_alive(&h, GOV_HS_SENSOR, 0u);
	gov_health_note_deadline(&h, true);
	/* All alive + deadlines met -> feed. */
	GOV_CHECK(gov_health_feed_watchdog(&h, 2u, threshold));

	/* Timing fault (deadlines missed) -> withhold the feed even if alive. */
	for (i = 0u; i < GOV_MISS_LIMIT; i++) {
		gov_health_note_deadline(&h, false);
	}
	GOV_CHECK(!gov_health_feed_watchdog(&h, 2u, threshold));
}

/* ================================ driver ================================== */

/* NULL-guard + edge paths (RULES R6: no crash on bad input) to exercise the
 * defensive branches across telem/ring/health. Added by  at  to close the
 * coverage gap on defensive returns (registry §6 ≥90%). */
static void test_telem_null_and_edges(void)
{
	uint8_t buf[GOV_TELEM_DATA_LEN];
	struct gov_telem_record rec = {0};
	/* encode/decode NULL guards */
	GOV_CHECK_EQ(gov_telem_encode(NULL, buf, sizeof buf), 0);
	GOV_CHECK_EQ(gov_telem_encode(&rec, NULL, sizeof buf), 0);
	GOV_CHECK(!gov_telem_decode(NULL, sizeof buf, &rec));
	GOV_CHECK(!gov_telem_decode(buf, sizeof buf, NULL));
	struct gov_heartbeat hb = {0};
	uint8_t hbuf[GOV_TELEM_HEARTBEAT_LEN];
	GOV_CHECK_EQ(gov_heartbeat_encode(NULL, hbuf, sizeof hbuf), 0);
	GOV_CHECK(!gov_heartbeat_decode(NULL, sizeof hbuf, &hb));

	/* ring NULL/empty guards */
	struct gov_telem_ring r;
	gov_telem_ring_init(&r);
	gov_telem_ring_init(NULL);
	GOV_CHECK(!gov_telem_ring_push(NULL, &rec));
	GOV_CHECK(!gov_telem_ring_push(&r, NULL));
	GOV_CHECK(!gov_telem_ring_pop(NULL, &rec));
	GOV_CHECK(gov_telem_ring_empty(&r));
	GOV_CHECK(!gov_telem_ring_full(&r));
	GOV_CHECK_EQ(gov_telem_ring_count(NULL), 0u);
	GOV_CHECK_EQ(gov_telem_ring_drops(NULL), 0u);
	GOV_CHECK(gov_telem_ring_empty(NULL));
	GOV_CHECK(gov_telem_ring_push(&r, &rec)); /* non-empty then */
	GOV_CHECK(!gov_telem_ring_empty(&r));
	GOV_CHECK(!gov_telem_ring_pop(&r, NULL)); /* NULL out rejected */

	/* health NULL guards + never-seen subsystem */
	struct gov_health h;
	gov_health_init(&h);
	gov_health_init(NULL);
	gov_health_note_alive(NULL, GOV_HS_CONTROL, 0);
	gov_health_note_deadline(NULL, true);
	GOV_CHECK(!gov_health_all_alive(NULL, 0, 100));
	GOV_CHECK(!gov_health_all_alive(&h, 100, 10)); /* nothing seen yet */
	GOV_CHECK(!gov_health_subsys_alive(NULL, GOV_HS_LINK, 0, 100));
	GOV_CHECK(!gov_health_timing_fault(NULL));
	GOV_CHECK_EQ(gov_health_consec_misses(NULL), 0u);
	GOV_CHECK(!gov_health_feed_watchdog(NULL, 0, 100));
}

int main(void)
{
	printf("== telem: record encode/decode ==\n");
	GOV_RUN(test_data_roundtrip);
	GOV_RUN(test_sizes_within_payload);
	GOV_RUN(test_data_big_endian_layout);
	GOV_RUN(test_every_flag_bit_roundtrips);
	GOV_RUN(test_multi_flag_roundtrip);
	GOV_RUN(test_heartbeat_roundtrip);
	GOV_RUN(test_encode_buffer_too_small);
	GOV_RUN(test_decode_rejects_short_and_null);

	printf("== telem: ring buffer ==\n");
	GOV_RUN(test_ring_empty_pop);
	GOV_RUN(test_ring_overflow_counted_and_fifo);
	GOV_RUN(test_ring_wrap_fifo);

	printf("== telem: health aggregator ==\n");
	GOV_RUN(test_health_timing_fault_flips);
	GOV_RUN(test_health_liveness);
	GOV_RUN(test_health_feed_watchdog);
	GOV_RUN(test_telem_null_and_edges);

	return GOV_TEST_SUMMARY();
}
