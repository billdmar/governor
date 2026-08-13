# DESIGN — governor design decisions (living log)

Every non-obvious decision gets 2–4 lines here (the design notes hard rule). Newest
decisions appended; contracts are frozen in their own docs (PROTOCOL_SPEC,
SAFETY_SM, TASKS, registry) and only referenced here.

##  decisions

### D1 — Zephyr v3.7.0 (LTS v3), pinned
Chose the long-term-support release over bleeding edge for stability and because
the Zephyr SDK 0.16.8 and Antmicro's Renode CI action are well-matched to it.
Pinned by tag in `west.yml`.

### D2 — Local target = qemu_cortex_m3; native_sim in CI only
Zephyr 3.7's POSIX architecture (`native_sim`) is **Linux-only** and aborts on
macOS. Rather than weaken the project, we adopt its documented fallback: local
macOS emulated-hardware work runs on `qemu_cortex_m3` (real ARM cross-compile,
verified booting at ), plus **standalone host-compiled** unit tests + libFuzzer
for the host-portable modules (which have no Zephyr dependency by design, so they
compile with Apple clang directly). CI on ubuntu-latest additionally runs
`native_sim` under twister. This keeps every capability covered without
presenting an emulator as silicon. See docs/ENV.md.

### D3 — CMake pinned to 3.31.6 in the venv
Homebrew shipped CMake 4.4.2; Zephyr 3.7 predates CMake 4 and its stricter parser
breaks `FindZephyr-sdk.cmake`. We pin CMake 3.31.6 in the `.venv` and prepend it
to PATH for Zephyr builds (`tools/env.sh`); Homebrew CMake is untouched for other
projects.

### D4 — Zephyr RTOS checkout relocated to deps/zephyr
This repo is simultaneously the west manifest repo and the Zephyr *application*.
Zephyr auto-includes `<app>/zephyr/Kconfig` as the app's module Kconfig; if the
RTOS itself lives at `<app>/zephyr`, its root Kconfig recursively sources itself.
Moving the RTOS to `deps/zephyr` frees that path. Pinned via `path:` in west.yml.

### D5 — Host-portable-first module boundary
`lib/proto`, `lib/control`, `lib/telem`, `lib/safety` are pure C with **no Zephyr
includes**, so they are unit-tested, property-tested, and fuzzed on the host with
sanitizers before any firmware integration (the  verification). Zephyr sees them
only through thin adapters. This is the core of the verification strategy.

