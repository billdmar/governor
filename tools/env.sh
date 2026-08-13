#!/usr/bin/env bash
# governor — reproducible build environment. `source tools/env.sh` from repo root.
# Pins the venv (west + CMake 3.31), Zephyr base, and the Zephyr SDK so every
# shell builds identically. See docs/ENV.md for why CMake is pinned in the venv.
GOV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"

export ZEPHYR_BASE="${GOV_ROOT}/deps/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT="zephyr"
export ZEPHYR_SDK_INSTALL_DIR="${GOV_ROOT}/zephyr-sdk-0.16.8"

# venv first: gives us west + CMake 3.31 (Zephyr 3.7 is incompatible with CMake 4).
export PATH="${GOV_ROOT}/.venv/bin:${PATH}"
# brew llvm (clang-tidy/clang-format/llvm-cov) — keg-only, not on PATH by default.
export PATH="/opt/homebrew/opt/llvm/bin:${PATH}"

# Host fuzz/test compiler: Apple clang ships ASan/UBSan but NOT libFuzzer
# (libclang_rt.fuzzer_osx.a is absent), so use Homebrew LLVM's clang for fuzzing.
export GOV_HOST_CC="/opt/homebrew/opt/llvm/bin/clang"

echo "governor env: zephyr=$(cat "${ZEPHYR_BASE}/VERSION" 2>/dev/null | tr '\n' ' ')"
echo "  west=$(west --version 2>/dev/null)  cmake=$(cmake --version 2>/dev/null | head -1)"
echo "  sdk=${ZEPHYR_SDK_INSTALL_DIR##*/}"
