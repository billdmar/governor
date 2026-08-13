/*
 * config.h — host-portable persistent configuration (CRC-protected, A/B slots).
 *
 * Stores the node's persistable settings (control setpoint + PID trims) across
 * resets. The torn-write safety property (registry F15: "a reset mid config
 * write leaves the stored config either OLD-valid or NEW-valid, never corrupt")
 * is pure logic proven on the host here; a thin Zephyr NVS/settings adapter
 * (app/target only) binds this to the STM32 internal flash. NO Zephyr includes.
 *
 * Design — two independent slots (A/B double-buffer):
 *   - Each slot is [magic | version | seq | payload | crc16]. A slot is VALID
 *     iff magic + version match and crc16 verifies.
 *   - Load picks the VALID slot with the highest seq (monotonic, wrap-aware).
 *   - Save always writes the *other* slot, then that slot's higher seq makes it
 *     the active one. A power cut mid-write corrupts at most the slot being
 *     written; the previously-active slot is untouched → still recoverable.
 *   - If neither slot is valid (first boot / both erased) load returns defaults
 *     and reports GOV_CONFIG_DEFAULTED (not a fault — a clean first boot).
 *
 * No dynamic allocation (RULES R1); fixed-width types (R3); all buffers bounded.
 */
#ifndef GOV_CONFIG_H
#define GOV_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Persistable payload. Keep small + fixed; extend with care (bump VERSION). */
struct gov_config {
	int32_t  setpoint;   /* control target, plant-units */
	int32_t  kp_milli;   /* PID gains as fixed-point milli-units (x1000) so */
	int32_t  ki_milli;   /* the persisted blob has no float representation  */
	int32_t  kd_milli;   /* portability concerns across host/target.        */
	uint32_t boot_count; /* incremented each successful load — liveness/telemetry */
};

/* One stored slot on the medium. Packed layout is CRC'd as raw bytes. */
#define GOV_CONFIG_MAGIC   0x47434647u /* "GCFG" */
#define GOV_CONFIG_VERSION 1u
struct gov_config_slot {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;               /* monotonic; highest valid slot wins */
	struct gov_config payload;
	uint16_t crc;               /* CRC-16/CCITT over magic..payload */
	uint16_t _pad;              /* keep the struct 4-byte aligned */
};

#define GOV_CONFIG_SLOT_BYTES ((uint32_t)sizeof(struct gov_config_slot))

/*
 * Abstract storage medium (two slots). On target this binds to two NVS entries
 * / two flash pages; in tests it binds to a fake that can simulate torn writes.
 * read/write operate on a slot index (0 or 1); write MAY be interrupted (the
 * fake models that). erase is optional (NVS handles it); may be NULL.
 */
struct gov_config_store {
	/* Read slot `idx` (0/1) into `out`. Returns true on a completed read. */
	bool (*read)(void *ctx, uint8_t idx, struct gov_config_slot *out);
	/* Persist `slot` to slot `idx`. Returns true if the write fully landed. */
	bool (*write)(void *ctx, uint8_t idx, const struct gov_config_slot *slot);
	void *ctx;
};

typedef enum {
	GOV_CONFIG_LOADED = 0,   /* a valid slot was found and loaded */
	GOV_CONFIG_DEFAULTED,    /* no valid slot — defaults returned (first boot) */
	GOV_CONFIG_RECOVERED,    /* one slot was corrupt; recovered from the other */
} gov_config_status_t;

/* The compile-time defaults used when nothing valid is stored. */
extern const struct gov_config GOV_CONFIG_DEFAULTS;

/* Runtime handle (caller-owned; no allocation). */
typedef struct {
	struct gov_config_store store;
	struct gov_config cfg;   /* current in-RAM config */
	uint32_t last_seq;       /* seq of the active slot */
	uint8_t  active_idx;     /* which slot is currently active (0/1) */
	bool     valid;          /* a load has established a valid active slot */
} gov_config_ctx_t;

/* --- API --- */
void                gov_config_init(gov_config_ctx_t *c, const struct gov_config_store *store);
/* Load the best valid slot (or defaults). Sets *out to the loaded config. */
gov_config_status_t gov_config_load(gov_config_ctx_t *c, struct gov_config *out);
/* Persist `in` to the inactive slot (atomic swap on success). Returns true if
 * the write completed; false if the medium rejected/tore the write (the old
 * active slot remains intact and recoverable). */
bool                gov_config_save(gov_config_ctx_t *c, const struct gov_config *in);

/* Compute a slot's CRC (exposed for the adapter + tests). */
uint16_t            gov_config_slot_crc(const struct gov_config_slot *slot);
/* True iff a slot is structurally valid (magic+version+crc). */
bool                gov_config_slot_valid(const struct gov_config_slot *slot);

#endif /* GOV_CONFIG_H */
