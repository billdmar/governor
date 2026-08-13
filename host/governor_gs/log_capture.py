"""log_capture.py — structured capture of telemetry / heartbeats for assertions.

The scenario runner receives DATA telemetry records and HEARTBEAT frames from the
node. This module parses the safety-relevant fields (state, fault flags,
timestamp) into records that tests can assert on, and can serialise them to a
parseable JSON-lines log for offline inspection or CI artifacts.

The HEARTBEAT payload layout mirrors PROTOCOL_SPEC.md §3 ("Liveness + safety-state
byte + fault-flags"): a 1-byte safety state followed by a 4-byte big-endian
fault-flags word. Any extra bytes are retained raw. This is a host-side
convenience decode; the authoritative producer is ``lib/telem`` on the target.
"""

from __future__ import annotations

import enum
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional


class SafetyState(enum.IntEnum):
    """Safety state byte, 1:1 with ``gov_state_t`` in SAFETY_SM.md §6."""

    INIT = 0
    RUN = 1
    DEGRADED = 2
    SAFE_STOP = 3
    FAULT_HALT = 4


@dataclass
class TelemetryRecord:
    """One captured telemetry / heartbeat observation.

    Attributes:
        seq: Frame sequence number it arrived on.
        msg_type: The message type byte (DATA or HEARTBEAT).
        recv_ms: Virtual-time timestamp when the host captured it.
        state: Parsed safety state (heartbeats only), else ``None``.
        fault_flags: Parsed 32-bit fault-flags word (heartbeats only).
        raw: The raw payload bytes (hex-encoded when serialised).
    """

    seq: int
    msg_type: int
    recv_ms: int
    state: Optional[int] = None
    fault_flags: Optional[int] = None
    raw: bytes = b""


@dataclass
class LogCapture:
    """Accumulates telemetry records and answers assertion queries.

    Attributes:
        records: All captured records in arrival order.
    """

    records: list[TelemetryRecord] = field(default_factory=list)

    def capture_heartbeat(self, seq: int, payload: bytes, recv_ms: int) -> TelemetryRecord:
        """Parse and store a HEARTBEAT frame.

        Args:
            seq: Frame sequence number.
            payload: Heartbeat payload (>=5 bytes: state + u32 flags).
            recv_ms: Capture timestamp (virtual ms).

        Returns:
            The stored :class:`TelemetryRecord`.
        """
        state: Optional[int] = None
        flags: Optional[int] = None
        if len(payload) >= 1:
            state = payload[0]
        if len(payload) >= 5:
            flags = int.from_bytes(payload[1:5], "big")
        rec = TelemetryRecord(
            seq=seq,
            msg_type=0x05,
            recv_ms=recv_ms,
            state=state,
            fault_flags=flags,
            raw=bytes(payload),
        )
        self.records.append(rec)
        return rec

    def capture_data(self, seq: int, payload: bytes, recv_ms: int) -> TelemetryRecord:
        """Store a DATA telemetry frame (payload kept raw)."""
        rec = TelemetryRecord(seq=seq, msg_type=0x01, recv_ms=recv_ms, raw=bytes(payload))
        self.records.append(rec)
        return rec

    def last_state(self) -> Optional[int]:
        """Most recently observed safety state, or ``None`` if none seen."""
        for rec in reversed(self.records):
            if rec.state is not None:
                return rec.state
        return None

    def saw_state(self, state: int) -> bool:
        """True if ``state`` was observed in any captured heartbeat."""
        return any(rec.state == state for rec in self.records)

    def saw_fault_bit(self, mask: int) -> bool:
        """True if any captured heartbeat had all bits in ``mask`` set."""
        return any(
            rec.fault_flags is not None and (rec.fault_flags & mask) == mask
            for rec in self.records
        )

    def to_jsonl(self, path: str | Path) -> None:
        """Write all records to a JSON-lines file (raw payload as hex)."""
        p = Path(path)
        with p.open("w", encoding="utf-8") as fh:
            for rec in self.records:
                d = asdict(rec)
                d["raw"] = rec.raw.hex()
                fh.write(json.dumps(d) + "\n")

    def __len__(self) -> int:
        return len(self.records)
