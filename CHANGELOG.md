# Changelog

All notable changes to **governor** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

All timing and stack figures are measured under QEMU / Renode in **virtual time**
— they validate logic and timing *structure*, not silicon performance. See the
README's "Honest emulation limits."

## [Unreleased]

### Fixed
- **Integrated fault recovery (registry F01/F02/F04, T6).** The RTOS app never
  emitted `GOV_EV_CAUSE_CLEARED`, so a *transient* recoverable fault could not
  return `DEGRADED → RUN`; it escalated to `SAFE_STOP` at the degrade-max timeout
  instead. Edge-triggered `gov_node_note_{sensor,link,timing}()` in the
  host-portable coordinator now post the fault on the rising edge and
  `CAUSE_CLEARED` only when the *last* recoverable cause clears.
- **Second-independent-fault escalation (T7).** A distinct second recoverable
  cause arriving while already `DEGRADED` now posts `GOV_EV_SECOND_FAULT` and
  latches `SAFE_STOP` (`FAULT_ESCALATED`) rather than continuing to clamp — a leg
  the original firmware never wired.
- **Specific fault bits in telemetry (DESIGN D7).** The driver's
  `SENSOR_DROP/STUCK/RANGE/BUS_*` bits are now OR-merged into the telemetry /
  heartbeat / fault words; previously only the coarse `SENSOR_ACTIVE` bit shipped.
- **Coordinator fault-shadow resync (DESIGN D21).** The T7 wiring above tracked
  active causes in a coordinator shadow that was not reset at the recovery
  boundary; after an operator-clear the *first* subsequent fault was mis-routed to
  `GOV_EV_SECOND_FAULT` and **silently rejected** in RUN (an unflagged fault). The
  shadow is now cleared when the SM re-enters INIT. Two follow-on adversarial
  passes tightened this: the reset is scoped to INIT (not any non-actuating state)
  so the specific cause bit persists in a latched SAFE_STOP for operator
  diagnosis, and the specific-bit clear on a healthy sensor read is gated on
  actuation so a physical recovery *while latched* can't wipe the root cause.
  Three regression tests pin all three properties (RED→GREEN).

### Added
- **Concurrency: `node_lock`.** A priority-inheritance `K_MUTEX` in the Zephyr
  adapter serializes the singleton safety SM across its five driver threads (the
  SM stays lock-free and host-portable); deadlock-free by construction. See
  DESIGN **D19/D20**.
- Tests: operator e-stop from `INIT` (F13 bring-up branch) and five host
  integration tests (recovery, T7 escalation, flapping-cause gating, shadow-resync
  across operator-clear, F05 degrade-max escalation) —
  **host C checks 2254 → 2298**, all RED→GREEN-proven.
- **Above-the-fold animated demo** (`docs/img/demo.svg`, PNG fallback): the real
  boot → RUN → track → e-stop → SAFE_STOP console sequence.

### Changed
- CI: least-privilege `permissions: contents: read` on both workflows.
- Docs: corrected the committed-corpus count (**1112 → 1123**, matching
  `git ls-files`), reconciled the control-loop-timing wording (adherence % + 0
  missed deadlines over tens of thousands of periods, not a fixed "10 s run"),
  refreshed footprints after the wiring, and made `docs/DEMO.md` §6 reproducible.
