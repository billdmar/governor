#!/usr/bin/env python3
# =============================================================================
# governor — measure_loop.py
#
#   *** EMULATION / VIRTUAL-TIME MEASUREMENT — NOT SILICON PERFORMANCE ***
#
# Measures periodic-loop cadence from a captured QEMU/Renode console log using
# Zephyr LOG timestamps ([HH:MM:SS.mmm,uuu] <inf> ...), and checks it against
# the registered timing bounds (config/registry.md §3). All timing is QEMU/
# Renode VIRTUAL TIME: it validates timing STRUCTURE (period adherence, jitter
# shape, deadline behaviour, cadence stability), NEVER silicon timing.
#
# --------------------------------------------------------------------------
# WHAT THIS CAN MEASURE FROM THE CURRENT FIRMWARE
# --------------------------------------------------------------------------
# The firmware's only periodic console line is the ~1 Hz "GOV state=..." status
# (telemetry thread, emitted every 10th 100 ms telemetry tick -> ~1.1 s in
# practice). That lets us measure the TELEMETRY-thread cadence adherence/jitter
# in virtual time — a real structural signal that the scheduler is stable and
# that the telemetry period is not drifting or stalling.
#
# It DOES NOT let us measure the 10 ms CONTROL-loop period (§3 rows 1-2): the
# control thread emits no per-iteration timestamped line, so control period
# adherence / jitter / missed-deadline count are NOT observable from the
# console. See CONTROL_INSTRUMENTATION_NOTE for the exact one-line firmware
# change  must add to make §3 rows 1-2 directly measurable.
#
# Input : a captured log with LOG timestamps on stdin or as a file argument.
#         (Requires CONFIG_LOG_MODE_MINIMAL=n so timestamps are emitted; the
#          production prj.conf uses minimal mode -> no timestamps. See the note
#          printed at the end for the exact config  must add.)
# Output: cadence table + PASS/FAIL vs the derived bound for whatever periodic
#         line is present.
# Exit  : 0 = measured cadence within bound; 1 = out of bound / missed;
#         2 = insufficient timing data in the log (documents what's needed).
#
# Pure Python 3.12 stdlib. Owned by SA-timing (tools/timing/**).
# =============================================================================
from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass

# --- Registered timing bounds (config/registry.md §3) -----------------------
GOV_CTRL_PERIOD_MS = 10.0
GOV_CTRL_JITTER_MS = 1.0     # +/- tolerance band for control-period adherence
GOV_TELEM_PERIOD_MS = 100.0  # telemetry tick; GOV-state line every 10 ticks
ADHERENCE_MIN_PCT = 99.0     # >= 99% of periods within band (control, §3 row1)

# The "GOV state" line prints every 10th telemetry tick. Nominal spacing is
# 10 * 100 ms = 1000 ms of loop work, but each tick also sleeps 100 ms AFTER
# doing work, so observed virtual-time spacing is ~1100 ms. We measure the
# stability of that spacing (jitter as a fraction of the mean), not an absolute
# 1000 ms target, because the target depends on work-vs-sleep accounting the
# console can't disambiguate.
GOV_STATE_JITTER_TOL_PCT = 5.0  # cadence stability tolerance (structural)

CONTROL_INSTRUMENTATION_NOTE = """\
TO MEASURE §3 rows 1-2 (control period adherence / jitter / missed deadlines)
DIRECTLY,  (owns src/) should add ONE timestamped log line inside the
control loop in src/main.c control_entry(), e.g. just before k_sleep():

    uint32_t t = k_uptime_get_32();
    LOG_INF("CTRL tick dt=%u", (unsigned)(t - last)); last = t;

(guarded behind a Kconfig like CONFIG_GOV_TIMING_TRACE so it is OFF in the
shipped build). This parser already recognises a "CTRL tick" / "CTRL dt=<n>"
line: if present it measures true 10 ms control-period adherence against the
+/-1 ms band and the >=99% / 0-missed bounds. Absent that line, only telemetry
cadence is observable from the console."""

# --- Log line patterns (verified against a real qemu_cortex_m3 run) ---------
_ANSI = re.compile(r"\x1b\[[0-9;]*m")
# Zephyr deferred/immediate timestamp: [HH:MM:SS.mmm,uuu]
_TS = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d{3}),(\d{3})\]")
_GOV_STATE = re.compile(r"GOV state=")
# Optional dedicated control-loop trace line  may add (see note above).
_CTRL_TICK = re.compile(r"CTRL tick(?:.*?dt=(\d+))?")
_CTRL_DT = re.compile(r"CTRL .*?dt=(\d+)")


def _ts_ms(m: re.Match) -> float:
    hh, mm, ss, milli, micro = (int(m.group(i)) for i in range(1, 6))
    return ((hh * 3600 + mm * 60 + ss) * 1000.0) + milli + micro / 1000.0


@dataclass
class Series:
    label: str
    stamps_ms: list[float]

    def deltas(self) -> list[float]:
        return [b - a for a, b in zip(self.stamps_ms, self.stamps_ms[1:])]


