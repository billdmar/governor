#!/usr/bin/env python3
# =============================================================================
# governor — parse_thread_analyzer.py
#
#   *** EMULATION / VIRTUAL-TIME MEASUREMENT — NOT SILICON PERFORMANCE ***
#
# Parses Zephyr CONFIG_THREAD_ANALYZER console output (as produced under QEMU /
# Renode in virtual time) and reports per-thread UNUSED-stack headroom against
# the registered margin GOV_STACK_MARGIN_PCT = 25% (config/registry.md §4).
#
# This validates static-memory DISCIPLINE and stack-budget STRUCTURE (poisoned
# high-water marks in an emulator), never silicon stack behaviour. Stack
# high-water is deterministic w.r.t. the code path, so the emulated number is a
# faithful structural measurement — but it is still labelled EMULATION here and
# in every output string, per the design notes.
#
# Input : a captured QEMU/Renode console log on stdin or as a file argument.
# Output: a per-thread table + PASS/FAIL vs the 25% unused-stack margin.
# Exit  : 0 = all registered threads meet the margin; 1 = a margin FAIL or a
#         required steady-state thread was not observed; 2 = no analyzer output
#         found at all (bad log / analyzer not enabled).
#
# Pure Python 3.12 stdlib. Owned by SA-timing (tools/timing/**).
# =============================================================================
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass

# --- Registered stack budgets & margin (config/registry.md §4) --------------
# Single source of truth mirrored here so the parser is self-contained; if the
# frozen registry changes, this table changes with it (never the other way).
STACK_MARGIN_PCT = 25  # GOV_STACK_MARGIN_PCT — required min UNUSED headroom.

# Registered steady-state threads that MUST be observed and MUST meet the
# margin. `main`/init is intentionally NOT here: main() returns 0 in this
# firmware, so its thread terminates and THREAD_ANALYZER cannot report it
# (see MAIN_NOTE below). Its 2048 B budget is a bring-up (init) budget, not a
# steady-state one.
REGISTERED_THREADS = {
    "control": 1024,
    "link_rx": 1024,
    "link_tx": 1024,
    "telemetry": 1024,
    "health": 768,
}

MAIN_NOTE = (
    "note: 'main'/init (2048 B budget, registry §4) is NOT reported by "
    "THREAD_ANALYZER because main() returns 0 and the thread terminates "
    "before steady state; its stack is a bring-up budget, not a hot-path one."
)

# --- Line format (verified against a real qemu_cortex_m3 run, Zephyr 3.7) ----
# Minimal log mode:
#   I:  control             : STACK: unused 856 usage 168 / 1024 (16 %); CPU: 5 %
#   I:  ISR0                 : STACK: unused 1880 usage 168 / 2048 (8 %)
# Deferred log mode (with timestamps + <inf> + module tag):
#   [00:00:00.000,000] <inf> thread_analyzer:  control  : STACK: unused 856 ...
# The parenthetical "(N %)" is the USAGE percentage; UNUSED % is unused/size.
_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_STACK = re.compile(
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s+: STACK: "
    r"unused (?P<unused>\d+) usage (?P<usage>\d+) / (?P<size>\d+) "
    r"\((?P<pct>\d+) ?%\)"
)


@dataclass
class ThreadStack:
    name: str
    unused: int
    usage: int
    size: int

    @property
    def unused_pct(self) -> float:
        return (self.unused / self.size * 100.0) if self.size else 0.0


def parse(lines) -> dict[str, ThreadStack]:
    """Return the LAST observed stack sample per thread (worst case over the
    run is the min unused, but the analyzer prints high-water each dump; the
    last dump is the most-converged high-water, so we keep the min unused seen
    across all dumps to be conservative)."""
    seen: dict[str, ThreadStack] = {}
    for raw in lines:
        line = _ANSI.sub("", raw)
        m = _STACK.search(line)
        if not m:
            continue
        ts = ThreadStack(
            name=m.group("name"),
            unused=int(m.group("unused")),
            usage=int(m.group("usage")),
            size=int(m.group("size")),
        )
        # Keep the conservative (minimum-unused / highest-water) sample.
        prev = seen.get(ts.name)
        if prev is None or ts.unused < prev.unused:
            seen[ts.name] = ts
    return seen


def report(seen: dict[str, ThreadStack]) -> int:
    print("=" * 74)
    print("governor stack-margin report  —  EMULATION / virtual time (NOT silicon)")
    print(f"required unused-stack margin: >= {STACK_MARGIN_PCT}%  (GOV_STACK_MARGIN_PCT)")
    print("=" * 74)

    if not seen:
        print("ERROR: no THREAD_ANALYZER 'STACK:' lines found in the log.")
        print("  Enable CONFIG_THREAD_ANALYZER + CONFIG_THREAD_ANALYZER_AUTO=y")
        print("  (and CONFIG_THREAD_ANALYZER_AUTO_INTERVAL=<sec>) so it prints"
              " periodically.")
        return 2

    hdr = f"{'thread':<18}{'size':>6}{'unused':>8}{'unused%':>9}  result"
    print(hdr)
    print("-" * len(hdr))

    ok = True
    # Registered threads first (the ones that gate the acceptance criteria).
    for name, budget in REGISTERED_THREADS.items():
        ts = seen.get(name)
        if ts is None:
            print(f"{name:<18}{budget:>6}{'--':>8}{'--':>9}  FAIL (not observed)")
            ok = False
            continue
        if ts.size != budget:
            # Not fatal, but flag: the linked stack differs from the registry.
            budget_note = f" [!= registry {budget}]"
        else:
            budget_note = ""
        passed = ts.unused_pct >= STACK_MARGIN_PCT
        ok = ok and passed
        result = "PASS" if passed else "FAIL (< margin)"
        print(f"{name:<18}{ts.size:>6}{ts.unused:>8}{ts.unused_pct:>8.1f}%  "
              f"{result}{budget_note}")

    # Non-registered / runtime threads (idle, thread_analyzer, logging, ISR*):
    # informational only — reported so the picture is complete, not gated.
    extras = sorted(k for k in seen if k not in REGISTERED_THREADS)
    if extras:
        print("-" * len(hdr))
        print("(informational — runtime/infra threads, not registry-gated)")
        for name in extras:
            ts = seen[name]
            print(f"{name:<18}{ts.size:>6}{ts.unused:>8}{ts.unused_pct:>8.1f}%  --")

    print("-" * len(hdr))
    print(MAIN_NOTE)
    print("=" * 74)
    verdict = "PASS — all registered threads meet the 25% margin" if ok \
        else "FAIL — a registered thread is under-margin or unobserved"
    print(f"RESULT: {verdict}   (EMULATION / virtual time)")
    print("=" * 74)
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Parse Zephyr THREAD_ANALYZER output; check the 25% "
                    "unused-stack margin (EMULATION / virtual time).")
    ap.add_argument("logfile", nargs="?", default="-",
                    help="console log file (default: stdin)")
    args = ap.parse_args()

    if args.logfile == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.logfile, "r", errors="replace") as f:
            lines = f.readlines()

    return report(parse(lines))


if __name__ == "__main__":
    raise SystemExit(main())
