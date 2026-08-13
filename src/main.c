/*
 * governor — application entry point (Zephyr adapter over the host-portable
 * modules).  system integration.
 *
 * All policy lives in the host-portable libraries (lib/{safety,control,telem,
 * proto}, drivers/sensor); this file is the thin RTOS glue that realizes the
 * frozen task architecture (docs/TASKS.md): five threads at fixed priorities on
 * static stacks, fixed-depth queues, a spinlock-guarded sensor slot, an
 * interrupt-driven link UART, and a health-fed hardware watchdog. No dynamic
 * allocation after init.
 *
 * Peripheral-dependent paths (I2C sensor, watchdog, link UART) are compiled in
 * only where the board provides them (CONFIG_I2C / CONFIG_WATCHDOG /
 * DT_HAS link uart) — on qemu_cortex_m3 (TI LM3S6965, no I2C/WDT) the node still
 * boots to RUN and drives the simulated plant, and the emulated-hardware fault
 * rows run on the STM32 (Renode) build.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>

#include "node_core.h"
#include "control_config.h"
#include "reliable.h"
#include "proto.h"
#include "telem.h"
#include "telem_config.h"
#include "ring.h"
#include "health.h"

/* sensor.h is host-portable (no Zephyr): the sample-slot type + sensor logic
 * are always available. The Zephyr I2C binding (hal_zephyr.h) is target-only. */
#include "sensor.h"
#if defined(CONFIG_I2C)
#include "hal_zephyr.h"
#endif
#if defined(CONFIG_WATCHDOG)
#include <zephyr/drivers/watchdog.h>
#endif
#if defined(CONFIG_SETTINGS)
#include "persist.h"
static gov_config_ctx_t config_ctx;
#endif
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

LOG_MODULE_REGISTER(governor, LOG_LEVEL_INF);

/* ---- Frozen task architecture (docs/TASKS.md §2) ---------------------- */
#define CONTROL_PRIO    4
#define LINK_RX_PRIO    5
#define LINK_TX_PRIO    6
#define TELEMETRY_PRIO  7
#define HEALTH_PRIO     8

#define CONTROL_STACK   1024
#define LINK_RX_STACK   1024
#define LINK_TX_STACK   1024
#define TELEMETRY_STACK 1024
#define HEALTH_STACK    768

K_THREAD_STACK_DEFINE(control_stack, CONTROL_STACK);
K_THREAD_STACK_DEFINE(link_rx_stack, LINK_RX_STACK);
K_THREAD_STACK_DEFINE(link_tx_stack, LINK_TX_STACK);
K_THREAD_STACK_DEFINE(telemetry_stack, TELEMETRY_STACK);
K_THREAD_STACK_DEFINE(health_stack, HEALTH_STACK);

static struct k_thread control_thread, link_rx_thread, link_tx_thread,
	telemetry_thread, health_thread;

/* Control-loop cadence: a periodic k_timer gives a semaphore every
 * GOV_CTRL_PERIOD_MS on an ABSOLUTE schedule, so the period does not drift by
 * the loop's own work time (as a k_sleep(period) at the loop tail would). The
 * control thread blocks on the sem; a missed slot is detectable if the sem
 * count grows. Requires 1 kHz kernel tick (prj.conf) for 10 ms = 10 ticks. */
static K_SEM_DEFINE(control_tick_sem, 0, 1);
static void control_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_sem_give(&control_tick_sem);
}
static K_TIMER_DEFINE(control_timer, control_timer_fn, NULL);

/* rx_byteq: raw UART bytes ISR→link_rx (256 B ≈ 3.5× GOV_FRAME_MAX=72, TASKS §2).
 * tx_frameq: framed telemetry link_tx producers→sender (depth 4). */
K_MSGQ_DEFINE(rx_byteq, sizeof(uint8_t), 256, 1);

/* ---- Shared node + link state (static; no allocation) ----------------- */
static struct gov_node node;
static struct gov_rx link_rx;      /* receiver: dedup + auto-ACK */
static gov_tx_t link_tx;           /* sender: stop-and-wait ARQ */
static gov_decoder_t link_decoder; /* byte-at-a-time frame decoder */
static struct gov_telem_ring telem_ring;
static struct gov_health health;

/* Sensor sample slot (sensor→control handoff), spinlock-guarded. */
static struct gov_sample_slot sample_slot;
static struct k_spinlock slot_lock;

