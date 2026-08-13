"""scenario.py — transport abstraction + fault-injecting scenario runner.

The scenario runner connects the ground-station endpoint to a firmware endpoint
over an abstract byte-stream :class:`Transport`. The firmware side is wired by
 later (native_sim / socket), so the transport is deliberately abstract; two
concrete transports are provided here:

* :class:`LoopbackTransport` — an in-process, deterministic, virtual-time pair of
  endpoints (used by unit / end-to-end tests). No sockets, no threads.
* :class:`SocketTransport` — a TCP stream transport for talking to an external
  firmware endpoint (native_sim under a socket, a Renode UART bridge, etc.).

At the **link level** the runner can inject the registry faults (config/registry.md
§2, rows F06–F09, F13):

* corrupt a byte (single-bit flip)               → F06
* drop a frame / drop an ACK                      → F07
* duplicate a frame                               → F08
* inject a garbage burst before a frame           → F09
* send an operator emergency-stop ``CMD``         → F13

Results (deliveries, ACKs, retransmits, link faults, telemetry) are recorded via
:class:`~governor_gs.log_capture.LogCapture` and the endpoint counters so tests
can assert the Required Outcomes. Time is virtual and explicit.
"""

from __future__ import annotations

import abc
import socket
from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Optional

from .log_capture import LogCapture
from .protocol import MsgType, ReliableEndpoint, encode_frame

# Operator emergency-stop command opcode carried in a CMD payload (host<->node
# convention for F13; the target maps this to GOV_EV_OPERATOR_STOP / T8).
CMD_ESTOP: int = 0xE5
CMD_CLEAR: int = 0xC1
CMD_SET_SETPOINT: int = 0x50


# --------------------------------------------------------------------------- #
# Transport abstraction
# --------------------------------------------------------------------------- #
class Transport(abc.ABC):
    """Abstract full-duplex byte-stream transport between two endpoints.

    Implementations move raw bytes; they know nothing about frames. ``send``
    pushes bytes toward the peer; ``recv`` returns any bytes that have arrived
    (non-blocking, ``b""`` if none).
    """

    @abc.abstractmethod
    def send(self, data: bytes) -> None:
        """Transmit ``data`` toward the peer."""

    @abc.abstractmethod
    def recv(self) -> bytes:
        """Return bytes received from the peer (``b""`` if none pending)."""

    def close(self) -> None:  # noqa: B027 - optional override
        """Release any resources. Default is a no-op."""


class _LoopSide(Transport):
    """One direction of a :class:`LoopbackTransport` (an in-process pipe)."""

    def __init__(self, outbound: Deque[int], inbound: Deque[int]) -> None:
        self._out = outbound
        self._in = inbound

    def send(self, data: bytes) -> None:
        self._out.extend(data)

    def recv(self) -> bytes:
        out = bytes(self._in)
        self._in.clear()
        return out


class LoopbackTransport:
    """In-process, deterministic transport pair.

    ``a`` and ``b`` are the two :class:`Transport` endpoints; bytes sent on ``a``
    appear on ``b.recv()`` and vice-versa. No sockets or threads, so tests are
    fully deterministic and virtual-time driven.
    """

    def __init__(self) -> None:
        self._a2b: Deque[int] = deque()
        self._b2a: Deque[int] = deque()
        self.a: Transport = _LoopSide(self._a2b, self._b2a)
        self.b: Transport = _LoopSide(self._b2a, self._a2b)


class SocketTransport(Transport):
    """TCP stream transport to an external firmware endpoint.

    Non-blocking: :meth:`recv` drains whatever is available without blocking.
    Intended for  to point at a native_sim socket or a Renode UART bridge.
    """

    def __init__(self, sock: socket.socket) -> None:
        """Wrap an already-connected socket."""
        self._sock = sock
        self._sock.setblocking(False)

    @classmethod
    def connect(cls, host: str, port: int, timeout: float = 5.0) -> "SocketTransport":
        """Open a blocking TCP connection, then switch to non-blocking."""
        sock = socket.create_connection((host, port), timeout=timeout)
        return cls(sock)

    def send(self, data: bytes) -> None:
        self._sock.sendall(data)

    def recv(self) -> bytes:
        chunks: list[bytes] = []
        try:
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
        except (BlockingIOError, InterruptedError):
            pass
        return b"".join(chunks)

    def close(self) -> None:
        self._sock.close()


# --------------------------------------------------------------------------- #
# Link-level fault injection
# --------------------------------------------------------------------------- #
@dataclass
class FaultConfig:
    """One-shot / counted link fault knobs (config/registry.md §2, F06–F09).

    Attributes:
        corrupt_next: Flip one bit in the next N frames sent (F06).
        drop_next: Drop the next N frames entirely (F07: frame/ACK loss).
        duplicate_next: Send the next N frames twice (F08).
        garbage_prefix: Bytes to prepend before the next frame (F09 resync).
    """

    corrupt_next: int = 0
    drop_next: int = 0
    duplicate_next: int = 0
    garbage_prefix: bytes = b""


