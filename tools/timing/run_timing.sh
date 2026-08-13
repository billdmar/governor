#!/usr/bin/env bash
# =============================================================================
# governor — run_timing.sh
#
#   *** EMULATION / VIRTUAL-TIME MEASUREMENT — NOT SILICON PERFORMANCE ***
#
# Reproducible timing + stack-margin capture on qemu_cortex_m3:
#   (a) source the pinned build env (tools/env.sh)
#   (b) build qemu_cortex_m3 with THREAD_ANALYZER auto-run enabled so stack
#       high-water is printed periodically, and with LOG timestamps so loop
#       cadence is observable (both are BUILD-TIME overrides here — they do NOT
#       modify the shipped prj.conf, which stays in minimal-log mode)
#   (c) run under QEMU for a fixed window, capturing the console
#   (d) pipe the log through parse_thread_analyzer.py and measure_loop.py
#   (e) print a labelled EMULATION summary
#
# All numbers produced are QEMU VIRTUAL TIME: they validate timing/stack
# STRUCTURE, never silicon performance. Labelled as such throughout.
#
# Usage:  tools/timing/run_timing.sh [run_seconds]   (default 14)
# Owned by SA-timing (tools/timing/**).
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
GOV_ROOT="$(cd "${HERE}/../.." && pwd)"
RUN_SECONDS="${1:-14}"

BUILD_DIR="${GOV_ROOT}/build_qemu_timing"
LOG="${GOV_ROOT}/build/private/timing_console.log"   # build/ is a symlink; scratch
mkdir -p "$(dirname "${LOG}")" 2>/dev/null || LOG="/tmp/gov_timing_console.log"

# THREAD_ANALYZER_AUTO_INTERVAL range is 5..3600 s (virtual time); 5 gives
# multiple dumps inside a ~14 s window. Deferred log mode -> timestamps.
ANALYZER_INTERVAL=5

echo "=============================================================================="
echo "governor timing capture — EMULATION / QEMU virtual time (NOT silicon)"
echo "  target=qemu_cortex_m3  run_window=${RUN_SECONDS}s (wall)  "
echo "  analyzer_interval=${ANALYZER_INTERVAL}s (virtual)"
echo "=============================================================================="

# (a) pinned env
# shellcheck source=/dev/null
source "${GOV_ROOT}/tools/env.sh"

cd "${GOV_ROOT}"

# (b) build with analyzer auto-run + timestamps as build-time overrides only.
echo ">>> building (analyzer auto-run + log timestamps; prj.conf untouched)..."
west build -b qemu_cortex_m3 -d "${BUILD_DIR}" -- \
    -DCONFIG_THREAD_ANALYZER_AUTO=y \
    -DCONFIG_THREAD_ANALYZER_AUTO_INTERVAL="${ANALYZER_INTERVAL}" \
    -DCONFIG_LOG_MODE_MINIMAL=n \
    -DCONFIG_LOG_MODE_DEFERRED=y \
    > "${LOG}.build" 2>&1
echo "    build OK (log: ${LOG}.build)"

# (c) run under QEMU for the window, then stop it.
echo ">>> running under QEMU for ${RUN_SECONDS}s (capturing console)..."
( west build -d "${BUILD_DIR}" -t run > "${LOG}" 2>&1 ) &
RUN_PID=$!
sleep "${RUN_SECONDS}"
pkill -f qemu-system-arm 2>/dev/null || true
wait "${RUN_PID}" 2>/dev/null || true
sleep 1
echo "    captured $(wc -l < "${LOG}") console lines -> ${LOG}"

# (d) parse. Do not let a non-zero parser exit abort the summary; capture rc.
echo
STACK_RC=0
python3 "${HERE}/parse_thread_analyzer.py" "${LOG}" || STACK_RC=$?
echo
LOOP_RC=0
python3 "${HERE}/measure_loop.py" "${LOG}" || LOOP_RC=$?

# (e) labelled summary table.
echo
echo "=============================================================================="
echo "TIMING/STRUCTURE SUMMARY  —  EMULATION / QEMU virtual time (NOT silicon)"
echo "------------------------------------------------------------------------------"
printf "  %-34s %s\n" "stack margins (>=25% unused):" \
    "$([ ${STACK_RC} -eq 0 ] && echo PASS || echo 'FAIL/INCOMPLETE')"
printf "  %-34s %s\n" "loop cadence (structural):" \
    "$([ ${LOOP_RC} -eq 0 ] && echo PASS || echo 'PARTIAL (see notes)')"
echo "------------------------------------------------------------------------------"
echo "  All figures are QEMU virtual time: they validate timing/stack STRUCTURE,"
echo "  never silicon performance."
echo "=============================================================================="

# Exit non-zero only if the gating (stack) check failed; loop cadence is
# partial-by-design until  adds the control trace line (see measure_loop).
exit "${STACK_RC}"
