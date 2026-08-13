/*
 * proto.h — governor link-layer framing (host-portable, NO Zephyr includes).
 *
 * Implements PROTOCOL_SPEC.md exactly: frame layout (§2), message types (§3),
 * and the byte-at-a-time decoder state machine (§4). All multi-byte integer
 * fields are big-endian on the wire. No dynamic allocation (RULES R1); the
 * decoder works from a single fixed-size static payload buffer (R8).
 */
#ifndef GOV_PROTO_H
#define GOV_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "crc16.h"

/* Wire constants (PROTOCOL_SPEC §2). */
#define GOV_SOF 0x7Eu /* start-of-frame sentinel / resync anchor */
#define GOV_VER 0x01u /* protocol version; decoder rejects others */

/* CRC-16/CCITT-FALSE initial seed (PROTOCOL_SPEC §6). */
#define GOV_CRC_INIT ((uint16_t)0xFFFFu)

/*
 * GOV_MAX_PAYLOAD (config/registry.md §1): bounds the static decode buffer.
 * Kept here as the single source of truth for the proto module.
 */
#define GOV_MAX_PAYLOAD 64u

/*
 * On-wire header size including SOF: SOF+VER+TYPE+SEQ+LEN(2) = 6 bytes.
 * Trailer: CRC(2). GOV_FRAME_MAX is the largest complete frame on the wire
 * (header + max payload + CRC) and is the minimum output capacity a caller
 * must give gov_frame_encode() for a max-size frame.
 *
 * NOTE for : PROTOCOL_SPEC §2 states "GOV_FRAME_MAX = ... = 71 bytes incl.
 * SOF", but the arithmetic there (1+1+1+2+64+2) sums the six post-SOF fields
 * and omits the 1-byte SOF, so 71 is actually the buffered region *excluding*
 * SOF. The full on-wire frame including SOF is 72. We size the encoder output
 * capacity to 72 so a caller sizing out[GOV_FRAME_MAX] is never one byte short.
 * Flagging the spec's "incl. SOF" wording as an arithmetic slip; not editing
 * the frozen contract.
 */
#define GOV_FRAME_HDR 6u
#define GOV_FRAME_CRC 2u
#define GOV_FRAME_MAX (GOV_FRAME_HDR + GOV_MAX_PAYLOAD + GOV_FRAME_CRC) /* 72 */

/* Message types (PROTOCOL_SPEC §3). */
enum gov_msg_type {
	GOV_TYPE_DATA = 0x01,
	GOV_TYPE_ACK = 0x02,
	GOV_TYPE_NAK = 0x03,
	GOV_TYPE_CMD = 0x04,
	GOV_TYPE_HEARTBEAT = 0x05,
	GOV_TYPE_CFG_WRITE = 0x06,
};

/* Decoder state machine (PROTOCOL_SPEC §4). */
enum gov_decoder_state {
	GOV_DEC_HUNT_SOF = 0, /* scanning for SOF (resync) */
	GOV_DEC_VER,          /* SOF seen; next byte is VER */
	GOV_DEC_HDR,          /* accumulating TYPE, SEQ, LEN */
	GOV_DEC_PAYLOAD,      /* accumulating LEN payload bytes */
	GOV_DEC_CRC,          /* accumulating the 2 CRC bytes */
};

/*
 * Decoder statistics. crc_err/len_err/unknown_type are detectable at the pure
 * framing layer and counted here. Duplicate-frame accounting (stat_dup,
 * PROTOCOL_SPEC §5) is a reliability concept — it needs per-direction accepted-
 * SEQ state — so it lives in gov_rx_t (reliable.h), not here.
 */
struct gov_decoder_stats {
	uint32_t crc_err;      /* frames dropped on CRC mismatch */
	uint32_t len_err;      /* frames dropped: LEN > GOV_MAX_PAYLOAD */
	uint32_t unknown_type; /* CRC-valid frames with an unknown TYPE */
};

/*
 * Delivery callback: invoked once per CRC-valid, known-type frame. `payload`
 * points into the decoder's static buffer and is valid only for the duration
 * of the call (the caller must copy if it needs to retain it).
 */
typedef void (*gov_frame_cb)(uint8_t type, uint8_t seq, const uint8_t *payload,
			     uint16_t len, void *ctx);

struct gov_decoder {
	gov_frame_cb cb;
	void *ctx;
	enum gov_decoder_state state;
	uint8_t type;
	uint8_t seq;
	uint16_t len;         /* declared payload length, once known */
	uint16_t payload_idx; /* payload bytes received so far */
	uint8_t hdr_idx;      /* TYPE/SEQ/LEN bytes received so far (0..4) */
	uint8_t crc_idx;      /* CRC bytes received so far (0..2) */
	uint16_t crc_calc;    /* running CRC over VER..PAYLOAD */
	uint16_t crc_rx;      /* received CRC being accumulated */
	uint8_t payload[GOV_MAX_PAYLOAD];
	struct gov_decoder_stats stats;
};

typedef struct gov_decoder gov_decoder_t;

/*
 * Encode a frame into `out` (caller-supplied, no allocation). Returns the
 * number of bytes written, or 0 on error (len > GOV_MAX_PAYLOAD, payload NULL
 * while len > 0, or out_cap too small).
 */
size_t gov_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
			uint16_t len, uint8_t *out, size_t out_cap);

/* Initialize a decoder. `cb` may be NULL (frames are then validated only). */
void gov_decoder_init(gov_decoder_t *d, gov_frame_cb cb, void *ctx);

/* Feed one byte. Never blocks, never allocates, bounded work per byte. */
void gov_decoder_push(gov_decoder_t *d, uint8_t byte);

#endif /* GOV_PROTO_H */
