/*
 * crc16.h — CRC-16/CCITT-FALSE for the governor link layer.
 *
 * Host-portable C, NO Zephyr includes (see the project docs portability discipline).
 * Parameters (PROTOCOL_SPEC.md §6): poly 0x1021, init 0xFFFF, no input/output
 * reflection, xorout 0x0000. Used by both the encoder and the byte-at-a-time
 * decoder (incrementally) and by the host mirror + tests.
 */
#ifndef GOV_PROTO_CRC16_H
#define GOV_PROTO_CRC16_H

#include <stddef.h>
#include <stdint.h>

/*
 * Compute CRC-16/CCITT-FALSE over `len` bytes starting at `data`, continuing
 * from `seed`. Pass 0xFFFF as the seed to start a fresh CRC; pass a previously
 * returned value to accumulate additional bytes (the decoder feeds one byte at
 * a time). `data` may be NULL iff `len` is 0.
 */
uint16_t gov_crc16_ccitt(uint16_t seed, const uint8_t *data, size_t len);

#endif /* GOV_PROTO_CRC16_H */
