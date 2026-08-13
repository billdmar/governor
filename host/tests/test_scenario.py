"""End-to-end loopback + link-level fault injection (registry F06-F09, F13).

Two ReliableEndpoints (a "node" and a "ground station") talk over the in-process
LoopbackTransport, exchanging DATA+ACK and a CMD, with telemetry captured. Then
the FaultInjector exercises the link-level registry faults and asserts the
protocol's Required Outcomes.
"""

from __future__ import annotations

from governor_gs.log_capture import LogCapture, SafetyState
from governor_gs.protocol import MsgType, ReliableEndpoint, encode_frame
from governor_gs.scenario import (
    CMD_ESTOP,
    FaultConfig,
    FaultInjector,
    LoopbackTransport,
    ScenarioRunner,
)


def _wire_pair(loop: LoopbackTransport):
    """Build node+gs endpoints whose send routes onto the loopback transport."""
    node_deliv: list = []
    gs_deliv: list = []
    node = ReliableEndpoint(send_bytes=loop.a.send,
                            on_deliver=lambda t, s, p: node_deliv.append((t, s, p)))
    gs = ReliableEndpoint(send_bytes=loop.b.send,
                          on_deliver=lambda t, s, p: gs_deliv.append((t, s, p)))
    return node, gs, node_deliv, gs_deliv


def _pump(loop: LoopbackTransport, node: ReliableEndpoint, gs: ReliableEndpoint,
          now_ms: int, rounds: int = 4) -> None:
    """Shuttle bytes both directions until quiescent."""
    for _ in range(rounds):
        node.feed(loop.a.recv(), now_ms)
        gs.feed(loop.b.recv(), now_ms)


def test_loopback_data_ack_and_cmd() -> None:
    loop = LoopbackTransport()
    node, gs, node_deliv, gs_deliv = _wire_pair(loop)

    # node -> gs: a DATA telemetry frame; gs auto-ACKs on delivery.
    node.send(MsgType.DATA, b"telemetry", now_ms=0)
    _pump(loop, node, gs, now_ms=1)
    assert gs_deliv == [(MsgType.DATA, 0, b"telemetry")]
    # The ACK must have freed the node's window.
    assert node.outstanding_seq is None

    # gs -> node: a CMD; node auto-ACKs.
    gs.send(MsgType.CMD, b"\x50\x00\x64", now_ms=2)  # set-setpoint style
    _pump(loop, node, gs, now_ms=3)
    assert node_deliv == [(MsgType.CMD, 0, b"\x50\x00\x64")]
    assert gs.outstanding_seq is None


def test_loopback_heartbeat_captured() -> None:
    loop = LoopbackTransport()
    node, gs, _, _ = _wire_pair(loop)
    log = LogCapture()

    def gs_deliver(t: int, s: int, p: bytes) -> None:
        if t == MsgType.HEARTBEAT:
            log.capture_heartbeat(s, p, recv_ms=10)

    gs.on_deliver = gs_deliver
    # Heartbeat payload: state=RUN, fault flags=0.
    hb = bytes([SafetyState.RUN]) + (0).to_bytes(4, "big")
    node.send(MsgType.HEARTBEAT, hb, now_ms=0)
    node.feed(loop.a.recv(), 0)
    gs.feed(loop.b.recv(), 10)

    assert len(log) == 1
    assert log.last_state() == SafetyState.RUN
    assert not log.saw_fault_bit(0x1)


def test_scenario_runner_estop_and_capture(tmp_path) -> None:
    loop = LoopbackTransport()
    runner = ScenarioRunner(transport=loop.b)

    # A minimal "node" on the other side that ACKs reliable frames and would
    # act on the estop CMD.
    node_rx: list = []
    node = ReliableEndpoint(send_bytes=loop.a.send,
                            on_deliver=lambda t, s, p: node_rx.append((t, s, p)))

    runner.send_emergency_stop()          # F13: operator estop CMD
    node.feed(loop.a.recv(), 0)           # node receives + ACKs
    runner.advance(1)                     # gs ingests the ACK

    assert node_rx == [(MsgType.CMD, 0, bytes([CMD_ESTOP]))]
    assert runner.endpoint.outstanding_seq is None  # ACK freed the window

    # Node reports it went SAFE_STOP via a heartbeat.
    hb = bytes([SafetyState.SAFE_STOP]) + (0x00000200).to_bytes(4, "big")
    node.send(MsgType.HEARTBEAT, hb, now_ms=1)
    runner.advance(1)
    assert runner.log.last_state() == SafetyState.SAFE_STOP
    assert runner.log.saw_fault_bit(0x00000200)

    # log serialisation round-trips to disk.
    out = tmp_path / "telem.jsonl"
    runner.log.to_jsonl(out)
    assert out.read_text().count("\n") == len(runner.log)