static uint32_t now_ms(void) { return k_uptime_get_32(); }

/* The safety SM inside `node` is the single actuation authority, but it is
 * driven from five preemptible threads (control, link_rx, link_tx, telemetry,
 * health) plus the command handler. gov_safety_step() does a non-atomic
 * read-modify-write of the SM state/fault word, so concurrent posts would race.
 * Serialize every access through this mutex. A step is bounded, non-blocking
 * work, so the critical section is short; the mutex uses priority inheritance
 * (Zephyr default) so the 100 Hz control task is never priority-inverted by a
 * lower-priority poster. NOTE: this is concurrency-correct by construction —
 * emulation runs in virtual time and cannot measure real lock contention. */
K_MUTEX_DEFINE(node_lock);

static void locked_post_event(gov_event_t ev, uint32_t t)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	gov_node_post_event(&node, ev, t);
	k_mutex_unlock(&node_lock);
}
static void locked_note_sensor(uint32_t sflags, bool ok, uint32_t t)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	gov_node_note_sensor(&node, sflags, ok, t);
	k_mutex_unlock(&node_lock);
}
static void locked_note_link(bool ok, uint32_t t)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	gov_node_note_link(&node, ok, t);
	k_mutex_unlock(&node_lock);
}
static void locked_note_timing(bool ok, uint32_t t)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	gov_node_note_timing(&node, ok, t);
	k_mutex_unlock(&node_lock);
}
static float locked_control_step(float meas, uint32_t t)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	float applied = gov_node_control_step(&node, meas, t);
	k_mutex_unlock(&node_lock);
	return applied;
}
static void locked_fill_telemetry(struct gov_telem_record *rec)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	gov_node_fill_telemetry(&node, rec);
	k_mutex_unlock(&node_lock);
}
static void locked_selftest_ok(uint32_t t)
{
	k_mutex_lock(&node_lock, K_FOREVER);
	gov_node_selftest_ok(&node, t);
	k_mutex_unlock(&node_lock);
}

/* ---- Link UART -------------------------------------------------------- */
#if DT_NODE_EXISTS(DT_CHOSEN(govtelemetry_link_uart)) && defined(CONFIG_UART_INTERRUPT_DRIVEN)
#include <zephyr/drivers/uart.h>
#define HAVE_LINK_UART 1
static const struct device *const link_uart =
	DEVICE_DT_GET(DT_CHOSEN(govtelemetry_link_uart));

/* ISR: drain RX FIFO into rx_byteq (ISR does the minimum, TASKS §2). */
static void link_uart_isr(const struct device *dev, void *user)
{
	ARG_UNUSED(user);
	uint8_t byte;
	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
		return;
	}
	while (uart_fifo_read(dev, &byte, 1) == 1) {
		/* Non-blocking put from ISR; a full queue is a counted drop
		 * (link health), never a block. */
		(void)k_msgq_put(&rx_byteq, &byte, K_NO_WAIT);
	}
}

/* wire callback: emit framed bytes out the link UART (TX). */
static void link_wire_out(const uint8_t *frame, size_t len, void *ctx)
{
	ARG_UNUSED(ctx);
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(link_uart, frame[i]);
	}
}
#else
#define HAVE_LINK_UART 0
/* No link UART on this board (e.g. native_sim): frames go to the log so the
 * path is still exercised structurally. */
static void link_wire_out(const uint8_t *frame, size_t len, void *ctx)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(ctx);
	LOG_DBG("link tx %u bytes (no uart)", (unsigned)len);
}
#endif

/* Decoder delivery → route ACKs to the TX slot, everything else to the RX
 * dedup layer (which auto-ACKs and delivers de-duplicated frames). */
static void on_decoded_frame(uint8_t type, uint8_t seq, const uint8_t *payload,
			     uint16_t len, void *ctx)
{
	ARG_UNUSED(ctx);
	if (type == GOV_TYPE_ACK && len >= 1) {
		gov_tx_on_ack(&link_tx, payload[0]);
		return;
	}
	gov_rx_on_frame(&link_rx, type, seq, payload, len);
}

/* App-level delivery of a de-duplicated frame (commands from the ground
 * station). CMD payloads map to safety events (F13 operator stop / clear). */
