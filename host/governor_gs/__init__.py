"""governor_gs — host-side ground-station mirror of the governor link layer.

This package is the Python mirror of the target-side C link protocol defined in
``docs/PROTOCOL_SPEC.md`` (FROZEN). It is byte-for-byte compatible with the wire
format: same frame layout, same CRC-16/CCITT-FALSE, same stop-and-wait ARQ, and
the same de-duplication semantics. It exists so the host harness can act as the
ground-station endpoint of the link during native_sim / emulation end-to-end
tests, and so both sides can be cross-checked against a single spec.

Modules:
    protocol    Frame encode/decode + CRC-16 + stop-and-wait ARQ + dedup.
    plant_sim   Reference plant simulator (mirrors the C plant conceptually).
    scenario    Transport abstraction + fault-injecting scenario runner.
    log_capture Structured capture of telemetry/heartbeats for assertions.
"""

from __future__ import annotations

__all__ = ["protocol", "plant_sim", "scenario", "log_capture"]
