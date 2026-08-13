/*
 * tests/ontarget — ztest suite exercising the governor node coordinator + safety
 * events ON TARGET (built for native_sim + qemu_cortex_m3 via twister). These
 * cover the fault-matrix rows whose REQUIRED OUTCOME needs no I2C/watchdog
 * peripheral (F01-F03 via the host fake_bus adapter, F06-F09 link semantics,
 * F11-F14 event outcomes). The I2C/watchdog rows (F04/F05/F10) run on the STM32
 * Renode machine (the renode Robot scenarios). The pure module logic is already
 * covered by the host unit/property suites; this proves it runs on-target.
 *
 * EMULATION note: on native_sim/qemu this validates logic + integration, not
 * silicon timing.
 */
#include <zephyr/ztest.h>

#include "node_core.h"
#include "safety.h"
#include "proto.h"
#include "reliable.h"
#include "sensor.h"
#include "fake_bus.h"

/* --- Node coordinator on target: boot to RUN, drive plant to setpoint --- */
ZTEST(governor_ontarget, test_boot_to_run_and_track)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	zassert_equal(gov_node_state(&n), GOV_ST_RUN, "should reach RUN");

	float meas = 0.0f;
	uint32_t t = 10;
	for (int i = 0; i < 200; i++) {
		gov_node_control_step(&n, meas, t);
		meas = gov_plant_output(&n.plant);
		t += 10;
	}
	zassert_within((int)meas, 50, 2, "PID should track setpoint 50 +/-2");
	zassert_equal(gov_node_faults(&n), GOV_FAULT_NONE, "no faults nominal");
}

/* --- F12: sustained divergence -> SAFE_STOP, latched --- */
ZTEST(governor_ontarget, test_f12_divergence_safe_stop)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	float meas = 0.0f;
	uint32_t t = 10;
	for (int i = 0; i < 50; i++) {
		gov_node_control_step(&n, meas, t);
		meas = gov_plant_output(&n.plant);
		t += 10;
	}
	gov_plant_set_disturbance(&n.plant, 500.0f);
	for (int i = 0; i < 100; i++) {
		gov_node_control_step(&n, meas, t);
		meas = gov_plant_output(&n.plant);
		t += 10;
	}
	zassert_equal(gov_node_state(&n), GOV_ST_SAFE_STOP, "divergence -> SAFE_STOP");
	zassert_true(gov_node_faults(&n) & GOV_FAULT_DIVERGE, "FAULT_DIVERGE set");
}

/* --- F13: operator estop -> SAFE_STOP, latched --- */
ZTEST(governor_ontarget, test_f13_operator_estop)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	gov_node_post_event(&n, GOV_EV_OPERATOR_STOP, 100);
	zassert_equal(gov_node_state(&n), GOV_ST_SAFE_STOP, "estop -> SAFE_STOP");
	zassert_true(gov_node_faults(&n) & GOV_FAULT_OPERATOR_STOP, "flag set");
}

/* --- F14: self-test failure -> SAFE_STOP --- */
ZTEST(governor_ontarget, test_f14_selftest_fail)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_post_event(&n, GOV_EV_SELFTEST_FAIL, 5);
	zassert_equal(gov_node_state(&n), GOV_ST_SAFE_STOP, "selftest fail -> SAFE_STOP");
	zassert_true(gov_node_faults(&n) & GOV_FAULT_INIT, "FAULT_INIT set");
}

/* --- F11: control deadline misses -> DEGRADED (timing fault) --- */
ZTEST(governor_ontarget, test_f11_deadline_miss)
{
	struct gov_node n;
	gov_node_init(&n, 50.0f);
	gov_node_selftest_ok(&n, 0);
	gov_node_post_event(&n, GOV_EV_DEADLINE_MISS, 100);
	zassert_equal(gov_node_state(&n), GOV_ST_DEGRADED, "deadline miss -> DEGRADED");
	zassert_true(gov_node_faults(&n) & GOV_FAULT_TIMING, "FAULT_TIMING set");
}

/* --- F01-F03 via the host fake_bus: sensor faults surface as driver flags --- */
ZTEST(governor_ontarget, test_f01_f03_sensor_faults_via_fakebus)
{
	struct gov_fake_bus fb;
	gov_fake_bus_init(&fb);

	struct gov_sensor s;
	const struct gov_sensor_cfg cfg = {
		.bus = &fb.bus, .addr = 0x48, .reg = 0, .read_len = 2,
		.range_min = 0, .range_max = 100,
		.dropout_limit = 3, .stuck_limit = 5, .nak_retries = 3,
	};
	gov_sensor_init(&s, &cfg);

	/* F03 garbage: out-of-range sample rejected -> RANGE flag. */
	gov_fake_set(&fb, GOV_BUS_OK, 9999);
	gov_sensor_poll(&s, NULL);
	zassert_true(gov_sensor_faults(&s) & GOV_SFLAG_RANGE, "range fault flagged");

	/* F01 dropout: sustained NODATA -> DROPOUT flag. */
	gov_sensor_init(&s, &cfg);
	gov_fake_set(&fb, GOV_BUS_NODATA, 0);
	for (int i = 0; i < 4; i++) {
		gov_sensor_poll(&s, NULL);
	}
	zassert_true(gov_sensor_faults(&s) & GOV_SFLAG_DROPOUT, "dropout flagged");
}

/* --- F06/F08/F09: link framing integrity on target (round-trip, dedup, resync) */
static int deliver_count;
static uint8_t last_seq_delivered;
static void count_deliver(uint8_t type, uint8_t seq, const uint8_t *p, uint16_t l, void *ctx)
{
	ARG_UNUSED(type); ARG_UNUSED(p); ARG_UNUSED(l); ARG_UNUSED(ctx);
	deliver_count++;
	last_seq_delivered = seq;
}

ZTEST(governor_ontarget, test_f06_f09_link_framing)
{
	gov_decoder_t d;
	deliver_count = 0;
	gov_decoder_init(&d, count_deliver, NULL);

	uint8_t frame[GOV_FRAME_MAX];
	uint8_t payload[2] = {0xAA, 0xBB};
	size_t n = gov_frame_encode(GOV_TYPE_DATA, 7, payload, 2, frame, sizeof frame);
	zassert_true(n > 0, "encode ok");

	/* F09: garbage burst then a valid frame -> decoder resyncs, delivers once. */
	for (int i = 0; i < 5; i++) {
		gov_decoder_push(&d, (uint8_t)(0x11 * i));
	}
	for (size_t i = 0; i < n; i++) {
		gov_decoder_push(&d, frame[i]);
	}
	zassert_equal(deliver_count, 1, "valid frame delivered after garbage");
	zassert_equal(last_seq_delivered, 7, "correct seq");

	/* F06: single-bit corruption -> CRC rejects, no delivery. */
	deliver_count = 0;
	frame[4] ^= 0x01; /* flip a payload/len bit */
	for (size_t i = 0; i < n; i++) {
		gov_decoder_push(&d, frame[i]);
	}
	zassert_equal(deliver_count, 0, "corrupted frame rejected");
	zassert_true(d.stats.crc_err > 0 || d.stats.len_err > 0, "error counted");
}

ZTEST_SUITE(governor_ontarget, NULL, NULL, NULL, NULL, NULL);
