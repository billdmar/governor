# TASKS — governor task architecture & priority table (FROZEN @ )

> **Contract status: FROZEN.** . Zephyr thread priorities, queue
> depths, and stack budgets below are the target-side realization; the
> host-portable logic modules are agnostic to it. Every number is justified —
> that is the point of this document. Deviations logged in DESIGN.md.

## 1. Principles
- **Rate-monotonic-inspired priority:** the hard-real-time periodic control loop
  is the highest application priority; less time-critical / bursty work runs
  below it. Nothing that can block indefinitely runs at or above the control
  loop.
- **ISR does the minimum:** UART/sensor ISRs only move bytes into a ring/queue
  and signal; all parsing and policy run in threads (the ISR-to-thread handoff).
- **Bounded everything:** every queue has a fixed depth; every thread has a fixed
  static stack; overflow is a *detected, flagged* condition, never silent.
- **No dynamic allocation after init** — all threads, stacks, queues, and buffers
  are statically declared (`K_THREAD_STACK_DEFINE`, `K_MSGQ_DEFINE`, static
  pools). Verified by high-water marks + a no-`malloc`-after-init check.

## 2. Thread / task table
Zephyr cooperative+preemptive priorities: **lower number = higher priority.**
Application threads occupy 4..8; the control loop is the highest app thread.

| Task | Prio | Type | Period / trigger | Queue (depth) | Stack (bytes) | Rationale |
|------|------|------|------------------|---------------|---------------|-----------|
| `control` | 4 | periodic | `GOV_CTRL_PERIOD_MS = 10` (100 Hz) | — (reads latest sample) | 1024 | Highest app prio: the only hard-deadline task. 100 Hz is a decade above the simulated plant's dominant pole — enough loop margin without wasting cycles. Small stack: no recursion, no big locals, fixed-point PID. |
| `link_rx` | 5 | event | UART RX signal from ISR | `rx_byteq` (256 B) | 1024 | Above telemetry so acks/commands are handled promptly. 256 B ≈ 3.6× the max frame (71 B) — absorbs a full frame plus burst without overrun at 115200 baud vs 100 Hz drain. |
| `link_tx` | 6 | event | TX request (telemetry/ack) | `tx_frameq` (4 frames) | 1024 | Handles ARQ retransmit timing. Depth 4: one in-flight (window=1) + up to 3 queued producers before backpressure is *flagged*, matching `GOV_MAX_RETRIES`. |
| `telemetry` | 7 | periodic | `GOV_TELEM_PERIOD_MS = 100` (10 Hz) | `telem_ring` (8 records) | 1024 | 10 Hz heartbeat/telemetry is plenty for a ground station; below control so it never steals a control deadline. Ring depth 8 = 0.8 s of history to survive a link stall without loss-then-flag. |
| `health` | 8 | periodic | `GOV_HEALTH_PERIOD_MS = 50` (20 Hz) | — | 768 | Lowest: feeds the hardware watchdog, aggregates driver/link/control health into safety events. Runs often enough (20 Hz) to catch a missed control deadline within `GOV_MISS_LIMIT`. Small stack: bookkeeping only. |
| `main`/init | (init) | one-shot | boot | — | 2048 | Brings up drivers, creates objects, hands off to threads, then becomes idle/shell. Larger stack tolerates init-time driver calls; not in the steady-state hot path. |

**ISRs:** UART RX ISR → push byte to `rx_byteq`, signal `link_rx`. Sensor
"data-ready" (emulated) → push sample to a 1-deep latest-value slot, signal
`control` sampling. ISRs never parse, never allocate, never block.

## 3. Stack budget method (verified, not guessed)
- Sizes above are **initial budgets**; the **registered margin** (see
  `config/registry.md`) requires measured high-water marks to leave ≥ `GOV_STACK_MARGIN_PCT = 25%` headroom.
- Measured via `CONFIG_THREAD_ANALYZER` + `CONFIG_INIT_STACKS` (unused stack is
  poisoned with a sentinel; the analyzer reports the high-water line). Numbers
  land in the README headline table at . If a thread exceeds its margin, the
  stack is resized and re-justified here — never the margin lowered.

## 4. Watchdog placement philosophy
- One **hardware watchdog** (Renode STM32 IWDG-style), fed **only** by the
  `health` task — never by the control loop directly and never from an ISR.
  Rationale: the health task can only feed the dog if it observed that the
  control loop met its recent deadlines (`GOV_MISS_LIMIT`) and the other threads
  are alive. So the dog proves *the system as a whole* is scheduling correctly,
  not merely that one ISR still fires. A hung control loop ⇒ health stops feeding
  ⇒ dog resets ⇒ boot into INIT with the `FAULT_WATCHDOG` sticky bit (SAFETY_SM
  T11). Feed period and timeout are registered; timeout > worst-case health
  period with margin, so a healthy system never trips.

## 5. Concurrency & data-sharing rules
- Cross-thread data moves through Zephyr `k_msgq`/rings (SPSC where possible) or
  a single-writer latest-value slot guarded by `k_spinlock` for the sensor
  sample — never ad-hoc shared globals.
- The safety SM runs in the `health`/`control` context (single-writer); other
  threads *post events* to it via a lock-free SPSC event queue. The SM itself is
  the host-portable pure module (`lib/safety`), so its logic is unit-tested
  without any RTOS.