def collect(lines) -> tuple[Series, list[int]]:
    """Return (gov_state cadence series, explicit CTRL dt list if  added
    the trace line)."""
    gov_stamps: list[float] = []
    ctrl_dts: list[int] = []
    for raw in lines:
        line = _ANSI.sub("", raw)
        # explicit control trace (only if  instrumented it)
        dm = _CTRL_DT.search(line)
        if dm:
            ctrl_dts.append(int(dm.group(1)))
            continue
        if _GOV_STATE.search(line):
            tm = _TS.search(line)
            if tm:
                gov_stamps.append(_ts_ms(tm))
    return Series("GOV-state cadence (telemetry, ~1 Hz)", gov_stamps), ctrl_dts


def _stats(deltas: list[float]) -> dict:
    return {
        "n": len(deltas),
        "mean": statistics.fmean(deltas),
        "min": min(deltas),
        "max": max(deltas),
        "stdev": statistics.pstdev(deltas) if len(deltas) > 1 else 0.0,
    }


def report_control_trace(dts: list[int]) -> int:
    """Direct §3 control-loop measurement — only reachable if  added the
    CTRL trace line."""
    print("-" * 74)
    print("CONTROL LOOP — direct trace (EMULATION / virtual time)")
    band_lo, band_hi = GOV_CTRL_PERIOD_MS - GOV_CTRL_JITTER_MS, \
        GOV_CTRL_PERIOD_MS + GOV_CTRL_JITTER_MS
    in_band = sum(1 for d in dts if band_lo <= d <= band_hi)
    adherence = in_band / len(dts) * 100.0
    missed = sum(1 for d in dts if d > band_hi)
    st = _stats([float(d) for d in dts])
    print(f"  periods={st['n']}  mean={st['mean']:.3f} ms  "
          f"min={st['min']:.0f}  max={st['max']:.0f}  stdev={st['stdev']:.3f}")
    print(f"  target: {GOV_CTRL_PERIOD_MS:.0f} +/- {GOV_CTRL_JITTER_MS:.0f} ms  "
          f"[{band_lo:.0f}, {band_hi:.0f}]")
    print(f"  adherence: {adherence:.2f}%  (bound >= {ADHERENCE_MIN_PCT}%)")
    print(f"  missed deadlines: {missed}  (bound = 0 nominal)")
    ok = adherence >= ADHERENCE_MIN_PCT and missed == 0
    print(f"  RESULT: {'PASS' if ok else 'FAIL'}  (EMULATION / virtual time)")
    return 0 if ok else 1


def report_gov_cadence(series: Series) -> int:
    deltas = series.deltas()
    print("-" * 74)
    print(f"{series.label} — cadence stability (EMULATION / virtual time)")
    if len(deltas) < 3:
        print("  INSUFFICIENT DATA: need >=4 timestamped 'GOV state' lines.")
        print("  Likely cause: CONFIG_LOG_MODE_MINIMAL=y (no timestamps) or run"
              " too short.")
        return 2
    st = _stats(deltas)
    jitter_pct = (st["stdev"] / st["mean"] * 100.0) if st["mean"] else 0.0
    spread_pct = ((st["max"] - st["min"]) / st["mean"] * 100.0) if st["mean"] else 0.0
    print(f"  intervals={st['n']}  mean={st['mean']:.1f} ms  "
          f"min={st['min']:.1f}  max={st['max']:.1f}")
    print(f"  stdev={st['stdev']:.3f} ms  jitter={jitter_pct:.3f}% of mean  "
          f"peak-spread={spread_pct:.3f}%")
    ok = jitter_pct <= GOV_STATE_JITTER_TOL_PCT
    print(f"  cadence stability bound: jitter <= {GOV_STATE_JITTER_TOL_PCT}% "
          f"of mean  ->  {'PASS' if ok else 'FAIL'}")
    print("  NOTE: this is the TELEMETRY cadence, a structural stability proxy."
          " It is NOT the 10 ms control period.")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Measure loop cadence from a QEMU/Renode console log "
                    "(EMULATION / virtual time).")
    ap.add_argument("logfile", nargs="?", default="-",
                    help="console log file (default: stdin)")
    args = ap.parse_args()

    if args.logfile == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.logfile, "r", errors="replace") as f:
            lines = f.readlines()

    print("=" * 74)
    print("governor loop-timing report  —  EMULATION / virtual time (NOT silicon)")
    print("bounds: config/registry.md §3")
    print("=" * 74)

    series, ctrl_dts = collect(lines)

    rc = 0
    if ctrl_dts:
        rc = report_control_trace(ctrl_dts)
        # telemetry cadence is still informative alongside a real control trace
        report_gov_cadence(series)
    else:
        print("control-loop period (§3 rows 1-2): NOT MEASURABLE from console.")
        print("  The control thread emits no per-iteration timestamped line.")
        rc_c = report_gov_cadence(series)
        rc = rc_c

    print("-" * 74)
    print(CONTROL_INSTRUMENTATION_NOTE)
    print("=" * 74)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