def test_fault_corrupt_byte_is_dropped_then_retransmit(tmp_path) -> None:
    """F06: single-bit corruption -> CRC drop -> retransmit -> clean delivery."""
    loop = LoopbackTransport()
    inj = FaultInjector(loop.a, FaultConfig(corrupt_next=1))
    node_deliv: list = []
    gs_deliv: list = []
    node = ReliableEndpoint(send_bytes=inj.send,
                            on_deliver=lambda t, s, p: node_deliv.append((t, s, p)))
    gs = ReliableEndpoint(send_bytes=loop.b.send,
                          on_deliver=lambda t, s, p: gs_deliv.append((t, s, p)))

    node.send(MsgType.DATA, b"corruptme", now_ms=0)
    gs.feed(loop.b.recv(), 0)          # gs sees a corrupt frame -> CRC drop
    assert gs_deliv == []
    assert gs.stats.stat_crc_err == 1

    # ARQ timeout -> retransmit (uncorrupted now) -> delivered exactly once.
    from governor_gs.protocol import GOV_ACK_TIMEOUT_MS
    node.tick(now_ms=GOV_ACK_TIMEOUT_MS)
    gs.feed(loop.b.recv(), GOV_ACK_TIMEOUT_MS)
    node.feed(loop.a.recv(), GOV_ACK_TIMEOUT_MS)  # gs's ACK frees the node
    assert gs_deliv == [(MsgType.DATA, 0, b"corruptme")]
    assert node.outstanding_seq is None
    assert inj.corrupted == 1


def test_fault_drop_ack_triggers_retransmit_then_success() -> None:
    """F07: dropped ACK -> node retransmits -> gs dedups -> node eventually ACKed."""
    from governor_gs.protocol import GOV_ACK_TIMEOUT_MS

    loop = LoopbackTransport()
    # Inject on the gs->node direction to drop the first ACK.
    inj_b = FaultInjector(loop.b, FaultConfig(drop_next=1))
    gs_deliv: list = []
    gs = ReliableEndpoint(send_bytes=inj_b.send,
                          on_deliver=lambda t, s, p: gs_deliv.append((t, s, p)))
    node = ReliableEndpoint(send_bytes=loop.a.send)

    node.send(MsgType.DATA, b"x", now_ms=0)
    gs.feed(loop.b.recv(), 0)          # gs delivers + tries to ACK (ACK dropped)
    node.feed(loop.a.recv(), 0)        # node gets nothing
    assert gs_deliv == [(MsgType.DATA, 0, b"x")]
    assert node.outstanding_seq == 0   # still waiting

    node.tick(now_ms=GOV_ACK_TIMEOUT_MS)   # retransmit
    gs.feed(loop.b.recv(), GOV_ACK_TIMEOUT_MS)   # dup -> re-ACK, no re-deliver
    node.feed(loop.a.recv(), GOV_ACK_TIMEOUT_MS)
    assert gs.dup_count == 1
    assert gs_deliv == [(MsgType.DATA, 0, b"x")]  # still delivered once
    assert node.outstanding_seq is None           # 2nd ACK got through
    assert inj_b.dropped == 1


def test_fault_duplicate_frame_deduped() -> None:
    """F08: duplicated frame on the wire -> dedup, single delivery."""
    loop = LoopbackTransport()
    inj = FaultInjector(loop.a, FaultConfig(duplicate_next=1))
    gs_deliv: list = []
    node = ReliableEndpoint(send_bytes=inj.send)
    gs = ReliableEndpoint(send_bytes=loop.b.send,
                          on_deliver=lambda t, s, p: gs_deliv.append((t, s, p)))
    node.send(MsgType.DATA, b"dup", now_ms=0)
    gs.feed(loop.b.recv(), 0)
    assert gs_deliv == [(MsgType.DATA, 0, b"dup")]
    assert gs.dup_count == 1
    assert inj.duplicated == 1


def test_fault_garbage_burst_then_resync() -> None:
    """F09: garbage burst before a valid frame -> decoder resyncs and delivers."""
    loop = LoopbackTransport()
    inj = FaultInjector(loop.a, FaultConfig(garbage_prefix=bytes([0xDE, 0xAD, 0xBE, 0xEF])))
    gs_deliv: list = []
    node = ReliableEndpoint(send_bytes=inj.send)
    gs = ReliableEndpoint(send_bytes=loop.b.send,
                          on_deliver=lambda t, s, p: gs_deliv.append((t, s, p)))
    node.send(MsgType.DATA, b"resync", now_ms=0)
    gs.feed(loop.b.recv(), 0)
    assert gs_deliv == [(MsgType.DATA, 0, b"resync")]
    assert gs.stats.delivered == 1
