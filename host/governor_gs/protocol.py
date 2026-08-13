"""protocol.py — host mirror of the governor link layer (PROTOCOL_SPEC.md, FROZEN).

Byte-for-byte compatible with the target-side C implementation:

* Frame layout (big-endian): ``SOF(0x7E) VER(0x01) TYPE SEQ LEN(u16) PAYLOAD CRC16(u16)``.
* CRC-16/CCITT-FALSE (poly ``0x1021``, init ``0xFFFF``, no reflect, xorout ``0``)
  computed over ``VER,TYPE,SEQ,LEN,PAYLOAD`` — **not** the SOF.
* Byte-at-a-time decoder state machine that never blocks and never allocates
  unboundedly (a single fixed-size payload buffer), with SOF resync.
* Stop-and-wait ARQ (window = 1), retransmit on ACK timeout up to
  ``GOV_MAX_RETRIES``, then a ``LINK_FAULT`` indication.
* Receiver de-duplication: a reliable frame whose SEQ equals the last accepted
  SEQ is re-ACKed but delivered only once.

This is the ground-station endpoint of the link; the target implements the exact
mirror in ``lib/proto``. The wire format here IS the cross-check that both sides
agree — do not diverge from PROTOCOL_SPEC.md.
"""

from __future__ import annotations

import enum
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Deque, Optional

# --------------------------------------------------------------------------- #
# Frozen wire constants (PROTOCOL_SPEC.md §2, config/registry.md §1)
# --------------------------------------------------------------------------- #
SOF: int = 0x7E
"""Start-of-frame sentinel / resync anchor."""

VER: int = 0x01
"""Protocol version. The decoder rejects any other version."""

GOV_MAX_PAYLOAD: int = 64
"""Max payload length (bytes). Bounds the static decode buffer."""

#: Fixed header size (VER, TYPE, SEQ, LEN) after the SOF, excluding CRC.
_HEADER_AFTER_SOF: int = 4
#: Whole-frame maximum incl. SOF and CRC (== GOV_FRAME_MAX in the spec, 71).
GOV_FRAME_MAX: int = 1 + 1 + 1 + 1 + 2 + GOV_MAX_PAYLOAD + 2

# Reliability bounds (config/registry.md §1) — NEVER widened to pass a test.
GOV_ACK_TIMEOUT_MS: int = 100
"""Stop-and-wait ARQ retransmit timeout."""

GOV_MAX_RETRIES: int = 3
"""Retransmits attempted before raising LINK_FAULT."""


class MsgType(enum.IntEnum):
    """Message type byte (PROTOCOL_SPEC.md §3)."""

    DATA = 0x01
    ACK = 0x02
    NAK = 0x03
    CMD = 0x04
    HEARTBEAT = 0x05
    CFG_WRITE = 0x06


#: The reliable message types: they carry SEQ, expect an ACK, and are de-duped.
RELIABLE_TYPES: frozenset[int] = frozenset(
    {MsgType.DATA, MsgType.CMD, MsgType.HEARTBEAT, MsgType.CFG_WRITE}
)


# --------------------------------------------------------------------------- #
# CRC-16/CCITT-FALSE
# --------------------------------------------------------------------------- #
def crc16_ccitt(data: bytes, seed: int = 0xFFFF) -> int:
    """Compute CRC-16/CCITT-FALSE over ``data``.

    Parameters mirror the C ``gov_crc16_ccitt(seed, data, len)``:
    polynomial ``0x1021``, initial value ``0xFFFF`` (the ``seed``), no input or
    output reflection, and no final XOR. This is the same variant Zephyr's
    ``crc16_ccitt`` family uses, so host and target agree without a bespoke table.

    Args:
        data: Bytes to checksum.
        seed: Initial CRC register value (default ``0xFFFF``).

    Returns:
        The 16-bit CRC as an ``int`` in ``[0, 0xFFFF]``.
    """
    crc = seed & 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


