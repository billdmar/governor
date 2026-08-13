# REGISTRY — fault matrix, required outcomes, bounds (FROZEN @ )

> **Contract status: FROZEN.** -only. This is the authority for what "green"
> means. **Required outcomes and bounds are NEVER weakened to pass a test**
> (the design notes §8). A failing scenario is fixed and its deterministic replay kept
> — never deleted. SA-fault () implements each matrix row as a deterministic
> Renode/Robot scenario asserting its Required Outcome; SA-timing measures the
> timing/stack rows.

## 1. Numeric constants (single source of truth → `lib/*/*_config.h`)
| Constant | Value | Justification |
|----------|-------|---------------|
| `GOV_CTRL_PERIOD_MS` | 10 | 100 Hz control loop (TASKS §2). |
| `GOV_TELEM_PERIOD_MS` | 100 | 10 Hz telemetry/heartbeat. |
| `GOV_HEALTH_PERIOD_MS` | 50 | 20 Hz health/watchdog feed. |
| `GOV_MAX_PAYLOAD` | 64 | Bounds the static frame buffer (PROTOCOL_SPEC §2). |
| `GOV_ACK_TIMEOUT_MS` | 100 | Stop-and-wait ARQ retransmit timeout. |
| `GOV_MAX_RETRIES` | 3 | Retransmits before `LINK_FAULT` → DEGRADED. |
| `GOV_INIT_TIMEOUT_MS` | 2000 | Max self-test/bring-up before SAFE_STOP (T2). |
| `GOV_DIVERGE_LIMIT` | 150.0 plant-units | |error| beyond which the plant is "diverging". Resolved at : 1.5× the 0..100 operating envelope (lib/control sets the plant-unit convention; rationale in control_config.h). |
| `GOV_DIVERGE_MS` | 200 | Sustained divergence before SAFE_STOP (T5). |
| `GOV_RECOVER_MS` | 500 | Stable-clear dwell before DEGRADED→RUN (T6). |
| `GOV_DEGRADE_MAX_MS` | 5000 | Max time in DEGRADED before escalate→SAFE_STOP (T7). |
| `GOV_MISS_LIMIT` | 3 | Consecutive control-deadline misses → timing fault (T10). |
| `GOV_STACK_MARGIN_PCT` | 25 | Required unused-stack headroom (measured). |
| `GOV_SAFE_OUTPUT` | 0 | Safe actuator value (plant de-energized). |
| `GOV_DEGRADED_CLAMP` | 50% | Output clamp magnitude in DEGRADED. |

## 2. Fault-injection matrix (the verification — every row must be GREEN)
Each row: an injected fault × where it's injected → the **Required Outcome**
(safety state transition + fault flag + no-deadlock + recovery behavior). "Replay
deterministic" is mandatory for all rows (fixed seed, virtual time).