static void on_app_deliver(uint8_t type, uint8_t seq, const uint8_t *payload,
			   uint16_t len, void *ctx)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(ctx);
	if (type != GOV_TYPE_CMD || len < 1) {
		return;
	}
	switch (payload[0]) {
	case 0xE5: /* CMD_ESTOP */
		locked_post_event(GOV_EV_OPERATOR_STOP, now_ms());
		break;
	case 0xC1: /* CMD_CLEAR */
		locked_post_event(GOV_EV_OPERATOR_CLEAR, now_ms());
		break;
	default:
		break;
	}
}

/* ---- Watchdog --------------------------------------------------------- */
#if defined(CONFIG_WATCHDOG) && DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
#define HAVE_WDT 1
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

static void watchdog_setup(void)
{
	if (!device_is_ready(wdt)) {
		return;
	}
	struct wdt_timeout_cfg cfg = {
		.flags = WDT_FLAG_RESET_SOC,
		/* Window: must be fed within this bound. Chosen > worst-case
		 * health period (50 ms) with margin so a healthy system never
		 * trips; a hung loop (no feed) resets (SAFETY_SM T11). */
		.window = { .min = 0U, .max = 500U },
	};
	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel >= 0) {
		(void)wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	}
}

static void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}
#else
#define HAVE_WDT 0
static void watchdog_setup(void) {}
static void watchdog_feed(void) {}
#endif

/* Read the reset cause; return true if the last reset was the watchdog. */
static bool was_watchdog_reset(void)
{
#if defined(CONFIG_HWINFO)
	uint32_t cause = 0;
	if (hwinfo_get_reset_cause(&cause) == 0) {
		(void)hwinfo_clear_reset_cause();
		return (cause & RESET_WATCHDOG) != 0U;
	}
#endif
	return false;
}

/* ---- Sensor (I2C, STM32 emulated-hardware target only) ---------------- */
#if defined(CONFIG_I2C)
#define HAVE_SENSOR 1
static struct gov_bus sensor_bus;
static struct gov_sensor sensor;

static bool sensor_setup(void)
{
	if (!gov_hal_zephyr_init(&sensor_bus)) {
		return false;
	}
	const struct gov_sensor_cfg cfg = {
		.bus = &sensor_bus,
		.addr = GOV_SENSOR_I2C_ADDR,
		.reg = GOV_SENSOR_I2C_REG,
		.read_len = 2u,
		.range_min = 0,
		.range_max = 100,
		.dropout_limit = 5u,
		.stuck_limit = 50u,
		.nak_retries = 3u, /* aligns with GOV_MAX_RETRIES */
	};
	gov_sensor_init(&sensor, &cfg);
	return true;
}
#else
#define HAVE_SENSOR 0
static bool sensor_setup(void) { return true; }
#endif

/* ---- Threads ---------------------------------------------------------- */

/* control (prio 4, 100 Hz): the only hard-deadline task. */
static void control_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	float measurement = 0.0f;
#if defined(CONFIG_GOV_TIMING_TRACE)
	uint32_t last_tick = k_uptime_get_32();
#endif
	/* Absolute-cadence periodic timer: first fire after one period, then
	 * every period regardless of how long the loop body takes. */
	k_timer_start(&control_timer, K_MSEC(GOV_CTRL_PERIOD_MS),
		      K_MSEC(GOV_CTRL_PERIOD_MS));
	for (;;) {
		/* Wait for the next control slot (the cadence source). */
		k_sem_take(&control_tick_sem, K_FOREVER);
#if defined(CONFIG_GOV_TIMING_TRACE)
		/* Per-iteration timing trace (EMULATION/virtual-time only) so
		 * tools/timing/measure_loop.py can check period adherence vs
		 * registry §3. Gated off in production builds. */
		uint32_t t_now = k_uptime_get_32();
		LOG_INF("CTRL tick dt=%u", (unsigned)(t_now - last_tick));
		last_tick = t_now;
#endif
		int32_t s;
		bool have_sample = false;
		k_spinlock_key_t key = k_spin_lock(&slot_lock);
		have_sample = gov_slot_get(&sample_slot, &s);
		k_spin_unlock(&slot_lock, key);
		if (have_sample) {
			measurement = (float)s;
		}
		float applied = locked_control_step(measurement, now_ms());
#if !HAVE_SENSOR
		/* No real sensor on this board: close the loop with the plant
		 * output so the node still demonstrates tracking to setpoint. */
		measurement = gov_plant_output(&node.plant);
		ARG_UNUSED(applied);
#else
		ARG_UNUSED(applied);
#endif
		gov_health_note_deadline(&health, true);
		gov_health_note_alive(&health, GOV_HS_CONTROL, now_ms());
		/* Cadence comes from control_tick_sem at the loop top — no
		 * trailing sleep (that would add the loop's work time to the
		 * period and cause drift). */
	}
}