### D6 — Host fuzz/test compiler = Homebrew LLVM clang (not Apple clang)
Apple clang (Xcode) provides ASan/UBSan but **omits libFuzzer**
(`libclang_rt.fuzzer_osx.a` is not shipped), so `-fsanitize=fuzzer` fails to
link. Homebrew LLVM 22.1.8's clang includes it and runs `fuzzer,address,
undefined` cleanly (verified at ). `tools/env.sh` exports `GOV_HOST_CC`
pointing at it; the host test/fuzz Makefiles use `$GOV_HOST_CC`. CI (ubuntu)
uses distro clang, which has libFuzzer natively.

##  decisions

### D7 — Fault-flag ownership split (safety vs. drivers)
The canonical fault-flag bitmask lives in `lib/safety/safety.h` (the SM is the
fault authority, SAFETY_SM invariant 4). But the SM receives only a *generic*
`GOV_EV_SENSOR_FAULT` — it cannot know whether the cause was drop/stuck/range,
which is the driver's knowledge. Per the honesty rule (no fabricated specific
value), the SM sets a coarse `GOV_FAULT_SENSOR_ACTIVE` bit; the driver/health
layer OR-merges the specific `SENSOR_DROP/STUCK/RANGE/BUS_*` bit into the
telemetry word. Telemetry thus reports both "SM says a sensor fault is active"
and "driver says it was specifically X" without either layer guessing.

### D8 — Injected monotonic time in the safety SM
`gov_safety_step(s, ev, now_ms)` takes an explicit monotonic tick rather than
calling any clock, so the SM is fully deterministic and host-testable (all T#
timeouts — init timeout, recover dwell, degrade-max — are exercised by feeding
ticks). Time going backwards is treated as a real internal violation (→
FAULT_HALT), not silently tolerated. Verified by 20 table-driven tests / 74
checks, clean under -Werror -Wconversion + ASan/UBSan + cppcheck.

### D9 — Node coordinator + the sticky-init bug the E2E test caught
`app/node_core.{c,h}` is the host-portable coordinator wiring PID→safety-clamp→
plant→telemetry into one deterministic step (still no Zephyr; the RTOS app is a
thin driver over it). Writing the end-to-end integration test surfaced a real
defect: `gov_safety_init()` read `s->sticky` *before* initializing the struct
(to preserve a watchdog bit across re-init), so a first init of an uninitialized
stack struct leaked garbage into the fault word. Fixed by making
`gov_safety_init()` a clean zero-init that never reads pre-init memory, and
adding an explicit `gov_safety_init_sticky(s, flags)` for the reboot path that
carries a persisted watchdog bit. Lesson logged: model "survives reboot" with an
explicit input, never by reading memory that may be uninitialized.

### D10 —  contract corrections (, not weakenings)
Two frozen-doc fixes at , both corrections rather than relaxations: (a)
PROTOCOL_SPEC `GOV_FRAME_MAX` was 71 mislabelled "incl. SOF"; the SOF byte makes
the full frame 72 — `lib/proto` uses 72. (b) registry `GOV_DIVERGE_LIMIT`,
deliberately delegated to control, resolved to 150.0 plant-units (1.5× the
0..100 envelope). Fault-flag bitmask unified into `lib/common/gov_faults.h`,
included by both lib/safety and lib/telem (D7).

<!-- more  decisions appended as modules land. -->

##  decisions (emulated hardware)

### D11 — Two board classes: qemu_cortex_m3 (logic) + stm32f103_mini (peripherals)
`qemu_cortex_m3` is a TI LM3S6965 SoC with **no I2C and no watchdog**, so the
I2C rows (F04/F05) and watchdog row (F10) physically cannot run there. Rather
than weaken those rows,  adds a **`stm32f103_mini`** target — a real STM32F103
(Cortex-M3) with usart1(console)/usart2(link)/i2c1/iwdg/flash — that maps 1:1
onto Renode's stock `stm32f103.repl`. So the *same ELF* Renode runs is a real
STM32 build. `qemu_cortex_m3` + `native_sim` carry the twister ztest matrix
(logic/timing/link/event rows that need no I2C/WDT). Peripheral-dependent code
is `#ifdef CONFIG_I2C/WATCHDOG`-guarded so all three targets build.

### D12 — Link ARQ only runs where a link UART exists
Reliable telemetry TX (stop-and-wait ARQ) runs only when the board has a link
UART with a peer to ACK. On a link-less board (qemu_cortex_m3/native_sim
standalone) running ARQ would spuriously latch LINK_FAULT — there is genuinely
no link to fault, so asserting one would be dishonest. Gated on `HAVE_LINK_UART`.
The emulated-hardware STM32/Renode build has usart2 + the Python ground station,
where the real F06–F09 link faults are injected.

### D13 — Watchdog fed only by the health task (defense-in-depth)
Per TASKS §4 the hardware IWDG is fed **only** from the `health` thread via
`gov_health_feed_watchdog()`, which returns true only when every subsystem is
alive AND the control loop is meeting deadlines. A hung control loop ⇒ health
stops feeding ⇒ IWDG resets ⇒ boot reads the reset cause (`hwinfo`) and calls
`gov_safety_init_sticky(GOV_FAULT_WATCHDOG)` so telemetry reports the cause
(SAFETY_SM T11). The dog proves the whole system schedules correctly, not just
that one ISR fires.