- Stack-margin headline updated to the freshly-measured set after the `node_lock`
  wiring: **every task ≥ 63 %** unused (telemetry is now the tightest at 63 %,
  down from ~75 % — the added lock wrapper's frame; still 2.5× the ≥ 25 % bound).

### Planned
- **P6 (stretch):** real-board bring-up on a $15 STM32F103, OTA bootloader, and a
  second node over a radio model — see `docs/BOARD_APPENDIX.md`. Everything to date
  is verified emulation-first and has not run on physical silicon.

## [1.0.0] - 2026-08-10

First complete, verified release: the full telemetry-and-control node, its
16-scenario fault matrix green with required safe-state outcomes, and the
comprehensive documentation set. Built and gated wave-by-wave ( bootstrap →
 verification →  emulated hardware →  persistence/recovery → verification).

### Added

- **Host-portable C core** (`lib/{proto,control,telem,safety,config}`) with **zero
  Zephyr includes** — the hard logic is unit/property/fuzz-tested on the host before
  any firmware, wired to Zephyr through thin adapters ( "verification").
- **Safety state machine** — 5 states (`INIT → RUN → DEGRADED → SAFE_STOP`, plus
  terminal `FAULT_HALT`), transitions **T0–T12**, 14 events; every transition raises
  a telemetry fault flag; SAFE_STOP/FAULT_HALT latch; no deadlock, no silent
  degradation (`lib/safety`, `docs/SAFETY_SM.md`).
- **CRC-framed reliable UART link** — CRC-16/CCITT-FALSE framing, stop-and-wait ARQ
  (window 1, 100 ms timeout, 3 retries → `LINK_FAULT` rather than a silent drop),
  and receiver dedup (`lib/proto`, `docs/PROTOCOL_SPEC.md`).
- **PID control loop** on a simulated plant with output clamping, anti-windup, and
  divergence detection driving the safety SM (`lib/control`, 100 Hz).
- **HAL-abstracted sensor drivers** — a vtable-based I2C sensor driver (0x48) with
  dropout / stuck / range / NAK / bus-error fault hooks (`drivers/`).
- **Torn-write-safe config persistence** — an A/B double-buffer with CRC + monotonic
  sequence; a write always targets the inactive slot, so a power cut leaves the
  stored config old-valid or new-valid, never corrupt (`lib/config`,
  `app/persist.c` binding to Zephyr settings/NVS).
- **5-thread Zephyr node** — rate-monotonic tasks (control p4 · link_rx p5 ·
  link_tx p6 · telemetry p7 · health p8) plus main/init, health-gated hardware
  watchdog (IWDG fed only when the whole system is scheduling), running on
  **stm32f103_mini** (Renode) and **qemu_cortex_m3** (`src/main.c`, `docs/TASKS.md`).
- **Renode STM32F103 machine + Robot fault scenarios** — `governor.repl`/`.resc`
  with documented Python surrogates for the RCC clock and IWDG (Renode 1.16.1 ships
  no STM32F1 models), so the node boots to RUN on the emulated STM32 (`renode/`).
- **Python ground station** — protocol mirror, plant simulator, and scenario runner
  driving the framed usart2 link over TCP (`host/governor_gs`).
- **CI on simulated hardware** — two gating workflows, `ci` (host suites + native_sim
  twister + qemu smoke build) and `renode-matrix` (headless Renode 1.16.1 via
  `antmicro/renode-test-action`), on every push.
- **Full documentation set** — `README`, `DESIGN`, `RULES` (MISRA-inspired subset),
  `PROTOCOL_SPEC`, `SAFETY_SM`, `TASKS`, `ENV`,
  and `BOARD_APPENDIX`; the frozen fault matrix in `config/registry.md`.

### Verified

- **Fault-injection matrix — 16 / 16.** Every registered row **F01–F16** meets its
  required safe-state outcome (correct transition · fault flag raised · no deadlock ·
  clean recovery) with deterministic replay, proven in the layer that produces it
  deterministically — Renode STM32F103 for boot/link/watchdog/e-stop/reset-mid-frame,
  ztest on qemu_cortex_m3 + native_sim for sensor/framing/timing/divergence/init,
  host fake-bus and host property sweep for the rest. Nothing weakened or dropped.
- **Renode STM32 scenarios — 8 / 8** pass in CI on the emulated STM32F103 (boot-to-RUN,
  telemetry line, link starvation, garbage-burst resync, watchdog reset-cause,
  operator e-stop, and the persistence init/load scenarios).
- **Protocol fuzzing — 178,832,918 executions**, libFuzzer + ASan + UBSan on the
  byte-at-a-time frame decoder: **zero** crashes / leaks / overreads; 1123-file
  corpus committed (`fuzz/fuzz_frame.c`).
- **Tests — 2254 host C assertions + 43 Python**, including property tests for frame
  round-trip, single-bit-flip rejection, garbage resync, dedup idempotence, PID
  step-response/anti-windup, config torn-write at every byte offset, and safety
  transitions T0–T12.
- **Coverage — 98.16 %** line coverage on the host-portable modules (proto 98.5 ·
  control 100 · telem 99.0 · safety 96.1 · config 98.8), against a ≥ 90 % bar.
- **Timing (virtual time)** — 100.00 % control-loop period adherence (10.000 ms mean),
  0 missed deadlines over a multi-second capture (tens of thousands of control periods).
- **Stack margins (virtual time)** — every task ≥ 75 % stack unused against a required
  ≥ 25 %; **0** dynamic allocations after init verified.
- **Static discipline** — `-Wall -Wextra -Werror -Wconversion` clean on all targets;
  clang-tidy + cppcheck clean; **0** Zephyr includes in `lib/`.

### Milestones

- **** — Zephyr 3.7 + SDK 0.16.8 workspace bootstrap, scaffold, CI, and the 
  contracts frozen (`6bae73a`).
- **** — host-portable modules (proto/control/telem/safety/config) unit + property
  + fuzz + static clean; native_sim end-to-end green — the verification (`439e43b`,
  `81cd84f`).
- **** — full node on qemu_cortex_m3 + Renode STM32; the fault matrix green with
  required outcomes; stack margins + timing recorded; coverage ≥ 90 % (`8a8e353`,
  CI hardening in `0643997`).
- **** — config persistence + reset-recovery (F15/F16); extended sweeps (`18f8ea4`).
- **** — README, DESIGN/RULES,
  and the real-board appendix (`8cfc1c3`).

[Unreleased]: https://github.com/billdmar/governor/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/billdmar/governor/releases/tag/v1.0.0
