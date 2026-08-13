"""CRC-16/CCITT-FALSE known-answer vectors — must match the C side exactly.

Variant (PROTOCOL_SPEC.md §6): poly 0x1021, init 0xFFFF, no reflect in/out,
xorout 0x0000. The canonical "check" value for the ASCII string "123456789" of
this variant is 0x29B1; the C ``gov_crc16_ccitt`` MUST produce these same values.
"""

from __future__ import annotations

import pytest

from governor_gs.protocol import crc16_ccitt

# (input bytes, expected CRC) — the vectors  cross-checks against lib/proto.
KNOWN_ANSWERS = [
    (b"123456789", 0x29B1),  # canonical CCITT-FALSE check value
    (b"", 0xFFFF),           # empty input == the seed
    (b"A", 0xB915),
    (b"\x00", 0xE1F0),
    (bytes([0xFF] * 4), 0x1D0F),
    (bytes(range(16)), 0x3B37),
]


@pytest.mark.parametrize("data, expected", KNOWN_ANSWERS)
def test_crc_known_answers(data: bytes, expected: int) -> None:
    assert crc16_ccitt(data) == expected


def test_crc_seed_is_ffff_default() -> None:
    assert crc16_ccitt(b"") == 0xFFFF


def test_crc_is_order_sensitive() -> None:
    assert crc16_ccitt(b"AB") != crc16_ccitt(b"BA")