##  decisions (persistence + reset-recovery)

### D16 — Config persistence: host-portable A/B + thin NVS adapter
Persisted config (`lib/config`) is host-portable with the torn-write safety as
pure logic (A/B double-buffer, CRC-16, monotonic seq; save writes the inactive
slot). The F15 invariant is proven by a property sweep tearing the write at
every byte offset — always old-valid or new-valid, never corrupt. `app/persist.c`
is the thin Zephyr settings/NVS adapter (target-only). Renode has no STM32F1
flash-erase model, so the Renode scenario proves on-target subsystem init/load;
the byte-exact torn-write guarantee is the host suite's job (renode/README.md).

### D17 — Extended fuzz (): 178.8M runs, zero findings
The frame decoder fuzz target ran a sustained 601s (libFuzzer+ASan+UBSan) →
**178,832,918 executions, zero crashes/leaks/overreads**; corpus grew 673→1123
files (committed). This is the  extended-fuzz sweep. Emulated/virtual-time
labeling unchanged.

### D18 — Renode wall-clock timeouts sized for slow shared CI runners
Renode scenarios' `Wait For Line` timeouts are VIRTUAL time and can't fire if
the guest fails to boot, so each suite carries a robot `Test Timeout` (wall
clock) + `renode-test --test-timeout` as real-time guards. Shared GitHub runners
emulate far slower than a dev box (F09: ~6.5s local vs up to 283s on a busy
runner), so these guards are generous (540s / 500s) — they exist to fail a true
hang fast, not to bound correctness. Multi-byte link injections use a single
Monitor round-trip (one `python` write loop) instead of per-byte WriteChar to
cut robot↔Renode RPC overhead.

### D15 — Renode RCC clock model (the STM32-in-Renode boot fix)
Renode's stock `stm32f103.repl` models the RCC clock controller only as a static
`Tag`, so register writes are dropped and reads return a constant. Zephyr's
STM32F1 clock driver enables HSE/PLL and then spins on the readback ready bits
(CR.HSERDY/PLLRDY) and the sysclk-switch status (CFGR.SWS) — which never appear
against a static Tag — so the firmware never leaves `stm32_clock_control_init`,
virtual time freezes, and every Robot `Wait For Line On Uart` (whose timeout is
*virtual* time) blocks forever. This was the entire cause of the "Renode CI job
hangs" symptom. `renode/governor.repl` replaces RCC + IWDG with
`Python.PythonPeripheral` models that behave as read-write registers and force
the ready bits set (CR: HSIRDY|HSERDY|PLLRDY; CFGR: mirror SW→SWS; CSR: LSIRDY).
The node then boots to RUN on the emulated STM32 and all 6 Renode scenarios pass.
Wall-clock guards (`Test Timeout` per suite + `renode-test --test-timeout`) make
any future mis-boot fail fast instead of hanging. Verified on the osx-arm64
Renode 1.16.1 locally; CI uses the linux-portable build headless (no xvfb —
renode-test passes `--disable-gui` itself).

### D14 — Structured console status line for deterministic scenario assertions
The telemetry thread emits a stable ~1 Hz console line
`GOV state=<NAME> faults=0x<hex> meas=<n> out=<n>` distinct from the binary DATA
telemetry on the link UART. Renode/Robot scenarios and the timing tooling assert
on this human/scenario-readable line via `Wait For Line On Uart`, keeping the
fault-matrix checks deterministic without decoding the binary link.

## Post- integration hardening

