# PROTOCOL_SPEC — governor link layer (FROZEN @ )

> **Contract status: FROZEN.** Changes go through  only (the design notes §4).
> `lib/proto/**` implements exactly this; `host/**` implements the mirror image;
> `fuzz/fuzz_frame.c` fuzzes the decoder defined here. Host-portable C — **no
> Zephyr includes**.

## 1. Purpose & scope
A reliable, framed, point-to-point link over a raw byte stream (UART on target,
a socket/pipe on the host harness). It provides: **framing** (find message
boundaries in a byte stream), **integrity** (detect corruption), **reliability**
(detect loss, retransmit), and **de-duplication** (idempotent delivery despite
retransmission). It is deliberately *not* a routing, fragmentation, or
flow-control-window protocol — one node talks to one ground station.

## 2. Frame layout (on the wire)
All multi-byte integer fields are **big-endian** (network order) — one fixed
convention removes an entire class of host/target mismatch bugs.

```
+--------+--------+--------+--------+--------+--------+= ... =+--------+--------+
|  SOF   |  VER   |  TYPE  |  SEQ   |  LEN (u16, BE) |  PAYLOAD (LEN bytes)  |  CRC16 (BE) |
| 0x7E   | 0x01   |  1 B   |  1 B   |     2 B         |     0..MAX_PAYLOAD    |    2 B      |
+--------+--------+--------+--------+--------+--------+= ... =+--------+--------+
   ^ frame start                                       ^ CRC covers VER..end of PAYLOAD
```

| Field   | Size | Meaning |
|---------|------|---------|
| SOF     | 1 B  | Start-of-frame sentinel `0x7E`. Resync anchor (see §4). |
| VER     | 1 B  | Protocol version. `0x01`. Decoder rejects unknown versions. |
| TYPE    | 1 B  | Message type (§3). |
| SEQ     | 1 B  | Sequence number, wraps mod 256 (§5). |
| LEN     | 2 B  | Payload length in bytes, `0 .. GOV_MAX_PAYLOAD (=64)`. |
| PAYLOAD | LEN  | Opaque to the link layer. |
| CRC16   | 2 B  | CRC-16/CCITT-FALSE over **VER, TYPE, SEQ, LEN, PAYLOAD** (not SOF). |

- **`GOV_MAX_PAYLOAD = 64`.** Justified: telemetry records and config blobs are
  small; a 64-byte cap bounds the static frame buffer. The full on-wire frame is
  `GOV_FRAME_MAX = 1 (SOF) + 1 (VER) + 1 (TYPE) + 1 (SEQ) + 2 (LEN) + 64
  (PAYLOAD) + 2 (CRC) = 72` bytes, so the whole parser lives in a single
  fixed-size static buffer — **no dynamic allocation**, DoS-bounded. (Corrected
  at : an earlier draft summed the six post-SOF fields to 71 and mislabelled it
  "incl. SOF"; the SOF byte makes it 72. `lib/proto` uses 72.)
- SOF is **not** escaped/byte-stuffed. A `0x7E` inside a payload is fine: the
  decoder is length-driven once a header is accepted, and a false SOF is caught
  by the CRC + length sanity (§4). This keeps the encoder allocation-free and
  branchless; the cost (rare resync on a corrupt header) is acceptable for a
  link with CRC.

## 3. Message types (`TYPE`)
| Value | Name | Dir | Payload |
|-------|------|-----|---------|
| 0x01 | `DATA`      | node→gs | Telemetry record (encoding owned by `lib/telem`). Reliable: expects ACK. |
| 0x02 | `ACK`       | either  | 1 byte: the SEQ being acknowledged. |
| 0x03 | `NAK`       | either  | 1 byte: SEQ the receiver wants retransmitted (optional fast-retransmit). |
| 0x04 | `CMD`       | gs→node | Command (e.g. set-setpoint, clear-fault, write-config). Reliable. |
| 0x05 | `HEARTBEAT` | node→gs | Liveness + safety-state byte + fault-flags. Reliable (see §5 note). |
| 0x06 | `CFG_WRITE` | gs→node | Persisted config write (used by  reset-mid-transaction tests). Reliable. |

Unknown TYPE with a valid CRC ⇒ frame is dropped and counted
(`stat_unknown_type`); it is **not** a protocol error that resets the link.

## 4. Decoder state machine (what the fuzzer must survive)
Byte-at-a-time, single static buffer, never blocks, never allocates:

