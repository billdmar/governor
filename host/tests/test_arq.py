"""Stop-and-wait ARQ + de-duplication (PROTOCOL_SPEC.md §5).

Uses two ReliableEndpoints over a deterministic in-process wiring so both the
sender's retransmit/fault behaviour and the receiver's dedup can be asserted in
virtual time.
"""

from __future__ import annotations

from governor_gs.protocol import (
    GOV_ACK_TIMEOUT_MS,
    GOV_MAX_RETRIES,
    FrameDecoder,
    MsgType,
    ReliableEndpoint,
    encode_frame,
)


def test_dedup_duplicate_delivered_once() -> None:
    """A retransmitted (identical SEQ) reliable frame is ACKed but delivered once."""
    delivered: list = []
    acks: list = []
    rx = ReliableEndpoint(
        send_bytes=lambda b: acks.append(b),  # capture the ACK it sends back
        on_deliver=lambda t, s, p: delivered.append((t, s, p)),
    )
    frame = encode_frame(MsgType.DATA, 42, b"payload")
    rx.feed(frame)
    rx.feed(frame)  # duplicate retransmission

    assert delivered == [(MsgType.DATA, 42, b"payload")]  # delivered exactly once
    assert rx.dup_count == 1
    assert len(acks) == 2  # both the original and the duplicate get ACKed


def test_first_frame_after_reset_accepts_any_seq() -> None:
    delivered: list = []
    rx = ReliableEndpoint(send_bytes=lambda b: None,
                          on_deliver=lambda t, s, p: delivered.append(s))
    rx.feed(encode_frame(MsgType.DATA, 200, b"x"))  # arbitrary first SEQ
    assert delivered == [200]


def test_ack_releases_outstanding_and_pumps_next() -> None:
    wire: list = []
    tx = ReliableEndpoint(send_bytes=lambda b: wire.append(b))
    tx.send(MsgType.DATA, b"one", now_ms=0)
    tx.send(MsgType.DATA, b"two", now_ms=0)  # queued behind window=1
    assert tx.outstanding_seq == 0
    assert len(wire) == 1  # only the first is on the wire

    # Deliver an ACK for seq 0 -> releases, pumps seq 1.
    tx.feed(encode_frame(MsgType.ACK, 0, bytes([0])), now_ms=1)
    assert tx.outstanding_seq == 1
    assert len(wire) == 2


def test_retransmit_on_ack_timeout() -> None:
    wire: list = []
    tx = ReliableEndpoint(send_bytes=lambda b: wire.append(b))
    tx.send(MsgType.DATA, b"data", now_ms=0)
    first = wire[0]

    # Before timeout: no retransmit.
    tx.tick(now_ms=GOV_ACK_TIMEOUT_MS - 1)
    assert tx.retransmit_count == 0

    # At timeout: identical frame (same SEQ) retransmitted.
    tx.tick(now_ms=GOV_ACK_TIMEOUT_MS)
    assert tx.retransmit_count == 1
    assert wire[-1] == first  # byte-for-byte identical retransmit


def test_ack_loss_triggers_link_fault_after_max_retries() -> None:
    wire: list = []
    faults: list = []
    tx = ReliableEndpoint(
        send_bytes=lambda b: wire.append(b),
        on_link_fault=lambda seq: faults.append(seq),
    )
    tx.send(MsgType.DATA, b"lost", now_ms=0)

    # Never ACK. Each timeout window either retransmits or (finally) faults.
    t = 0
    for _ in range(GOV_MAX_RETRIES + 1):
        t += GOV_ACK_TIMEOUT_MS
        tx.tick(now_ms=t)

    assert tx.retransmit_count == GOV_MAX_RETRIES  # exactly 3 retransmits
    assert faults == [0]  # then LINK_FAULT for that SEQ
    assert tx.link_fault_count == 1
    assert tx.outstanding_seq is None  # frame dropped, link degraded


def test_seq_increments_and_wraps() -> None:
    seqs: list = []
    dec = FrameDecoder(lambda t, s, p: seqs.append(s))
    tx = ReliableEndpoint(send_bytes=dec.push_bytes)
    # Send 258 reliable frames, ACKing each so the window frees.
    for i in range(258):
        tx.send(MsgType.DATA, bytes([i & 0xFF]), now_ms=i)
        tx.feed(encode_frame(MsgType.ACK, 0, bytes([tx.outstanding_seq])), now_ms=i)
    # SEQ wraps mod 256: frames 0..257 -> seq 0..255,0,1
    assert seqs[:3] == [0, 1, 2]
    assert seqs[255:258] == [255, 0, 1]
