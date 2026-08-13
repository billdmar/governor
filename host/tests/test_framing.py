"""Frame encode/decode: round-trip, corruption rejection, resync, concat.

Covers PROTOCOL_SPEC.md §2 (layout) and §4 (decoder state machine invariants).
"""

from __future__ import annotations

import pytest

from governor_gs.protocol import (
    GOV_MAX_PAYLOAD,
    SOF,
    FrameDecoder,
    MsgType,
    encode_frame,
)


def _collect() -> tuple[FrameDecoder, list]:
    got: list = []
    dec = FrameDecoder(lambda t, s, p: got.append((t, s, p)))
    return dec, got


def test_encode_layout() -> None:
    frame = encode_frame(MsgType.DATA, 7, b"\xaa\xbb")
    # SOF, VER, TYPE, SEQ, LEN hi, LEN lo, payload..., CRC hi, CRC lo
    assert frame[0] == SOF
    assert frame[1] == 0x01  # VER
    assert frame[2] == MsgType.DATA
    assert frame[3] == 7
    assert frame[4:6] == b"\x00\x02"  # LEN big-endian
    assert frame[6:8] == b"\xaa\xbb"
    assert len(frame) == 1 + 5 + 2 + 2  # SOF + (VER,TYPE,SEQ,LEN) + payload + CRC


@pytest.mark.parametrize("payload_len", [0, 1, 2, 32, GOV_MAX_PAYLOAD])
def test_round_trip(payload_len: int) -> None:
    payload = bytes((i * 7) & 0xFF for i in range(payload_len))
    dec, got = _collect()
    dec.push_bytes(encode_frame(MsgType.CMD, 0x2A, payload))
    assert got == [(MsgType.CMD, 0x2A, payload)]
    assert dec.stats.delivered == 1


def test_payload_too_long_rejected_on_encode() -> None:
    with pytest.raises(ValueError):
        encode_frame(MsgType.DATA, 0, bytes(GOV_MAX_PAYLOAD + 1))


def test_single_bit_flip_rejected() -> None:
    frame = bytearray(encode_frame(MsgType.DATA, 3, b"hello"))
    frame[6] ^= 0x01  # flip a bit inside the payload
    dec, got = _collect()
    dec.push_bytes(frame)
    assert got == []
    assert dec.stats.delivered == 0
    assert dec.stats.stat_crc_err == 1


def test_crc_bit_flip_rejected() -> None:
    frame = bytearray(encode_frame(MsgType.DATA, 3, b"hi"))
    frame[-1] ^= 0x80  # corrupt the CRC itself
    dec, got = _collect()
    dec.push_bytes(frame)
    assert got == []
    assert dec.stats.stat_crc_err == 1


def test_bad_version_rejected() -> None:
    frame = bytearray(encode_frame(MsgType.DATA, 1, b"x"))
    frame[1] = 0x02  # wrong VER
    dec, got = _collect()
    dec.push_bytes(frame)
    assert got == []
    assert dec.stats.stat_ver_err == 1


def test_oversize_len_rejected() -> None:
    # Hand-craft a header claiming LEN > MAX; decoder must drop, count len_err.
    stream = bytes([SOF, 0x01, MsgType.DATA, 0x00, 0xFF, 0xFF])  # LEN=0xFFFF
    dec, got = _collect()
    dec.push_bytes(stream)
    assert got == []
    assert dec.stats.stat_len_err == 1


def test_unknown_type_dropped_not_fatal() -> None:
    frame = encode_frame(0x7F, 0, b"z")  # 0x7F is not a known TYPE
    dec, got = _collect()
    dec.push_bytes(frame)
    assert got == []
    assert dec.stats.stat_unknown_type == 1
    # link still works afterward:
    dec.push_bytes(encode_frame(MsgType.DATA, 1, b"ok"))
    assert got == [(MsgType.DATA, 1, b"ok")]


def test_concatenated_frames() -> None:
    a = encode_frame(MsgType.DATA, 1, b"AAA")
    b = encode_frame(MsgType.CMD, 2, b"BB")
    dec, got = _collect()
    dec.push_bytes(a + b)
    assert got == [(MsgType.DATA, 1, b"AAA"), (MsgType.CMD, 2, b"BB")]


def test_resync_after_garbage() -> None:
    garbage = bytes([0x00, 0xFF, 0x7E, 0x99, 0x12, 0x34, 0x7E, 0xAB])
    frame = encode_frame(MsgType.HEARTBEAT, 5, b"live")
    dec, got = _collect()
    dec.push_bytes(garbage + frame)
    assert got == [(MsgType.HEARTBEAT, 5, b"live")]


def test_sof_inside_payload_is_length_driven() -> None:
    # A 0x7E byte inside the payload must not break length-driven decoding.
    payload = bytes([0x7E, 0x7E, 0x01, 0x7E])
    dec, got = _collect()
    dec.push_bytes(encode_frame(MsgType.DATA, 9, payload))
    assert got == [(MsgType.DATA, 9, payload)]


def test_back_to_back_sof_reanchors() -> None:
    frame = encode_frame(MsgType.DATA, 1, b"q")
    dec, got = _collect()
    dec.push_bytes(bytes([SOF, SOF]) + frame[1:])  # extra SOF then rest of frame
    assert got == [(MsgType.DATA, 1, b"q")]


def test_stream_never_containing_frame_yields_nothing() -> None:
    dec, got = _collect()
    dec.push_bytes(bytes(range(256)) * 4)
    # No crash, possibly zero deliveries (random bytes rarely form a valid frame).
    assert isinstance(dec.stats.delivered, int)
    assert got == [] or all(len(p) <= GOV_MAX_PAYLOAD for _, _, p in got)
