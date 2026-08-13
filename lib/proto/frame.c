/*
 * frame.c — governor link-layer framing: encoder + byte-at-a-time decoder.
 *
 * Implements PROTOCOL_SPEC.md §2 (layout), §3 (types), §4 (decoder state
 * machine). Big-endian wire order. No dynamic allocation (RULES R1); the
 * decoder owns a single fixed payload buffer and does bounded work per byte
 * (R8). Length-driven once a header is accepted; SOF is not byte-stuffed, so a
 * false SOF is caught by LEN sanity + CRC and triggers a resync (§2/§4).
 */
#include <string.h>

#include "proto.h"

/* True for a TYPE the link layer knows how to deliver (PROTOCOL_SPEC §3). */
static int is_known_type(uint8_t type)
{
	switch (type) {
	case GOV_TYPE_DATA:
	case GOV_TYPE_ACK:
	case GOV_TYPE_NAK:
	case GOV_TYPE_CMD:
	case GOV_TYPE_HEARTBEAT:
	case GOV_TYPE_CFG_WRITE:
		return 1;
	default:
		return 0;
	}
}

size_t gov_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
			uint16_t len, uint8_t *out, size_t out_cap)
{
	size_t total;
	uint16_t crc;

	if (out == NULL) {
		return 0u;
	}
	if (len > GOV_MAX_PAYLOAD) {
		return 0u;
	}
	if ((payload == NULL) && (len > 0u)) {
		return 0u;
	}

	total = (size_t)GOV_FRAME_HDR + (size_t)len + (size_t)GOV_FRAME_CRC;
	if (out_cap < total) {
		return 0u;
	}

	out[0] = (uint8_t)GOV_SOF;
	out[1] = (uint8_t)GOV_VER;
	out[2] = type;
	out[3] = seq;
	out[4] = (uint8_t)((len >> 8) & 0xFFu);
	out[5] = (uint8_t)(len & 0xFFu);
	if (len > 0u) {
		memcpy(&out[GOV_FRAME_HDR], payload, (size_t)len);
	}

	/* CRC covers VER..end of PAYLOAD, i.e. out[1] .. out[5+len]. */
	crc = gov_crc16_ccitt(GOV_CRC_INIT, &out[1], (size_t)(5u + len));
	out[(size_t)GOV_FRAME_HDR + (size_t)len] = (uint8_t)((crc >> 8) & 0xFFu);
	out[(size_t)GOV_FRAME_HDR + (size_t)len + 1u] = (uint8_t)(crc & 0xFFu);

	return total;
}

void gov_decoder_init(gov_decoder_t *d, gov_frame_cb cb, void *ctx)
{
	if (d == NULL) {
		return;
	}
	memset(d, 0, sizeof(*d));
	d->cb = cb;
	d->ctx = ctx;
	d->state = GOV_DEC_HUNT_SOF;
}

/* Begin buffering a new frame after a SOF byte (SOF is not part of the CRC). */
static void start_frame(gov_decoder_t *d)
{
	d->crc_calc = GOV_CRC_INIT;
	d->hdr_idx = 0u;
	d->payload_idx = 0u;
	d->crc_idx = 0u;
	d->crc_rx = 0u;
	d->len = 0u;
	d->state = GOV_DEC_VER;
}

/* Accumulate one wire byte into the running CRC (VER..PAYLOAD). */
static void crc_feed(gov_decoder_t *d, uint8_t byte)
{
	d->crc_calc = gov_crc16_ccitt(d->crc_calc, &byte, 1u);
}

/* CRC bytes complete: validate, deliver or count, then resync. */
static void finish_frame(gov_decoder_t *d)
{
	if (d->crc_rx != d->crc_calc) {
		d->stats.crc_err++;
	} else if (!is_known_type(d->type)) {
		d->stats.unknown_type++;
	} else if (d->cb != NULL) {
		d->cb(d->type, d->seq, d->payload, d->len, d->ctx);
	} else {
		/* Valid known frame, no callback registered: nothing to do. */
	}
	d->state = GOV_DEC_HUNT_SOF;
}

void gov_decoder_push(gov_decoder_t *d, uint8_t byte)
{
	if (d == NULL) {
		return;
	}

	switch (d->state) {
	case GOV_DEC_HUNT_SOF:
		if (byte == GOV_SOF) {
			start_frame(d);
		}
		break;

	case GOV_DEC_VER:
		if (byte == GOV_VER) {
			crc_feed(d, byte);
			d->state = GOV_DEC_HDR;
		} else if (byte == GOV_SOF) {
			/* Bad version, but this byte is itself a SOF: re-anchor. */
			start_frame(d);
		} else {
			d->state = GOV_DEC_HUNT_SOF;
		}
		break;

	case GOV_DEC_HDR:
		crc_feed(d, byte);
		switch (d->hdr_idx) {
		case 0u:
			d->type = byte;
			break;
		case 1u:
			d->seq = byte;
			break;
		case 2u:
			d->len = (uint16_t)((uint16_t)byte << 8);
			break;
		default: /* hdr_idx == 3: LEN low byte, header complete */
			d->len = (uint16_t)(d->len | (uint16_t)byte);
			break;
		}
		d->hdr_idx++;
		if (d->hdr_idx >= 4u) {
			if (d->len > GOV_MAX_PAYLOAD) {
				d->stats.len_err++;
				d->state = GOV_DEC_HUNT_SOF;
			} else if (d->len == 0u) {
				d->state = GOV_DEC_CRC;
			} else {
				d->payload_idx = 0u;
				d->state = GOV_DEC_PAYLOAD;
			}
		}
		break;

	case GOV_DEC_PAYLOAD:
		crc_feed(d, byte);
		d->payload[d->payload_idx] = byte;
		d->payload_idx++;
		if (d->payload_idx >= d->len) {
			d->state = GOV_DEC_CRC;
		}
		break;

	case GOV_DEC_CRC:
		d->crc_rx = (uint16_t)((uint16_t)(d->crc_rx << 8) | (uint16_t)byte);
		d->crc_idx++;
		if (d->crc_idx >= 2u) {
			finish_frame(d);
		}
		break;

	default:
		/* Unreachable defensive reset (RULES R4): never silently wedge. */
		d->state = GOV_DEC_HUNT_SOF;
		break;
	}
}