```
HUNT_SOF ──0x7E──▶ GOT_SOF ──VER ok──▶ HDR (accumulate TYPE,SEQ,LEN)
   ▲   └─ else stay        └─ VER bad ─┘ (drop, back to HUNT_SOF)
   │                                    │ LEN>MAX ⇒ drop, HUNT_SOF (count stat_len_err)
   │                                    ▼
   │                              PAYLOAD (accumulate LEN bytes)
   │                                    ▼
   │                              CRC (accumulate 2 bytes)
   └──────── CRC mismatch: drop, count stat_crc_err, HUNT_SOF ◀── CRC ok ⇒ deliver, HUNT_SOF
```

Invariants the fuzzer asserts (property + libFuzzer):
- **No read/write outside the static buffer** for *any* input byte sequence
  (ASan/UBSan clean).
- Decoder **always terminates** per byte (bounded work, no unbounded loop).
- A byte stream that never contains a valid frame yields **zero deliveries** and
  never crashes.
- Concatenation: `decode(encode(A) ++ encode(B))` delivers exactly A then B.
- Garbage-then-frame: `decode(random ++ encode(A))` delivers A (resync works).

## 5. Reliability: sequence / ACK / retransmit / dedup
- **SEQ** increments per *reliable* frame sent (DATA, CMD, HEARTBEAT, CFG_WRITE),
  wraps mod 256. ACK/NAK are **not** sequenced (they carry the acked SEQ in
  payload).
- **Stop-and-wait ARQ.** The sender keeps **one** outstanding reliable frame
  (window = 1). Rationale: simplest correct reliability, bounded state (one retry
  slot, no dynamic queue), and sufficient for a single telemetry stream. Window
  size is a documented design decision, not an oversight (see DESIGN.md).
- **Retransmit:** if no ACK within `GOV_ACK_TIMEOUT_MS = 100`, resend the same
  frame (identical SEQ). Up to `GOV_MAX_RETRIES = 3` retransmits. On exhaustion,
  the link raises `LINK_FAULT` to telemetry + safety (see SAFETY_SM.md) — it does
  **not** silently drop.
- **Dedup (receiver):** track the last accepted SEQ per direction. A reliable
  frame whose SEQ equals the last accepted SEQ is a **duplicate**: re-ACK it but
  do **not** re-deliver to the application. This makes delivery idempotent under
  retransmission. First-ever frame after reset accepts any SEQ.
- Timeout/retry values are registered in `config/registry.md` and are **never
  widened to pass a test**.

## 6. CRC-16 choice + limits
- **CRC-16/CCITT-FALSE**: poly `0x1021`, init `0xFFFF`, no reflect in/out, xorout
  `0x0000`. Big-endian on the wire.
- **Why this one:** poly `0x1021` is the well-studied CCITT polynomial with good
  Hamming-distance properties for short frames; `init=0xFFFF` (vs `0x0000`)
  detects leading-zero insertion/deletion that a zero init misses. It is the same
  variant Zephyr's `crc16_ccitt`/`crc16_itu_t` family uses, so target and host
  agree without a bespoke table.
- **Documented limits (honesty rule):** CRC-16 detects all single/double-bit
  errors, all odd-bit-count errors, all burst errors ≤16 bits, and most longer
  bursts (residual ~2⁻¹⁶). It is an **integrity check, not authentication** — it
  does not protect against a deliberate adversary who recomputes the CRC. This
  limit is stated in the README.

## 7. Public API surface (frozen names — `lib/proto/proto.h`)
```c
/* Encoding: caller supplies the output buffer; no allocation. */
size_t gov_frame_encode(uint8_t type, uint8_t seq,
                        const uint8_t *payload, uint16_t len,
                        uint8_t *out, size_t out_cap);   /* returns bytes, 0 on error */

/* Decoder: fed one byte at a time; calls a delivery callback on a good frame. */
typedef void (*gov_frame_cb)(uint8_t type, uint8_t seq,
                             const uint8_t *payload, uint16_t len, void *ctx);
void gov_decoder_init(gov_decoder_t *d, gov_frame_cb cb, void *ctx);
void gov_decoder_push(gov_decoder_t *d, uint8_t byte);   /* never blocks/allocates */

/* CRC (also used by tests + host mirror). */
uint16_t gov_crc16_ccitt(uint16_t seed, const uint8_t *data, size_t len);
```
Reliability (ARQ/dedup) sits above framing in `lib/proto/reliable.[ch]` with a
similarly allocation-free, testable API. Exact signatures may extend during 
but **field layout, CRC, and the §4/§5 semantics are frozen**.
