/*
 * telem_config.h -- telemetry-layer compile-time constants.
 *
 * Values mirror config/registry.md sec 1 (the single source of truth); they are
 * copied here as the telem module's compile-time constants exactly as the
 * registry prescribes (its "single source of truth" per-module config header).
 * NEVER widen these to pass a test (the design notes sec 8).
 *
 * Host-portable C, no Zephyr includes.
 */
#ifndef GOV_TELEM_CONFIG_H
#define GOV_TELEM_CONFIG_H

/* PROTOCOL_SPEC sec 2 / registry: bounds the static frame + record buffers. */
#define GOV_MAX_PAYLOAD 64u

/* TASKS sec 2: telem_ring depth 8 (= 0.8 s history at 10 Hz). */
#define GOV_TELEM_RING_DEPTH 8u

/* registry sec 1: consecutive control-deadline misses -> timing fault (T10). */
#define GOV_MISS_LIMIT 3u

/* registry sec 1: periodic task rates. Telemetry/heartbeat 10 Hz; the health
 * task that aggregates liveness and feeds the watchdog runs at 20 Hz. */
#define GOV_TELEM_PERIOD_MS 100u
#define GOV_HEALTH_PERIOD_MS 50u

#endif /* GOV_TELEM_CONFIG_H */
