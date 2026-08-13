# SAFETY_SM — governor safety state machine (FROZEN @ )

> **Contract status: FROZEN.**  owns `lib/safety/**` and this document
> exclusively (the design notes §4) — never delegated. Host-portable C, no Zephyr
> includes; driven by table-based transition tests covering every trigger below.

## 1. Philosophy
The node is a control system that can hurt the plant (or itself) if it keeps
actuating on bad information. The safety SM is the **single authority** on
whether actuation is allowed. Every fault path defined in `config/registry.md`
terminates in one of these states — there is **no undefined state, no deadlock,
no silent degradation**, and **every transition raises a telemetry fault flag**
(anti-goal: a fault that isn't flagged). The control loop and drivers *report*;
only the safety SM *decides*.

## 2. States
| State | Actuation | Meaning |
|-------|-----------|---------|
| `INIT` | **inhibited** | Power-on. Self-test + driver bring-up. No control output. |
| `RUN` | **enabled** | All health checks pass. PID drives the plant to setpoint. |
| `DEGRADED` | **limited** | A recoverable fault is active. Control continues with a clamped/limited output (or holds last-safe), fault flagged, actively trying to recover. |
| `SAFE_STOP` | **inhibited (latched)** | An unrecoverable or critical fault. Output driven to the defined safe value and **latched**. Exit only via explicit operator `CMD_CLEAR` after the cause clears. |
| `FAULT_HALT` | **inhibited (terminal)** | Safety-logic invariant violation / watchdog-confirmed hang. Terminal: only a reset leaves it. Distinct from SAFE_STOP so telemetry can tell "we stopped safely on purpose" from "we detected we were broken." |

`SAFE_STOP` output = the registered **safe actuator value** (`GOV_SAFE_OUTPUT`,
default 0 = plant de-energized). "Limited" in `DEGRADED` = output clamped to
`±GOV_DEGRADED_CLAMP` of the safe value.

## 3. Transition table (every transition = one trigger + required behavior)
Triggers come from: driver health (sensor), link health (proto), control health
(PID/plant), watchdog, and operator commands. **T#** are the registered IDs the
fault matrix (`config/registry.md`) asserts.

| T# | From | Event / trigger | To | Required behavior (all: flag fault in telemetry) |
|----|------|-----------------|----|--------------------------------------------------|
| T0 | (reset) | power-on | INIT | Clear all state; actuation inhibited. |
| T1 | INIT | self-test + all drivers OK within `GOV_INIT_TIMEOUT_MS` | RUN | Enable actuation; emit `HEARTBEAT`. |
| T2 | INIT | self-test fail OR init timeout | SAFE_STOP | Latch safe output; flag `FAULT_INIT`. |
| T3 | RUN | recoverable sensor fault (dropout/stuck/garbage, see matrix) | DEGRADED | Clamp output; flag the specific sensor fault; start recovery timer. |
| T4 | RUN | link fault: retransmit exhausted (`LINK_FAULT`) | DEGRADED | Keep controlling locally (setpoint held); flag `FAULT_LINK`. |
| T5 | RUN | critical fault: plant diverging / actuator saturated beyond `GOV_DIVERGE_LIMIT` for `GOV_DIVERGE_MS` | SAFE_STOP | Latch safe output; flag `FAULT_DIVERGE`. |
| T6 | DEGRADED | fault cause cleared AND stable for `GOV_RECOVER_MS` | RUN | Restore full actuation; clear that fault flag; keep a recovery counter. |
| T7 | DEGRADED | second independent fault while already degraded, OR a DEGRADED fault persists past `GOV_DEGRADE_MAX_MS` | SAFE_STOP | Latch safe output; flag `FAULT_ESCALATED`. |
| T8 | INIT, RUN, or DEGRADED | operator `CMD` = emergency stop | SAFE_STOP | Latch safe output; flag `FAULT_OPERATOR_STOP`. Accepted during bring-up (INIT) too — an operator e-stop must never be ignored, whatever the phase. |
| T9 | SAFE_STOP | operator `CMD_CLEAR` AND underlying cause reads clear | INIT | Re-run bring-up (→ T1/T2). Never RUN directly from SAFE_STOP. |
| T10 | RUN or DEGRADED | watchdog pre-warn: control loop missed `GOV_MISS_LIMIT` deadlines | DEGRADED | Flag `FAULT_TIMING`; shed optional work. |
| T11 | any operational | hardware watchdog fires (loop truly hung) | FAULT_HALT (via reset) | On reboot, telemetry reports last state = watchdog reset; boot into INIT with `FAULT_WATCHDOG` sticky bit. |
| T12 | any | safety SM detects its own invariant violation (impossible state/event) | FAULT_HALT | Latch safe output; flag `FAULT_INTERNAL`. Fail loud, never guess. |

**Self-loops / no-ops:** an event with no matching row for the current state is
**not** ignored silently — it is counted (`stat_sm_rejected_event`) and, if it is
a fault event that *should* have matched, treated as T12. This is what the
"every trigger covered" test enforces.

## 4. Invariants (asserted in code + tests)
1. Actuation is enabled **only** in RUN (full) and DEGRADED (clamped). INIT,
   SAFE_STOP, FAULT_HALT ⇒ output == safe value. Checked every SM step.
2. SAFE_STOP and FAULT_HALT are **latching**: no event except the specified
   clear/reset leaves them. No timeout auto-exits a safe state.
3. Every state is reachable and every non-terminal state has an exit — **no
   deadlock**. (Proved by the transition-graph test.)
4. Every transition sets exactly the fault flag(s) named above — **no unflagged
   fault** (anti-goal enforced by test: for each T#, assert the flag is set).
5. The SM never allocates and never blocks; one step is bounded work.

## 5. Interface (frozen names — `lib/safety/safety.h`)
```c
typedef enum { GOV_ST_INIT, GOV_ST_RUN, GOV_ST_DEGRADED,
              GOV_ST_SAFE_STOP, GOV_ST_FAULT_HALT } gov_state_t;

typedef enum { /* registered triggers, 1:1 with T# rows */
  GOV_EV_POWER_ON, GOV_EV_SELFTEST_OK, GOV_EV_SELFTEST_FAIL,
  GOV_EV_SENSOR_FAULT, GOV_EV_LINK_FAULT, GOV_EV_DIVERGE,
  GOV_EV_CAUSE_CLEARED, GOV_EV_SECOND_FAULT, GOV_EV_OPERATOR_STOP,
  GOV_EV_OPERATOR_CLEAR, GOV_EV_DEADLINE_MISS, GOV_EV_WATCHDOG,
  GOV_EV_INTERNAL_VIOLATION
} gov_event_t;

void        gov_safety_init(gov_safety_t *s);
gov_state_t gov_safety_step(gov_safety_t *s, gov_event_t ev); /* returns new state */
bool        gov_safety_actuation_allowed(const gov_safety_t *s);
int32_t     gov_safety_clamp_output(const gov_safety_t *s, int32_t desired);
uint32_t    gov_safety_fault_flags(const gov_safety_t *s);    /* → telemetry */
```
Numeric bounds (`GOV_*_MS`, clamps, limits) are defined in `config/registry.md`
and consumed as compile-time constants. **Never widened to pass a test.**