class FaultInjector:
    """Wraps a :class:`Transport` and mangles the outbound byte stream.

    It operates on whole frames when possible (it buffers the caller's writes and
    assumes each ``send`` from the endpoint is one frame, which the protocol layer
    guarantees — encoder emits one complete frame per call). Faults are consumed
    as configured, so a scenario can e.g. "drop the next ACK" then behave normally.
    """

    def __init__(self, inner: Transport, cfg: Optional[FaultConfig] = None) -> None:
        self.inner = inner
        self.cfg = cfg or FaultConfig()
        self.dropped = 0
        self.corrupted = 0
        self.duplicated = 0

    def send(self, data: bytes) -> None:
        """Apply configured faults to one frame's bytes, then forward."""
        if self.cfg.garbage_prefix:
            self.inner.send(self.cfg.garbage_prefix)
            self.cfg.garbage_prefix = b""

        if self.cfg.drop_next > 0:
            self.cfg.drop_next -= 1
            self.dropped += 1
            return  # frame vanishes

        out = bytearray(data)
        if self.cfg.corrupt_next > 0 and out:
            self.cfg.corrupt_next -= 1
            self.corrupted += 1
            # Flip a bit in a payload/CRC-covered byte (index 1 = VER..; pick a
            # middle byte so a header/CRC mismatch is provoked, never the SOF).
            idx = len(out) // 2
            out[idx] ^= 0x01

        self.inner.send(bytes(out))

        if self.cfg.duplicate_next > 0:
            self.cfg.duplicate_next -= 1
            self.duplicated += 1
            self.inner.send(bytes(out))

    def recv(self) -> bytes:
        return self.inner.recv()

    def close(self) -> None:
        self.inner.close()


# --------------------------------------------------------------------------- #
# Scenario runner
# --------------------------------------------------------------------------- #
@dataclass
class ScenarioRunner:
    """Drives a ground-station :class:`ReliableEndpoint` over a transport.

    It pumps bytes between the transport and the endpoint, advances virtual time,
    captures telemetry, and exposes helpers to inject the registry link faults.

    Attributes:
        transport: The (optionally fault-wrapped) byte-stream transport.
        log: Telemetry capture sink.
        endpoint: The ground-station reliable endpoint.
        now_ms: Current virtual time.
    """

    transport: Transport
    log: LogCapture = field(default_factory=LogCapture)
    now_ms: int = 0

    endpoint: ReliableEndpoint = field(init=False)
    delivered: list[tuple[int, int, bytes]] = field(default_factory=list, init=False)
    link_faults: list[int] = field(default_factory=list, init=False)

    def __post_init__(self) -> None:
        self.endpoint = ReliableEndpoint(
            send_bytes=self.transport.send,
            on_deliver=self._on_deliver,
            on_link_fault=self._on_link_fault,
        )

    def _on_deliver(self, msg_type: int, seq: int, payload: bytes) -> None:
        self.delivered.append((msg_type, seq, payload))
        if msg_type == MsgType.HEARTBEAT:
            self.log.capture_heartbeat(seq, payload, self.now_ms)
        elif msg_type == MsgType.DATA:
            self.log.capture_data(seq, payload, self.now_ms)

    def _on_link_fault(self, seq: int) -> None:
        self.link_faults.append(seq)

    # -- pumping / time ------------------------------------------------- #
    def pump(self) -> None:
        """Drain the transport into the endpoint decoder once."""
        data = self.transport.recv()
        if data:
            self.endpoint.feed(data, self.now_ms)

    def advance(self, dt_ms: int) -> None:
        """Advance virtual time by ``dt_ms`` and service ARQ + inbound bytes."""
        self.now_ms += dt_ms
        self.endpoint.tick(self.now_ms)
        self.pump()

    # -- operator / app sends ------------------------------------------- #
    def send_cmd(self, opcode: int, args: bytes = b"") -> None:
        """Send a reliable operator CMD (opcode byte + optional args)."""
        self.endpoint.send(MsgType.CMD, bytes([opcode]) + args, self.now_ms)

    def send_emergency_stop(self) -> None:
        """Inject the operator emergency-stop command (F13 → T8/SAFE_STOP)."""
        self.send_cmd(CMD_ESTOP)

    def send_data(self, payload: bytes) -> None:
        """Send a reliable DATA frame."""
        self.endpoint.send(MsgType.DATA, payload, self.now_ms)
