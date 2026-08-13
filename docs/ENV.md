# ENV — toolchain & reproducible build environment

All versions **pinned**; recorded at  on the build machine. Re-run
`source tools/env.sh` in any shell to get an identical environment.

## Build machine ()
- macOS (Darwin 25.x), Apple Silicon (arm64), 10 cores.
- Xcode Command Line Tools present; Homebrew 6.0.15 at `/opt/homebrew`.

## Pinned tool versions
| Tool | Version | Source |
|------|---------|--------|
| Zephyr RTOS | **v3.7.0 (LTS v3)** | west manifest (`west.yml`), checked out at `deps/zephyr` |
| Zephyr SDK | **0.16.8** (`arm-zephyr-eabi` GCC 12.2.0) | github sdk-ng, minimal + arm toolchain, at `zephyr-sdk-0.16.8/` |
| west | v1.5.0 | pip, in `.venv` |
| CMake (used) | **3.31.6** | pip, in `.venv` — see note below |
| CMake (Homebrew) | 4.4.2 | `/opt/homebrew` — **not used for Zephyr** (see note) |
| Ninja | 1.13.2 | Homebrew |
| dtc | 1.8.1 | Homebrew |
| cppcheck | 2.21.0 | Homebrew |
| QEMU | 11.0.3 (`qemu-system-arm`) | Homebrew |
| clang-tidy / clang-format / llvm-cov | LLVM 22.1.8 | Homebrew `llvm` (keg-only, `/opt/homebrew/opt/llvm/bin`) |
| Apple clang | 21.0.0 | Xcode CLT (host fuzz/test compiler) |
| Python | 3.12.13 | `.venv` (from `~/.local/bin/python3.12`) |

## Targets & the macOS caveat (honest labeling)
- **`qemu_cortex_m3`** — real ARM Cortex-M3 cross-compile, runs under QEMU.
  **This is the primary LOCAL emulated-hardware target on macOS.** Verified at :
  builds clean and boots to log under `west build -t run`.
- **Renode STM32 machine** — the fault-injection matrix target ().
- **`native_sim`** — Zephyr's POSIX architecture is **Linux-only**; it **does not
  build on macOS** (`arch/posix` aborts: "POSIX architecture only works on
  Linux"). Per the design notes's documented fallback, `native_sim` runs **in CI on
  ubuntu-latest** (twister), while local macOS work uses `qemu_cortex_m3` plus
  **standalone host-compiled** unit tests + libFuzzer for the host-portable
  modules (which need no Zephyr at all). See DESIGN.md for the rationale.

## Twister needs setuptools < 81
Zephyr 3.7's twister imports `pkg_resources`, which setuptools removed in v81.
The venv (and CI) pin **`setuptools<81`** (80.10.2 locally) so `west twister`
runs. Symptom if unpinned: `ModuleNotFoundError: No module named 'pkg_resources'`.

## Two CMake versions — why
Zephyr v3.7.0 predates CMake 4 and its stricter `if()`/argument parsing breaks
Zephyr's `FindZephyr-sdk.cmake`. We pin **CMake 3.31.6 in the venv** and put the
venv first on `PATH` for all Zephyr builds. Homebrew's CMake 4.4.2 remains for
other projects. `tools/env.sh` sets this up.

## Reproduce
```bash
source tools/env.sh          # PATH, ZEPHYR_BASE, ZEPHYR_SDK_INSTALL_DIR, venv cmake
west build -b qemu_cortex_m3 .          # build
west build -b qemu_cortex_m3 -t run .   # build + boot under QEMU
```