/* link_rx (prio 5): drain rx_byteq → decoder; poll sensor here on the STM32
 * build (data-ready cadence), pushing into the spinlock-guarded slot. */
static void link_rx_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		uint8_t byte;
		if (k_msgq_get(&rx_byteq, &byte, K_MSEC(GOV_HEALTH_PERIOD_MS)) == 0) {
			gov_decoder_push(&link_decoder, byte);
		}
#if HAVE_SENSOR
		if (gov_sensor_poll(&sensor, NULL) == GOV_SENSOR_HEALTHY &&
		    gov_sensor_has_value(&sensor)) {
			k_spinlock_key_t key = k_spin_lock(&slot_lock);
			gov_slot_push(&sample_slot, gov_sensor_value(&sensor));
			k_spin_unlock(&slot_lock, key);
		}
		/* Edge-report sensor health so the node degrades on a fault AND
		 * recovers (T6) when the sensor reads good again. gov_sensor_faults
		 * returns GOV_SFLAG_ bits, which are bit-identical to the specific
		 * sensor/bus telemetry bits in gov_faults.h, so they pass straight
		 * through as the specific-cause mask (DESIGN D7). */
		bool sensor_ok = gov_sensor_health(&sensor) != GOV_SENSOR_FAULTED;
		locked_note_sensor(gov_sensor_faults(&sensor), sensor_ok, now_ms());
#endif
		gov_health_note_alive(&health, GOV_HS_LINK, now_ms());
	}
}

/* link_tx (prio 6): pump the ARQ retransmit timer; map LINK_FAULT → safety. */
static void link_tx_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		bool link_ok = gov_tx_poll(&link_tx, now_ms()) != GOV_TX_LINK_FAULT;
		/* Edge-report link health: degrade on retransmit-exhaustion (T4),
		 * recover (T6) once the link drains cleanly again. */
		locked_note_link(link_ok, now_ms());
		if (!link_ok) {
			gov_tx_clear_fault(&link_tx);
		}
		k_sleep(K_MSEC(GOV_ACK_TIMEOUT_MS / 4));
	}
}

/* telemetry (prio 7, 10 Hz): snapshot node state → ring → link. */
static void telemetry_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	unsigned tick = 0;
	for (;;) {
		struct gov_telem_record rec;
		locked_fill_telemetry(&rec);
		(void)gov_telem_ring_push(&telem_ring, &rec); /* drop-counted */

		struct gov_telem_record out;
		if (HAVE_LINK_UART && gov_telem_ring_pop(&telem_ring, &out)) {
			/* Reliable telemetry only runs when a link UART exists.
			 * On a board with no link hardware (qemu_cortex_m3 /
			 * native_sim standalone) there is no peer to ACK, so
			 * running ARQ would spuriously latch LINK_FAULT — there
			 * is genuinely no link to fault. The emulated-hardware
			 * (STM32/Renode) build has the link + a ground station. */
			uint8_t payload[GOV_TELEM_DATA_LEN];
			size_t n = gov_telem_encode(&out, payload, sizeof payload);
			if (n > 0) {
				(void)gov_tx_send(&link_tx, GOV_TYPE_DATA,
						  payload, (uint16_t)n, now_ms());
			}
		}
		/* Structured console status line (~1 Hz) — the stable, greppable
		 * format the Renode/Robot fault scenarios assert on. Kept distinct
		 * from the binary DATA telemetry above (which goes out the link
		 * UART); this is human/scenario-readable on the console UART. */
		if ((tick++ % 10u) == 0u) {
			LOG_INF("GOV state=%s faults=0x%x meas=%d out=%d",
				gov_safety_state_name((gov_state_t)rec.state),
				(unsigned)rec.fault_flags, (int)rec.measurement,
				(int)rec.output);
		}
		k_sleep(K_MSEC(GOV_TELEM_PERIOD_MS));
	}
}

