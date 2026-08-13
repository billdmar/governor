/*
 * gov_faults.h — governor fault-flag bit layout (SINGLE SOURCE OF TRUTH).
 *
 * These bits are the u32 bitmask returned by lib/safety's
 * gov_safety_fault_flags() and encoded verbatim into every telemetry DATA
 * record and HEARTBEAT payload (PROTOCOL_SPEC §3; SAFETY_SM §4 invariant 4:
 * "every transition sets exactly the fault flag(s)"). safety and telem MUST
 * agree on the exact bit values — hence this ONE shared header, included by
 * both lib/safety and lib/telem (reconciled by  at ). Host-portable C,
 * no Zephyr includes.
 *
 * Ownership split (docs/DESIGN.md D7): the safety SM is the fault authority but
 * receives only a generic sensor-fault event, so it sets the coarse
 * GOV_FAULT_SENSOR_ACTIVE bit; the driver/health layer OR-merges the specific
 * SENSOR_DROP/STUCK/RANGE/BUS_* bit. Neither layer fabricates a cause it can't
 * distinguish.
 */
#ifndef GOV_FAULTS_H
#define GOV_FAULTS_H

#include <stdint.h>

#define GOV_FAULT_NONE          (0u)

/* --- sensor/driver faults (SAFETY_SM T3; registry F01-F03) --- */
#define GOV_FAULT_SENSOR_DROP   (1u << 0) /* F01: no data-ready for N periods    */
#define GOV_FAULT_SENSOR_STUCK  (1u << 1) /* F02: same value forever             */
#define GOV_FAULT_SENSOR_RANGE  (1u << 2) /* F03: out-of-range / garbage sample  */

/* --- bus faults (SAFETY_SM T3/T7; registry F04-F05) --- */
#define GOV_FAULT_BUS_NAK       (1u << 3) /* F04: I2C NAK on read                */
#define GOV_FAULT_BUS_ERR       (1u << 4) /* F05: I2C bus error / SDA stuck low  */

/* --- link fault (SAFETY_SM T4; registry F07) --- */
#define GOV_FAULT_LINK          (1u << 5) /* F07: retransmit exhausted           */

/* --- control faults (SAFETY_SM T5/T10; registry F11-F12) --- */
#define GOV_FAULT_DIVERGE       (1u << 6) /* F12: plant diverging, sustained     */
#define GOV_FAULT_TIMING        (1u << 7) /* F11: control missed GOV_MISS_LIMIT   */

/* --- system faults (SAFETY_SM T11; registry F10) --- */
#define GOV_FAULT_WATCHDOG      (1u << 8) /* F10: watchdog reset (sticky bit)     */

/* --- operator / init / escalation (SAFETY_SM T8/T2/T7/T12; F13/F14) --- */
#define GOV_FAULT_OPERATOR_STOP (1u << 9)  /* F13: operator emergency stop        */
#define GOV_FAULT_INIT          (1u << 10) /* F14: self-test / init-timeout fail  */
#define GOV_FAULT_ESCALATED     (1u << 11) /* T7:  DEGRADED escalated to SAFE_STOP */
#define GOV_FAULT_INTERNAL      (1u << 12) /* T12: safety-SM invariant violation  */

/* Coarse bit the safety SM owns (docs/DESIGN.md D7): a driver-reported sensor/
 * bus fault is active. The specific cause bit above is set by the driver/health
 * layer, not the SM. */
#define GOV_FAULT_SENSOR_ACTIVE (1u << 13)

/* Mask of the driver-owned specific sensor bits. */
#define GOV_FAULT_SENSOR_ANY \
	(GOV_FAULT_SENSOR_DROP | GOV_FAULT_SENSOR_STUCK | GOV_FAULT_SENSOR_RANGE)

/* Mask of ALL driver-owned specific bits (sensor + bus) the health/driver layer
 * OR-merges into telemetry (DESIGN D7) and clears on a healthy read. */
#define GOV_FAULT_SENSOR_ANY_DRIVER \
	(GOV_FAULT_SENSOR_ANY | GOV_FAULT_BUS_NAK | GOV_FAULT_BUS_ERR)

/* Mask of all defined flag bits — useful for validation/assertions. */
#define GOV_FAULT_ALL           (GOV_FAULT_SENSOR_DROP | GOV_FAULT_SENSOR_STUCK | \
				 GOV_FAULT_SENSOR_RANGE | GOV_FAULT_BUS_NAK |    \
				 GOV_FAULT_BUS_ERR | GOV_FAULT_LINK |            \
				 GOV_FAULT_DIVERGE | GOV_FAULT_TIMING |          \
				 GOV_FAULT_WATCHDOG | GOV_FAULT_OPERATOR_STOP |  \
				 GOV_FAULT_INIT | GOV_FAULT_ESCALATED |          \
				 GOV_FAULT_INTERNAL | GOV_FAULT_SENSOR_ACTIVE)

#endif /* GOV_FAULTS_H */