# --------------------------------------------------------------------------- #
# Frame encoder
# --------------------------------------------------------------------------- #
def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    """Encode one frame to its on-the-wire byte string.

    Mirror of the C ``gov_frame_encode``. All multi-byte fields are big-endian.
    The CRC covers ``VER,TYPE,SEQ,LEN,PAYLOAD`` (not the SOF).

    Args:
        msg_type: Message type byte (see :class:`MsgType`), ``0..255``.
        seq: Sequence number, ``0..255`` (wraps mod 256).
        payload: Payload bytes, ``0..GOV_MAX_PAYLOAD``.

    Returns:
        The complete frame including SOF and trailing CRC.

    Raises:
        ValueError: If a field is out of range or the payload is too long.
    """
    if not 0 <= msg_type <= 0xFF:
        raise ValueError(f"msg_type out of range: {msg_type}")
    if not 0 <= seq <= 0xFF:
        raise ValueError(f"seq out of range: {seq}")
    if len(payload) > GOV_MAX_PAYLOAD:
        raise ValueError(
            f"payload {len(payload)} > GOV_MAX_PAYLOAD ({GOV_MAX_PAYLOAD})"
        )

    length = len(payload)
    # CRC-covered region: VER, TYPE, SEQ, LEN(hi,lo), PAYLOAD.
    covered = bytes(
        [VER, msg_type & 0xFF, seq & 0xFF, (length >> 8) & 0xFF, length & 0xFF]
    ) + payload
    crc = crc16_ccitt(covered)
    return bytes([SOF]) + covered + bytes([(crc >> 8) & 0xFF, crc & 0xFF])


# --------------------------------------------------------------------------- #
# Byte-at-a-time decoder (PROTOCOL_SPEC.md §4)
# --------------------------------------------------------------------------- #
class _DecodeState(enum.Enum):
    HUNT_SOF = enum.auto()
    GOT_SOF = enum.auto()  # waiting for VER
    HDR = enum.auto()  # accumulating TYPE, SEQ, LEN
    PAYLOAD = enum.auto()  # accumulating LEN payload bytes
    CRC = enum.auto()  # accumulating the 2 CRC bytes


@dataclass
class DecoderStats:
    """Decoder counters (mirror the C ``stat_*`` fields)."""

    delivered: int = 0
    stat_crc_err: int = 0
    stat_len_err: int = 0
    stat_ver_err: int = 0
    stat_unknown_type: int = 0


#: Delivery callback: ``(msg_type, seq, payload) -> None``.
FrameCallback = Callable[[int, int, bytes], None]