/* health (prio 8, 20 Hz): aggregate liveness, feed the watchdog ONLY when the
 * whole system is scheduling correctly (TASKS §4), post timing faults. */
static void health_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		gov_health_note_alive(&health, GOV_HS_SENSOR, now_ms());
		/* Edge-report control-loop timing: degrade on missed deadlines
		 * (T10), recover (T6) when the loop meets its deadlines again. */
		locked_note_timing(!gov_health_timing_fault(&health), now_ms());
		if (gov_health_feed_watchdog(&health, now_ms(),
					     3u * GOV_HEALTH_PERIOD_MS)) {
			watchdog_feed();
		}
		k_sleep(K_MSEC(GOV_HEALTH_PERIOD_MS));
	}
}

int main(void)
{
	LOG_INF("governor boot: node online");

	float setpoint = 50.0f; /* compile-time default */
#if defined(CONFIG_SETTINGS)
	/* Restore persisted config across resets (/F15). On first boot or a
	 * fully-corrupt store this returns defaults; a torn prior write recovers
	 * the last-valid slot (lib/config A/B logic). */
	struct gov_config cfg;
	if (gov_persist_init(&config_ctx, &cfg)) {
		setpoint = (float)cfg.setpoint;
		LOG_INF("config: setpoint=%d boot_count=%u (persisted)",
			(int)cfg.setpoint, cfg.boot_count);
		/* Bump + persist boot_count so "survives reset" is observable. */
		cfg.boot_count++;
		(void)gov_persist_save(&config_ctx, &cfg);
	} else {
		LOG_WRN("config: persistence unavailable — using defaults");
	}
#endif
	gov_node_init(&node, setpoint);
	gov_health_init(&health);
	gov_telem_ring_init(&telem_ring);
	gov_slot_init(&sample_slot);
	gov_decoder_init(&link_decoder, on_decoded_frame, NULL);
	gov_tx_init(&link_tx, link_wire_out, NULL);
	gov_rx_init(&link_rx, link_wire_out, on_app_deliver, NULL);

	/* If we just came back from a watchdog reset, boot INIT with the sticky
	 * FAULT_WATCHDOG bit so telemetry reports the cause (SAFETY_SM T11). */
	if (was_watchdog_reset()) {
		gov_safety_init_sticky(&node.safety, GOV_FAULT_WATCHDOG);
		LOG_WRN("boot after watchdog reset (sticky FAULT_WATCHDOG)");
	}

	bool ok = sensor_setup();
	watchdog_setup();

#if HAVE_LINK_UART
	if (device_is_ready(link_uart)) {
		uart_irq_callback_user_data_set(link_uart, link_uart_isr, NULL);
		uart_irq_rx_enable(link_uart);
	}
#endif

	/* Self-test result drives INIT→RUN (T1) or INIT→SAFE_STOP (T2). Still
	 * single-threaded here (threads start below), but keep the locked path
	 * uniform so every SM access goes through node_lock. */
	if (ok) {
		locked_selftest_ok(now_ms());
	} else {
		locked_post_event(GOV_EV_SELFTEST_FAIL, now_ms());
		LOG_ERR("self-test failed: sensor bus not ready → SAFE_STOP");
	}
	LOG_INF("state=%s setpoint=50", gov_safety_state_name(gov_node_state(&node)));

	k_thread_create(&health_thread, health_stack, HEALTH_STACK, health_entry,
			NULL, NULL, NULL, HEALTH_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&health_thread, "health");
	k_thread_create(&telemetry_thread, telemetry_stack, TELEMETRY_STACK,
			telemetry_entry, NULL, NULL, NULL, TELEMETRY_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&telemetry_thread, "telemetry");
	k_thread_create(&link_tx_thread, link_tx_stack, LINK_TX_STACK, link_tx_entry,
			NULL, NULL, NULL, LINK_TX_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&link_tx_thread, "link_tx");
	k_thread_create(&link_rx_thread, link_rx_stack, LINK_RX_STACK, link_rx_entry,
			NULL, NULL, NULL, LINK_RX_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&link_rx_thread, "link_rx");
	k_thread_create(&control_thread, control_stack, CONTROL_STACK, control_entry,
			NULL, NULL, NULL, CONTROL_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&control_thread, "control");
	return 0;
}
