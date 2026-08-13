/*
 * telem.h -- governor telemetry record encoding (host-portable, no allocation).
 *
 * Owns the DATA (0x01) and HEARTBEAT (0x05) payload encodings referenced by
 * PROTOCOL_SPEC sec 3 ("Telemetry record (encoding owned by lib/telem)"). All
 * multi-byte fields are big-endian, matching the frame layer's fixed
 * convention (PROTOCOL_SPEC sec 2). Encoders write into a caller-supplied buffer
 * and never allocate; decoders exist for the host/tests to verify round-trip.
 *
 * These payloads are the LEN-byte opaque PAYLOAD carried inside a frame -- the
 * frame layer (lib/proto) adds SOF/VER/TYPE/SEQ/LEN/CRC around them.
 *
 * Host-portable C, no Zephyr includes.
 */
#ifndef GOV_TELEM_H
#define GOV_TELEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gov_faults.h"

/*
 * DATA record -- one control-loop telemetry snapshot (TYPE 0x01 payload).
 *
 * On-wire layout (big-endian, fixed, 21 bytes -- well within GOV_MAX_PAYLOAD=64):
 *   off  size  field
 *    0    4    tick        u32  monotonic control tick / timestamp
 *    4    4    setpoint    i32  commanded value (plant units)
 *    8    4    measurement i32  sensor reading (plant units)
 *   12    4    output      i32  actuator output (plant units)
 *   16    1    state       u8   safety state (gov_state_t as a byte)
 *   17    4    fault_flags u32  GOV_FAULT_* bitmask (gov_faults.h)
 *   ----------
 *   21 bytes total
 */
struct gov_telem_record {
	uint32_t tick;
	int32_t setpoint;
	int32_t measurement;
	int32_t output;
	uint8_t state;
	uint32_t fault_flags;
};

#define GOV_TELEM_DATA_LEN 21u

/*
 * HEARTBEAT payload (TYPE 0x05) -- liveness + safety-state + fault flags.
 *
 * On-wire layout (big-endian, fixed, 9 bytes):
 *   off  size  field
 *    0    1    state        u8   safety state (gov_state_t as a byte)
 *    1    4    fault_flags  u32  GOV_FAULT_* bitmask
 *    5    4    uptime_count u32  heartbeat counter / uptime ticks (liveness)
 *   ----------
 *   9 bytes total
 */
struct gov_heartbeat {
	uint8_t state;
	uint32_t fault_flags;
	uint32_t uptime_count;
};

#define GOV_TELEM_HEARTBEAT_LEN 9u

/*
 * Encode a DATA record into out[0..out_cap). Returns the number of bytes
 * written (GOV_TELEM_DATA_LEN), or 0 on error (null args / buffer too small).
 * No allocation.
 */
size_t gov_telem_encode(const struct gov_telem_record *rec, uint8_t *out, size_t out_cap);

/*
 * Decode a DATA record from in[0..len). Returns true on success (len >=
 * GOV_TELEM_DATA_LEN and args non-null), false otherwise. Reads exactly
 * GOV_TELEM_DATA_LEN bytes; trailing bytes are ignored.
 */
bool gov_telem_decode(const uint8_t *in, size_t len, struct gov_telem_record *rec);

/*
 * Encode a HEARTBEAT payload into out[0..out_cap). Returns bytes written
 * (GOV_TELEM_HEARTBEAT_LEN), or 0 on error. No allocation.
 */
size_t gov_heartbeat_encode(const struct gov_heartbeat *hb, uint8_t *out, size_t out_cap);

/*
 * Decode a HEARTBEAT payload from in[0..len). Returns true on success, false
 * otherwise. Reads exactly GOV_TELEM_HEARTBEAT_LEN bytes.
 */
bool gov_heartbeat_decode(const uint8_t *in, size_t len, struct gov_heartbeat *hb);

#endif /* GOV_TELEM_H */