| ID | Fault (injected) | Layer | Required Outcome (safety state · flag · recovery) |
|----|------------------|-------|---------------------------------------------------|
| F01 | Sensor **dropout** (no data-ready for N periods) | driver | RUN→DEGRADED (T3) · `FAULT_SENSOR_DROP` · on data resume + `GOV_RECOVER_MS` → RUN (T6) |
| F02 | Sensor **stuck** (same value forever) | driver | RUN→DEGRADED (T3) · `FAULT_SENSOR_STUCK` · recover on variance return |
| F03 | Sensor **garbage** (out-of-range/NaN-like) | driver | RUN→DEGRADED (T3) · `FAULT_SENSOR_RANGE` · reject sample, hold last-good, recover |
| F04 | I2C **NAK** on read | driver | RUN→DEGRADED (T3) · `FAULT_BUS_NAK` · retry policy then degrade; recover on ACK |
| F05 | I2C **bus error / SDA stuck low** | driver | RUN→DEGRADED, escalate to SAFE_STOP if persists > `GOV_DEGRADE_MAX_MS` (T7) · `FAULT_BUS_ERR` |
| F06 | UART **single-bit corruption** in frame | link | frame dropped by CRC · `stat_crc_err`++ · sender retransmits · no bad delivery · link stays RUN |
| F07 | UART **frame loss** (ACK never arrives) | link | retransmit ×`GOV_MAX_RETRIES`; success → RUN; exhaustion → `LINK_FAULT` RUN→DEGRADED (T4) · `FAULT_LINK` |
| F08 | UART **duplicate frame** | link | dedup: re-ACK, **no** re-delivery (PROTOCOL_SPEC §5) · `stat_dup`++ · state unchanged |
| F09 | UART **garbage burst** then valid frame | link | decoder resyncs on SOF, delivers the valid frame · no crash/overread |
| F10 | **Watchdog trip** (control loop hung) | system | reset → boot INIT with `FAULT_WATCHDOG` sticky (T11); telemetry reports watchdog reset cause |
| F11 | **Task overrun** (control misses deadlines) | system | `GOV_MISS_LIMIT` misses → DEGRADED (T10) · `FAULT_TIMING` · recover when deadlines met again |
| F12 | **Plant divergence** (error beyond limit, sustained) | control | RUN→SAFE_STOP (T5) · `FAULT_DIVERGE` · latched; exit only via operator clear (T9) |
| F13 | **Operator emergency stop** (`CMD` estop) | operator | any op state→SAFE_STOP (T8) · `FAULT_OPERATOR_STOP` · latched |
| F14 | **Init self-test failure** | system | INIT→SAFE_STOP (T2) · `FAULT_INIT` · latched |
| F15 | **Reset mid config-write** () | persistence | reboot: config is either old-valid or new-valid, never corrupt; `FAULT_NONE` or flagged CRC-recovered · boots to INIT cleanly |
| F16 | **Reset mid in-flight frame** () | persistence+link | reboot mid-frame: decoder starts clean (HUNT_SOF), no partial delivery, link re-syncs on next frame |

**Matrix size = 16 scenarios.** (Reported as the "N" in the README headline
table. F15/F16 land in /.)

## 3. Timing bounds (virtual time — EMULATION, not silicon)
> Measured in Renode/QEMU **virtual time**. These validate *logic and structural
> timing* (period adherence, jitter shape, deadline behavior), **never** silicon
> performance. Labeled as such everywhere they appear.

| Metric | Bound | Meaning |
|--------|-------|---------|
| Control period adherence | ≥ 99% of periods within ±`GOV_CTRL_JITTER_MS`(=1) of 10 ms | Loop scheduled on time in virtual time. |
| Missed deadlines (nominal) | 0 over a 10 s virtual-time run | No missed control deadlines absent injected overrun. |
| Fault→SAFE_STOP latency | ≤ 2×`GOV_CTRL_PERIOD_MS` after trigger | Safe state entered promptly (structural, virtual time). |

## 4. Stack margins (measured at )
| Thread | Budget (B) | Required min unused | Source |
|--------|-----------|---------------------|--------|
| control | 1024 | ≥ 25% | THREAD_ANALYZER high-water |
| link_rx | 1024 | ≥ 25% | " |
| link_tx | 1024 | ≥ 25% | " |
| telemetry | 1024 | ≥ 25% | " |
| health | 768 | ≥ 25% | " |
| main/init | 2048 | ≥ 25% | " |
Plus: **zero heap allocation after init** verified (no `k_malloc`/`malloc` in the
steady state; `CONFIG_HEAP_MEM_POOL_SIZE` = 0 or accounted).

## 5. Fuzz durations (the fuzz gate)
libFuzzer + ASan + UBSan on `gov_decoder_push` (the frame decoder, PROTOCOL_SPEC §4).

| Gate | Min duration | Pass criterion |
|------|-------------|----------------|
|  (local + CI) | **60 s** `-max_total_time` per run, seeded corpus | zero crashes / leaks / OOM / overreads; corpus committed |
|  (extended) | **≥ 1 h** accumulated (background across sessions) | zero findings; corpus grown & committed |
Reported in the README as fuzzing execs + wall-time with **zero findings**.
Any finding is a bug to fix, never a reason to shorten the run.

## 6. Coverage
- **≥ 90%** line coverage on host-portable modules (`lib/proto`, `lib/control`,
  `lib/telem`, `lib/safety`) via twister `--coverage` / llvm-cov. Number reported
  at  and in the README. Below 90% ⇒ add tests, don't lower the bar.
