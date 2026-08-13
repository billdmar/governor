#!/usr/bin/env python3
# =============================================================================
# tests/timing/test_parsers.py — self-tests for the SA-timing parsers.
#
# These validate the parsers against SAMPLE THREAD_ANALYZER / LOG lines whose
# exact format was VERIFIED against a real qemu_cortex_m3 run (Zephyr 3.7):
#   I:  control  : STACK: unused 856 usage 168 / 1024 (16 %); CPU: 5 %
#   [00:00:01.100,000] <inf> governor: GOV state=RUN faults=0x0 meas=49 out=50
#
# The parenthetical "(N %)" is the USAGE percentage, so the parser must derive
# UNUSED% = unused/size (regression guard for that gotcha). No Zephyr / QEMU
# build required — pure stdlib unittest, runnable in CI standalone.
#
#   python3 -m unittest tests.timing.test_parsers   (from repo root)
#   or:  python3 tests/timing/test_parsers.py
#
# All measured values are EMULATION / virtual time (NOT silicon).
# Owned by SA-timing (tests/timing/**).
# =============================================================================
import importlib.util
import io
import os
import sys
import unittest
from contextlib import redirect_stdout

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_TOOLS = os.path.join(_ROOT, "tools", "timing")


def _load(mod_name, filename):
    spec = importlib.util.spec_from_file_location(
        mod_name, os.path.join(_TOOLS, filename))
    mod = importlib.util.module_from_spec(spec)
    # Register before exec so @dataclass can resolve the module via sys.modules.
    sys.modules[mod_name] = mod
    spec.loader.exec_module(mod)
    return mod


pta = _load("parse_thread_analyzer", "parse_thread_analyzer.py")
ml = _load("measure_loop", "measure_loop.py")

# --- Fixtures: exact real-format lines (minimal log mode) -------------------
MINIMAL_ANALYZER = """\
I: Thread analyze:
I:  control             : STACK: unused 856 usage 168 / 1024 (16 %); CPU: 5 %
I:  link_rx             : STACK: unused 840 usage 184 / 1024 (17 %); CPU: 0 %
I:  link_tx             : STACK: unused 896 usage 128 / 1024 (12 %); CPU: 1 %
I:  telemetry           : STACK: unused 772 usage 252 / 1024 (24 %); CPU: 3 %
I:  health              : STACK: unused 640 usage 128 / 768 (16 %); CPU: 1 %
I:  idle                : STACK: unused 208 usage 48 / 256 (18 %); CPU: 0 %
I:  ISR0                : STACK: unused 1880 usage 168 / 2048 (8 %)
"""

# A thread deliberately under the 25% unused margin (unused 200/1024 = 19.5%).
UNDER_MARGIN = MINIMAL_ANALYZER.replace(
    "control             : STACK: unused 856 usage 168 / 1024 (16 %)",
    "control             : STACK: unused 200 usage 824 / 1024 (80 %)")

# Deferred-mode timestamped GOV-state lines (real spacing 1100 ms virtual).
TS_GOV = "".join(
    f"[00:00:{s:02d}.{ms:03d},000] <inf> governor: "
    f"GOV state=RUN faults=0x0 meas=49 out=50\n"
    for s, ms in [(0, 0), (1, 100), (2, 200), (3, 300), (4, 400)])

CTRL_TRACE = "".join(
    f"[00:00:00.{i*10:03d},000] <inf> governor: CTRL tick dt={dt}\n"
    for i, dt in enumerate([10, 10, 11, 9, 10, 10]))


class TestStackParser(unittest.TestCase):
    def test_unused_pct_derived_not_usage(self):
        seen = pta.parse(MINIMAL_ANALYZER.splitlines())
        # control: unused 856 / 1024 = 83.6% unused (NOT the 16% usage figure).
        self.assertAlmostEqual(seen["control"].unused_pct, 856 / 1024 * 100, places=1)
        self.assertGreater(seen["control"].unused_pct, 80.0)

    def test_all_registered_present_and_pass(self):
        seen = pta.parse(MINIMAL_ANALYZER.splitlines())
        for name in pta.REGISTERED_THREADS:
            self.assertIn(name, seen)
        with redirect_stdout(io.StringIO()):
            rc = pta.report(seen)
        self.assertEqual(rc, 0)

    def test_under_margin_fails(self):
        seen = pta.parse(UNDER_MARGIN.splitlines())
        with redirect_stdout(io.StringIO()):
            rc = pta.report(seen)
        self.assertEqual(rc, 1)

    def test_empty_log_exits_2(self):
        with redirect_stdout(io.StringIO()):
            rc = pta.report(pta.parse(["nothing here"]))
        self.assertEqual(rc, 2)

    def test_health_768_budget(self):
        seen = pta.parse(MINIMAL_ANALYZER.splitlines())
        self.assertEqual(seen["health"].size, 768)


class TestLoopParser(unittest.TestCase):
    def test_gov_cadence_stable(self):
        series, ctrl = ml.collect(TS_GOV.splitlines())
        self.assertEqual(ctrl, [])
        self.assertEqual(len(series.stamps_ms), 5)
        deltas = series.deltas()
        # Fixture spacing is 1100 ms virtual (00.000, 01.100, 02.200, ...),
        # matching the real ~1 Hz GOV-state cadence; perfectly stable.
        self.assertTrue(all(abs(d - 1100.0) < 1e-6 for d in deltas))

    def test_control_trace_measured_when_present(self):
        _, ctrl = ml.collect(CTRL_TRACE.splitlines())
        self.assertEqual(ctrl, [10, 10, 11, 9, 10, 10])
        with redirect_stdout(io.StringIO()):
            rc = ml.report_control_trace(ctrl)
        self.assertEqual(rc, 0)  # 100% within +/-1 ms, 0 missed

    def test_control_trace_missed_deadline_fails(self):
        with redirect_stdout(io.StringIO()):
            rc = ml.report_control_trace([10, 10, 25, 10])  # 25 ms > band
        self.assertEqual(rc, 1)

    def test_no_timestamps_insufficient(self):
        series, _ = ml.collect(MINIMAL_ANALYZER.splitlines())
        with redirect_stdout(io.StringIO()):
            rc = ml.report_gov_cadence(series)
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
