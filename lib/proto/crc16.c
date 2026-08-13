/*
 * crc16.c — CRC-16/CCITT-FALSE (bit-serial, table-free).
 *
 * Table-free keeps the target flash footprint tiny and the code trivially
 * auditable; the frame decoder feeds this one byte at a time, so per-call cost
 * is a fixed 8-iteration inner loop (bounded work, no allocation — RULES R1/R8).
 */
#include "crc16.h"

uint16_t gov_crc16_ccitt(uint16_t seed, const uint8_t *data, size_t len)
{
	uint16_t crc = seed;

	for (size_t i = 0u; i < len; i++) {
		/* XOR next byte into the high 8 bits of the register. */
		crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));

		for (unsigned bit = 0u; bit < 8u; bit++) {
			if ((crc & 0x8000u) != 0u) {
				crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}