class FrameDecoder:
    """Streaming frame decoder fed one byte at a time.

    Implements the exact state machine of PROTOCOL_SPEC.md §4. It never blocks
    and uses a single fixed-size payload buffer (no unbounded allocation). On a
    complete, CRC-valid frame of a known type it invokes ``callback``; unknown
    types with a valid CRC are dropped and counted (``stat_unknown_type``), which
    is not a link-resetting error.

    Resync note: while waiting for the VER byte, a fresh ``0x7E`` re-anchors the
    SOF (back-to-back SOFs), and any header/CRC failure returns to ``HUNT_SOF`` so
    the next real SOF re-syncs. This is the length-driven resync the spec §2/§4
    describes; a false SOF is ultimately caught by the CRC and length checks.
    """

    def __init__(self, callback: Optional[FrameCallback] = None) -> None:
        """Create a decoder.

        Args:
            callback: Invoked as ``callback(msg_type, seq, payload)`` on each
                good frame. May be ``None`` (frames are still counted).
        """
        self._cb = callback
        self.stats = DecoderStats()
        self._buf = bytearray(GOV_MAX_PAYLOAD)  # fixed static payload buffer
        self._reset_hunt()

    def _reset_hunt(self) -> None:
        self._state = _DecodeState.HUNT_SOF
        self._type = 0
        self._seq = 0
        self._len = 0
        self._hdr_bytes = bytearray()
        self._payload_got = 0
        self._crc_bytes = bytearray()

    def push(self, byte: int) -> None:
        """Feed one byte (``0..255``) into the decoder. Never blocks/allocates."""
        byte &= 0xFF
        state = self._state

        if state is _DecodeState.HUNT_SOF:
            if byte == SOF:
                self._state = _DecodeState.GOT_SOF
            # else: stay hunting.
            return

        if state is _DecodeState.GOT_SOF:
            if byte == VER:
                self._state = _DecodeState.HDR
                self._hdr_bytes = bytearray()
            elif byte == SOF:
                # Back-to-back SOF: re-anchor, keep waiting for VER.
                pass
            else:
                self.stats.stat_ver_err += 1
                self._reset_hunt()
            return

        if state is _DecodeState.HDR:
            self._hdr_bytes.append(byte)
            if len(self._hdr_bytes) == _HEADER_AFTER_SOF:  # TYPE, SEQ, LEN hi/lo
                self._type = self._hdr_bytes[0]
                self._seq = self._hdr_bytes[1]
                self._len = (self._hdr_bytes[2] << 8) | self._hdr_bytes[3]
                if self._len > GOV_MAX_PAYLOAD:
                    self.stats.stat_len_err += 1
                    self._reset_hunt()
                elif self._len == 0:
                    self._state = _DecodeState.CRC
                    self._crc_bytes = bytearray()
                else:
                    self._state = _DecodeState.PAYLOAD
                    self._payload_got = 0
            return

        if state is _DecodeState.PAYLOAD:
            self._buf[self._payload_got] = byte
            self._payload_got += 1
            if self._payload_got == self._len:
                self._state = _DecodeState.CRC
                self._crc_bytes = bytearray()
            return

        if state is _DecodeState.CRC:
            self._crc_bytes.append(byte)
            if len(self._crc_bytes) == 2:
                self._finish_frame()
            return

    def _finish_frame(self) -> None:
        rx_crc = (self._crc_bytes[0] << 8) | self._crc_bytes[1]
        payload = bytes(self._buf[: self._len])
        covered = bytes(
            [
                VER,
                self._type,
                self._seq,
                (self._len >> 8) & 0xFF,
                self._len & 0xFF,
            ]
        ) + payload
        calc = crc16_ccitt(covered)
        if calc != rx_crc:
            self.stats.stat_crc_err += 1
            self._reset_hunt()
            return
        # CRC good.
        if self._type not in {t.value for t in MsgType}:
            self.stats.stat_unknown_type += 1
            self._reset_hunt()
            return
        self.stats.delivered += 1
        msg_type, seq = self._type, self._seq
        self._reset_hunt()
        if self._cb is not None:
            self._cb(msg_type, seq, payload)

    def push_bytes(self, data: bytes) -> None:
        """Convenience: feed a whole byte string, one byte at a time."""
        for b in data:
            self.push(b)


# --------------------------------------------------------------------------- #
# Stop-and-wait ARQ + dedup (PROTOCOL_SPEC.md §5)
# --------------------------------------------------------------------------- #
@dataclass
class _Outstanding:
    """The single in-flight reliable frame (window = 1)."""

    seq: int
    frame: bytes
    sent_at_ms: int
    retries: int = 0  # retransmits performed so far


