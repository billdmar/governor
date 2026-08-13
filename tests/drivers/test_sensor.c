/*
 * test_sensor.c — host unit tests for the sensor driver logic (drivers/sensor.c)
 * driven by the scriptable fake bus (fake_bus.c). Covers the nominal path and
 * every driver fault-matrix row F01-F05 (config/registry.md), including
 * recovery, using the gov_test.h harness.
 */
#include "gov_test.h"

#include "sensor.h"
#include "fake_bus.h"

/* Shared test config: small windows so scenarios are short and deterministic.
 * read_len=2 → 16-bit big-endian sample. Range [0,1000]. */
static struct gov_sensor_cfg make_cfg(struct gov_fake_bus *fb)
{
	struct gov_sensor_cfg cfg = {
		.bus = &fb->bus,
		.addr = 0x48,
		.reg = 0x00,
		.read_len = 2,
		.range_min = 0,
		.range_max = 1000,
		.dropout_limit = 3, /* F01: 3 consecutive NODATA polls */
		.stuck_limit = 3,   /* F02: 3 consecutive identical samples */
		.nak_retries = 3,   /* F04: GOV_MAX_RETRIES-style retry budget */
	};
	return cfg;
}

/* ---- nominal: good samples flow through, healthy, slot published ---- */
static void test_nominal(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;
	struct gov_sample_slot slot;
	int32_t out = 0;

	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);
	gov_slot_init(&slot);

	gov_fake_push(&fb, GOV_BUS_OK, 100);
	gov_fake_push(&fb, GOV_BUS_OK, 200);
	gov_fake_push(&fb, GOV_BUS_OK, 300);

	GOV_CHECK_EQ(gov_sensor_poll(&s, &slot), GOV_SENSOR_HEALTHY);
	GOV_CHECK_EQ(gov_sensor_value(&s), 100);
	GOV_CHECK(gov_slot_get(&slot, &out));
	GOV_CHECK_EQ(out, 100);
	/* freshness cleared after get */
	GOV_CHECK(!gov_slot_get(&slot, &out));

	gov_sensor_poll(&s, &slot);
	gov_sensor_poll(&s, &slot);
	GOV_CHECK_EQ(gov_sensor_health(&s), GOV_SENSOR_HEALTHY);
	GOV_CHECK_EQ(gov_sensor_faults(&s), (uint32_t)GOV_SFLAG_NONE);
	GOV_CHECK_EQ(gov_sensor_value(&s), 300);
	GOV_CHECK(gov_slot_get(&slot, &out));
	GOV_CHECK_EQ(out, 300); /* newest-wins slot */
}

/* ---- F01 dropout: no data-ready for N periods; resumes healthy ---- */
static void test_f01_dropout(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;
	int32_t out = 0;
	struct gov_sample_slot slot;

	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);
	gov_slot_init(&slot);

	/* one good sample, then a run of NODATA */
	gov_fake_push(&fb, GOV_BUS_OK, 500);
	gov_sensor_poll(&s, &slot);
	GOV_CHECK_EQ(gov_sensor_health(&s), GOV_SENSOR_HEALTHY);
	GOV_CHECK(gov_slot_get(&slot, &out)); /* drain the good sample */
	GOV_CHECK_EQ(out, 500);

	gov_fake_set(&fb, GOV_BUS_NODATA, 0);
	/* below the limit → not yet flagged */
	gov_sensor_poll(&s, &slot);
	gov_sensor_poll(&s, &slot);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_DROPOUT));
	/* reaching dropout_limit consecutive NODATA → flagged */
	gov_sensor_poll(&s, &slot);
	GOV_CHECK(gov_sensor_faults(&s) & GOV_SFLAG_DROPOUT);
	GOV_CHECK_EQ(gov_sensor_health(&s), GOV_SENSOR_FAULTED);
	/* last-good held through dropout, nothing new published */
	GOV_CHECK_EQ(gov_sensor_value(&s), 500);
	GOV_CHECK(!gov_slot_get(&slot, &out));

	/* data returns → dropout clears, healthy again, new value flows */
	gov_fake_set(&fb, GOV_BUS_OK, 600);
	GOV_CHECK_EQ(gov_sensor_poll(&s, &slot), GOV_SENSOR_HEALTHY);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_DROPOUT));
	GOV_CHECK(gov_slot_get(&slot, &out));
	GOV_CHECK_EQ(out, 600);
}

/* ---- F02 stuck: identical value past the window; clears on variance ---- */
static void test_f02_stuck(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;

	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);

	/* Feed many identical in-range samples. First sample sets last_good;
	 * each subsequent identical one increments the stuck counter. */
	gov_fake_set(&fb, GOV_BUS_OK, 250);
	gov_sensor_poll(&s, NULL); /* first: establishes last_good, cnt=0 */
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_STUCK));
	gov_sensor_poll(&s, NULL); /* cnt=1 */
	gov_sensor_poll(&s, NULL); /* cnt=2 */
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_STUCK));
	gov_sensor_poll(&s, NULL); /* cnt=3 == stuck_limit → flagged */
	GOV_CHECK(gov_sensor_faults(&s) & GOV_SFLAG_STUCK);
	GOV_CHECK_EQ(gov_sensor_health(&s), GOV_SENSOR_FAULTED);

	/* variance returns → stuck clears, healthy again */
	gov_fake_set(&fb, GOV_BUS_OK, 260);
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_HEALTHY);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_STUCK));
}