### D19 — Edge-triggered recovery + D7 telemetry merge wired at the coordinator
The frozen matrix requires recoverable faults (F01/F02/F04/F07/F11) to return
DEGRADED→RUN (T6), but the RTOS app only ever posted the *fault* events — it
never posted `GOV_EV_CAUSE_CLEARED`, so a transient fault escalated to SAFE_STOP
at `GOV_DEGRADE_MAX_MS` instead of recovering. Fixed in the host-portable
coordinator (keeping the logic testable, D9): `gov_node_note_{sensor,link,timing}`
track active recoverable causes and post the fault on the rising edge and
`CAUSE_CLEARED` only when the *last* cause clears (so one source clearing while
another is still faulted does not prematurely recover). A *second independent*
cause arriving while already degraded is posted as `GOV_EV_SECOND_FAULT` so it
escalates to SAFE_STOP per T7 (the original app never wired this leg either — it
relied solely on the degrade-max timer). The same path OR-merges
the driver's specific `SENSOR_DROP/STUCK/RANGE/BUS_*` bits into the telemetry
word (D7), which was previously unimplemented — telemetry had only the coarse
`SENSOR_ACTIVE` bit. The coordinator logic is proven by host integration tests
(`tests/integration/test_node.c`, RED→GREEN): recovery, T7 escalation, flapping-
cause gating, shadow-resync across an operator-clear, and F05 degrade-max
escalation. **Honest scope:** the `src/main.c` glue that drives this on the STM32
build (`gov_node_note_*` from the sensor/link/health threads) is exercised only
on the host coordinator — no Renode scenario yet injects a live sensor dropout to
watch DEGRADED→RUN on-target, so the firmware wiring itself is verified by
construction + the host integration suite, not end-to-end in emulation. `src/main.c`
is a thin caller of the host-tested coordinator.

### D20 — Serialize the safety SM across threads (node_lock)
The singleton safety SM is driven from five preemptible threads plus the command
handler, and `gov_safety_step()` is a non-atomic read-modify-write of the state/
fault word. Added a priority-inheritance `K_MUTEX_DEFINE(node_lock)` in the
Zephyr adapter with thin `locked_*` wrappers around every SM access; the SM stays
lock-free and host-portable (the mutex lives only in `src/main.c`). A step is
bounded non-blocking work, so the critical section is short and the 100 Hz
control task is protected from priority inversion. Honest limit: this is
concurrency-correct by construction — QEMU/Renode run in virtual time and cannot
measure real lock contention.

### D21 — Coordinator fault-shadow resync (the T7-wiring bug an audit caught)
The T7 wiring (D19) keeps a coordinator-side shadow of the currently-active
recoverable causes (`active_causes`) plus the merged driver bits
(`driver_faults`), used to decide "first fault → DEGRADED" vs "second independent
fault → `GOV_EV_SECOND_FAULT`/SAFE_STOP". An adversarial completeness audit found
the shadow was reset only in `gov_node_init` (boot). After an escalation and an
operator `CMD_CLEAR` (SAFE_STOP→INIT→RUN), the SM cleared its fault word but the
shadow still held the old causes, so the *first* genuine fault afterward looked
like a second independent fault → posted `GOV_EV_SECOND_FAULT`, which has no
transition in RUN → **silently rejected: an unflagged fault**, violating the
"every fault flagged" safety invariant (and a stale specific bit lingered in
telemetry). Fix: `resync_shadow()` runs after every SM step and clears the shadow
when the SM re-enters **INIT** (the recovery boundary — reboot, watchdog reset, or
operator `CMD_CLEAR` out of SAFE_STOP), so a fresh bring-up starts consistent.
A second adversarial pass caught that an earlier, broader version cleared on *any*
non-actuating state, which erased the specific driver-cause bit (e.g.
`FAULT_BUS_ERR`, registry F05) from the *latched SAFE_STOP* telemetry an operator
inspects to learn why the node stopped — so the clear is deliberately scoped to
INIT and the specific bit persists through SAFE_STOP/FAULT_HALT. Locked by two host
regression tests (`test_e2e_shadow_resync_after_operator_clear` and the SAFE_STOP-
persistence assertion in `test_e2e_bus_error_persists_escalates`, both RED→GREEN).
Lesson: shadow/derived state must be resynced to its source of truth at the right
state-boundary — clearing too eagerly is its own bug.
