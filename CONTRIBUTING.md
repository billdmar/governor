# Contributing to governor

governor is embedded firmware built like shipped safety-critical software. The
contribution bar is correspondingly high: the value of this project is that
every claim in the README is backed by a green check that runs in CI. A change
that lands has to *keep* it that way. This document is the real workflow — the
same one every commit on `main` went through.

Read [`README.md`](README.md) for the architecture, and
[`docs/DESIGN.md`](docs/DESIGN.md) for why things are the way they are, before
you start.

---

## 1. The one rule that shapes everything: host-portable-first

The hard logic lives in **host-portable C** with **zero Zephyr includes**:

```
lib/proto    frame decoder, CRC-16, stop-and-wait ARQ, dedup
lib/control  PID, plant model, anti-windup, safety clamp
lib/telem    telemetry encode, ring buffer, health
lib/safety   the INIT→RUN→DEGRADED→SAFE_STOP/FAULT_HALT state machine
lib/config   torn-write-safe A/B persisted config
lib/common   shared fault bitmask (gov_faults.h)
```

These modules compile with plain `cc`/`clang` and are **unit-tested,
property-tested, and fuzzed on the host with sanitizers _before_ any firmware
integration**. This is the ** verification** (`docs/DESIGN.md` D5). Zephyr only
ever sees them through thin adapters (`src/main.c`, `drivers/hal_zephyr.c`,
`app/persist.c`).

Concretely, this means:

- **New logic goes in `lib/**` first**, with a host test, and passes host CI
  before you wire it into firmware. If you find yourself reaching for a
  `<zephyr/...>` include inside `lib/`, stop — that logic belongs behind the HAL
  vtable (`drivers/hal.h`) or in an `app/`/`src/` adapter instead.
- The host build has **no `#ifdef` escape hatch for Zephyr**. `cppcheck` runs
  over `lib/` and `drivers/` with no Zephyr headers on the include path; a stray
  RTOS dependency fails the static-analysis job.
- Firmware-only code (thread setup, driver binding, NVS) is minimal glue and is
  exercised on emulated hardware, not on the host.

If you're not sure where a piece of code belongs, it almost always belongs in
`lib/` behind an explicit input (see D8: the safety SM takes an injected
`now_ms` tick rather than calling a clock, so it is fully deterministic).

---

## 2. Environment setup

Everything is version-pinned; see [`docs/ENV.md`](docs/ENV.md) for the full
table and the rationale behind each pin.

```bash
source tools/env.sh     # pinned Zephyr 3.7.0 + SDK 0.16.8, west 1.5.0,
                        # CMake 3.31.6 (venv), and GOV_HOST_CC for fuzzing
```

Key gotchas `tools/env.sh` handles for you, worth knowing when something breaks:

- **CMake 3.31.6, not 4.x** — Zephyr 3.7 predates CMake 4's stricter parser,
  which breaks `FindZephyr-sdk.cmake`. The venv CMake is put first on `PATH`
  (D3).
- **`setuptools<81`** — Zephyr 3.7's twister imports `pkg_resources`, removed in
  setuptools 81. Unpinned you get `ModuleNotFoundError: No module named
  'pkg_resources'`.
- **`native_sim` does not build on macOS** — Zephyr's POSIX arch is Linux-only.
  Local macOS work uses `qemu_cortex_m3` (real ARM cross-compile) plus the
  standalone host-compiled tests/fuzzer; `native_sim` runs in CI on
  ubuntu-latest (D2).
- **Fuzzing needs Homebrew LLVM clang, not Apple clang** — Apple clang omits
  `libclang_rt.fuzzer_osx.a`, so `-fsanitize=fuzzer` won't link. `env.sh`
  exports `GOV_HOST_CC` pointing at Homebrew LLVM (D6).

---

## 3. Build and verify — the full local loop

A change is not done until all of the following are green. This mirrors the CI
jobs in `.github/workflows/{ci,renode}.yml` exactly; run locally first.

### Build the firmware

```bash
west build -b qemu_cortex_m3 -p always .          # real ARM cross-compile, boots under QEMU
west build -b stm32f103_mini -p always -d build_stm32 .   # the Renode / real-STM32 target (I2C + IWDG)
# native_sim is a CI-only target (Linux); it is built and tested there.
```

### Host unit + property tests (`-Werror`, ASan/UBSan)

```bash
for m in proto control telem drivers safety config integration; do
  make -C "tests/$m" CC=cc test
done
python -m pytest host/tests -q     # Python ground-station mirror
```

Every host suite compiles under `-Wall -Wextra -Werror -Wconversion -Wshadow
-Wpointer-arith` with `-fsanitize=address,undefined -fno-sanitize-recover=all`
(see `tests/host.mk`). A new host-portable module gets its own `tests/<m>/`
directory that includes `../host.mk`.

### The fuzz gate

The frame decoder (`gov_decoder_push`, PROTOCOL_SPEC §4) is continuously fuzzed.
Any change to `lib/proto` framing **must** re-run it clean and commit any new
corpus entries:

```bash
"$GOV_HOST_CC" -g -O1 -fsanitize=fuzzer,address,undefined \
  -I lib/proto -I lib/common fuzz/fuzz_frame.c \
  lib/proto/frame.c lib/proto/crc16.c -o /tmp/fuzz_frame
/tmp/fuzz_frame -max_total_time=60 fuzz/corpus     # verification: 60s, zero findings
```

Zero crashes / leaks / OOM / overreads is the only passing result. A finding is
a bug to fix — **never** a reason to shorten the run (`config/registry.md` §5).

### Coverage (≥ 90% bar on host-portable modules)

```bash
source tools/env.sh && bash tools/coverage.sh
```

`lib/proto`, `lib/control`, `lib/telem`, `lib/safety`, `lib/config` must stay at
**≥ 90%** line coverage (currently 98.16%). Below the bar means *add tests* —
never lower the bar (`config/registry.md` §6).

### Static analysis

```bash
cppcheck --enable=warning,style,performance,portability \
  --error-exitcode=1 --inline-suppr --std=c11 \
  -I lib/common -I lib/proto -I lib/control -I lib/telem -I lib/safety -I drivers \
  lib/ drivers/
```

clang-tidy runs against `.clang-tidy` (bugprone-*, cert-*, misc-*, readability
subset). Every cppcheck inline suppression must carry a documented reason (see
the one justified deviation in `docs/RULES.md`).

### On-target ztest + the Renode fault matrix

```bash
west twister -p qemu_cortex_m3 -T tests/                 # on-target ztest (native_sim in CI)
west build -b stm32f103_mini -d build_stm32 . && \
  renode-test renode/boot.robot renode/system_faults.robot renode/link_faults.robot
```

Renode is not required locally (CI is the source of truth for the emulated-STM32
matrix), but if you touch a driver, adapter, or the boot path, run it. See
[`renode/README.md`](renode/README.md) for how the 16-row fault matrix is proven
in layers.

---

## 4. The gate discipline (non-negotiable)

Every change must hold **all** of these — they are the project's reason to
exist, not bureaucracy:

- **`-Wall -Wextra -Werror -Wconversion` clean** on every target; clang-tidy +
  cppcheck clean. A warning is a latent bug (`docs/RULES.md` R2).
- **No dynamic allocation after init.** No `malloc`/`k_malloc` in the steady
  state; `CONFIG_HEAP_MEM_POOL_SIZE=0` (R1). Static/pool allocation only.
- **Stack margins ≥ 25% unused** on every thread, measured by high-water mark
  (`config/registry.md` §4). If your change grows a stack, justify the new
  budget in `docs/TASKS.md`.
- **The fault matrix in `config/registry.md` is FROZEN.** A required outcome, a
  fault row, or a timing/margin bound is **never weakened to make a test pass**.
  A failing fault scenario is *fixed* and its deterministic replay kept — never
  deleted (`config/registry.md` header; `the project docs` "Never do").
- **No silent degradation.** Every fault path must land in a defined safety
  state and raise a telemetry fault flag. No swallowed errors (R6), no bypassing
  the safety state machine "just for a test."
- **Timing is honest.** QEMU/Renode figures are virtual time; label them as
  emulation everywhere, never as silicon performance.

The frozen contracts (`docs/PROTOCOL_SPEC.md`, `docs/SAFETY_SM.md`,
`docs/TASKS.md`, `config/registry.md`) change only through a deliberate,
documented correction — and a correction is logged in `docs/DESIGN.md` (see D10)
with its rationale, never a quiet relaxation.

---

## 5. Coding standard

- **`docs/RULES.md`** — the MISRA-C:2012-inspired subset (R1–R10) with a
  rationale per rule. Fixed-width integer types on all wire/state data;
  `switch` on an enum handles every case or has an error `default`; bounded
  buffers/loops on anything touching external input; no recursion in
  portable/firmware code; `const`-correctness.
- **Zephyr / Linux-kernel C style** — tabs for indentation, brace and naming
  conventions per `.clang-format`. Run it before committing.
- Match the surrounding code's style and comment density. Comments describe what
  the code does *now* — update them in the same edit that changes the code.
- Every non-obvious decision (a priority, a queue depth, a timeout, a CRC
  polynomial choice) earns 2–4 lines in `docs/DESIGN.md`.

---

## 6. Pull requests

- **Branch from `main`**; never commit to `main` directly. One focused change
  per PR.
- **Both CI workflows must be green** — the `ci` workflow (build-and-test,
  host-tests, static-analysis, host-fuzz jobs) and the `renode-matrix` workflow.
  A red check is a blocker, not a discussion point.
- **Reference the contract you touched.** If your change affects fault handling,
  name the fault-matrix row(s) (e.g. "tightens F07 ARQ starvation") and the
  safety transition(s) (e.g. T4). If it changes a frozen doc, link the
  `docs/DESIGN.md` decision that justifies the correction.
- **Describe your verification.** State which of the loops in §3 you ran and
  paste the headline evidence (test counts, fuzz execs/time with zero findings,
  coverage number, Renode scenarios passed). Fresh command output is the
  currency here — "should work" is not.
- Keep diffs **surgical**: change only what the task needs. Don't refactor
  working code or restyle neighboring lines in the same PR — flag it separately.

Thanks for holding the bar. The whole point of governor is that the green
checks mean something.