/* ---- F03 garbage: out-of-range rejected, last-good held, flagged ---- */
static void test_f03_garbage(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;
	int32_t out = 0;
	struct gov_sample_slot slot;

	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);
	gov_slot_init(&slot);

	gov_fake_push(&fb, GOV_BUS_OK, 400); /* good */
	gov_sensor_poll(&s, &slot);
	GOV_CHECK(gov_slot_get(&slot, &out));
	GOV_CHECK_EQ(out, 400);

	/* out-of-range high (range_max=1000; read_len=2 caps raw at 0xFFFF) */
	gov_fake_set(&fb, GOV_BUS_OK, 5000);
	GOV_CHECK_EQ(gov_sensor_poll(&s, &slot), GOV_SENSOR_FAULTED);
	GOV_CHECK(gov_sensor_faults(&s) & GOV_SFLAG_RANGE);
	/* rejected: last-good held, nothing published */
	GOV_CHECK_EQ(gov_sensor_value(&s), 400);
	GOV_CHECK(!gov_slot_get(&slot, &out));

	/* in-range sample returns → range fault clears */
	gov_fake_set(&fb, GOV_BUS_OK, 450);
	GOV_CHECK_EQ(gov_sensor_poll(&s, &slot), GOV_SENSOR_HEALTHY);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_RANGE));
	GOV_CHECK_EQ(gov_sensor_value(&s), 450);
}

/* ---- F04 I2C NAK: retry policy engages; persistent NAK → flagged ---- */
static void test_f04_nak(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;

	/* (a) transient NAK then ACK within the retry budget → healthy, no flag */
	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);
	gov_fake_push(&fb, GOV_BUS_NAK, 0);
	gov_fake_push(&fb, GOV_BUS_NAK, 0);
	gov_fake_push(&fb, GOV_BUS_OK, 700); /* recovers on 3rd attempt */
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_HEALTHY);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_BUS_NAK));
	GOV_CHECK_EQ(gov_sensor_value(&s), 700);
	/* retry policy actually engaged: 3 reads within one poll */
	GOV_CHECK_EQ(fb.read_calls, 3u);

	/* (b) persistent NAK → flagged after 1 + nak_retries attempts */
	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);
	gov_fake_set(&fb, GOV_BUS_NAK, 0);
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_FAULTED);
	GOV_CHECK(gov_sensor_faults(&s) & GOV_SFLAG_BUS_NAK);
	GOV_CHECK_EQ(fb.read_calls, 1u + 3u); /* initial + nak_retries */

	/* recover on ACK */
	gov_fake_set(&fb, GOV_BUS_OK, 720);
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_HEALTHY);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_BUS_NAK));
}

/* ---- F05 bus error: flagged clearly; not retried; last-good held ---- */
static void test_f05_bus_error(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;

	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	gov_sensor_init(&s, &cfg);

	gov_fake_push(&fb, GOV_BUS_OK, 800); /* establish last-good */
	gov_sensor_poll(&s, NULL);

	gov_fake_set(&fb, GOV_BUS_ERR, 0); /* SDA stuck low */
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_FAULTED);
	GOV_CHECK(gov_sensor_faults(&s) & GOV_SFLAG_BUS_ERR);
	/* bus error is not retryable: exactly one read attempt */
	GOV_CHECK_EQ(fb.read_calls, 2u); /* the good poll + this one */
	GOV_CHECK_EQ(gov_sensor_value(&s), 800); /* last-good held */

	/* clears when the bus recovers */
	gov_fake_set(&fb, GOV_BUS_OK, 810);
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_HEALTHY);
	GOV_CHECK(!(gov_sensor_faults(&s) & GOV_SFLAG_BUS_ERR));
}

/* ---- slot: newest-wins + freshness semantics in isolation ---- */
static void test_slot(void)
{
	struct gov_sample_slot slot;
	int32_t out = 0;

	gov_slot_init(&slot);
	GOV_CHECK(!gov_slot_get(&slot, &out)); /* empty */

	gov_slot_push(&slot, 11);
	gov_slot_push(&slot, 22); /* newest wins, no queue */
	GOV_CHECK(gov_slot_get(&slot, &out));
	GOV_CHECK_EQ(out, 22);
	GOV_CHECK(!gov_slot_get(&slot, &out)); /* consumed */
}

/* ---- init clamps read_len into the static buffer bounds (RULES.md R8) ---- */
static void test_init_clamps_read_len(void)
{
	struct gov_fake_bus fb;
	struct gov_sensor s;
	struct gov_sensor_cfg cfg;

	gov_fake_bus_init(&fb);
	cfg = make_cfg(&fb);
	cfg.read_len = 99; /* absurd; must clamp to GOV_SENSOR_MAX_READ */
	gov_sensor_init(&s, &cfg);
	gov_fake_set(&fb, GOV_BUS_OK, 123);
	/* Must not overrun the internal buffer; ASan would catch it. */
	GOV_CHECK_EQ(gov_sensor_poll(&s, NULL), GOV_SENSOR_HEALTHY);
}

int main(void)
{
	GOV_RUN(test_nominal);
	GOV_RUN(test_f01_dropout);
	GOV_RUN(test_f02_stuck);
	GOV_RUN(test_f03_garbage);
	GOV_RUN(test_f04_nak);
	GOV_RUN(test_f05_bus_error);
	GOV_RUN(test_slot);
	GOV_RUN(test_init_clamps_read_len);
	return GOV_TEST_SUMMARY();
}