@dataclass
class ReliableEndpoint:
    """Stop-and-wait ARQ endpoint with de-duplication.

    Wraps the encoder/decoder and drives the reliability layer of
    PROTOCOL_SPEC.md §5. It is transport-agnostic: outbound bytes go to
    ``send_bytes`` and inbound bytes are handed in via :meth:`feed`. Time is
    injected (virtual-time friendly) via explicit ``now_ms`` arguments so tests
    are deterministic and do not depend on wall-clock behaviour.

    A single reliable frame is outstanding at a time (window = 1). Additional
    reliable sends queue in ``_pending`` (mirrors the target ``tx_frameq``) and
    are released as ACKs arrive.

    Callbacks:
        on_deliver(msg_type, seq, payload): a *new* (non-duplicate) reliable
            frame or any non-reliable app frame was received.
        on_link_fault(seq): retransmits for ``seq`` were exhausted
            (``GOV_MAX_RETRIES``) — the caller raises FAULT_LINK to safety.
    """

    send_bytes: Callable[[bytes], None]
    on_deliver: Optional[FrameCallback] = None
    on_link_fault: Optional[Callable[[int], None]] = None

    # --- internal state (not constructor args) ---
    _tx_seq: int = field(default=0, init=False)
    _outstanding: Optional[_Outstanding] = field(default=None, init=False)
    _pending: Deque[tuple[int, bytes]] = field(default_factory=deque, init=False)
    _last_rx_seq: Optional[int] = field(default=None, init=False)
    _decoder: FrameDecoder = field(init=False)

    # counters
    dup_count: int = field(default=0, init=False)
    retransmit_count: int = field(default=0, init=False)
    link_fault_count: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        self._decoder = FrameDecoder(self._on_frame)

    # ------------------------------------------------------------------ #
    # Outbound
    # ------------------------------------------------------------------ #
    def send(self, msg_type: int, payload: bytes = b"", now_ms: int = 0) -> None:
        """Send an application frame.

        Reliable types (DATA/CMD/HEARTBEAT/CFG_WRITE) are sequenced, tracked for
        ACK, and retransmitted on timeout. Non-reliable types (ACK/NAK) are
        transmitted immediately without sequencing.

        Args:
            msg_type: Message type byte.
            payload: Payload bytes (``<= GOV_MAX_PAYLOAD``).
            now_ms: Current virtual time in ms (used to arm the ARQ timer).
        """
        if msg_type in RELIABLE_TYPES:
            self._pending.append((msg_type, payload))
            self._pump(now_ms)
        else:
            self.send_bytes(encode_frame(msg_type, 0, payload))

    def _pump(self, now_ms: int) -> None:
        """If the link is free and work is queued, transmit the next frame."""
        if self._outstanding is not None or not self._pending:
            return
        msg_type, payload = self._pending.popleft()
        seq = self._tx_seq
        self._tx_seq = (self._tx_seq + 1) & 0xFF
        frame = encode_frame(msg_type, seq, payload)
        self._outstanding = _Outstanding(seq=seq, frame=frame, sent_at_ms=now_ms)
        self.send_bytes(frame)

    def tick(self, now_ms: int) -> None:
        """Advance the ARQ timer to ``now_ms`` and retransmit / fault as needed.

        If the outstanding frame has gone un-ACKed for ``GOV_ACK_TIMEOUT_MS``,
        retransmit the identical frame (same SEQ). After ``GOV_MAX_RETRIES``
        retransmits with still no ACK, drop the frame and fire ``on_link_fault``.
        """
        out = self._outstanding
        if out is None:
            return
        if now_ms - out.sent_at_ms < GOV_ACK_TIMEOUT_MS:
            return
        if out.retries < GOV_MAX_RETRIES:
            out.retries += 1
            out.sent_at_ms = now_ms
            self.retransmit_count += 1
            self.send_bytes(out.frame)
        else:
            failed_seq = out.seq
            self._outstanding = None
            self.link_fault_count += 1
            if self.on_link_fault is not None:
                self.on_link_fault(failed_seq)
            # Do not auto-advance pending: the caller decides (link is degraded).

    @property
    def outstanding_seq(self) -> Optional[int]:
        """SEQ of the in-flight reliable frame, or ``None`` if the link is idle."""
        return None if self._outstanding is None else self._outstanding.seq

    # ------------------------------------------------------------------ #
    # Inbound
    # ------------------------------------------------------------------ #
    def feed(self, data: bytes, now_ms: int = 0) -> None:
        """Feed received bytes from the transport into the decoder."""
        self._now_ms = now_ms
        self._decoder.push_bytes(data)

    def _on_frame(self, msg_type: int, seq: int, payload: bytes) -> None:
        now_ms = getattr(self, "_now_ms", 0)
        if msg_type == MsgType.ACK:
            acked = payload[0] if payload else -1
            if self._outstanding is not None and self._outstanding.seq == acked:
                self._outstanding = None
                self._pump(now_ms)
            return
        if msg_type == MsgType.NAK:
            # Optional fast retransmit: resend the outstanding frame if it matches.
            want = payload[0] if payload else -1
            if self._outstanding is not None and self._outstanding.seq == want:
                self._outstanding.sent_at_ms = now_ms
                self.retransmit_count += 1
                self.send_bytes(self._outstanding.frame)
            return

        # Reliable inbound frame: dedup, then ACK (always), deliver only if new.
        is_dup = self._last_rx_seq is not None and seq == self._last_rx_seq
        # Re-ACK on every reliable frame, including duplicates.
        self.send_bytes(encode_frame(MsgType.ACK, 0, bytes([seq])))
        if is_dup:
            self.dup_count += 1
            return
        self._last_rx_seq = seq
        if self.on_deliver is not None:
            self.on_deliver(msg_type, seq, payload)

    @property
    def stats(self) -> DecoderStats:
        """Decoder-level counters (CRC/len/ver/unknown/delivered)."""
        return self._decoder.stats
