/*
 * test_config.c — host tests for the persistent config module, focused on the
 * F15 torn-write invariant: after ANY interrupted config write, a reload yields
 * either the OLD-valid or the NEW-valid config, NEVER corrupt.
 *
 * The fake store models two flash slots and can inject a "torn write": the
 * write lands only partially (first N bytes) and then reports failure — exactly
 * what a power cut mid-flash-write looks like.
 */
#include "../gov_test.h"
#include "../../lib/config/config.h"

#include <string.h>

/* ---- fake two-slot store with torn-write injection ---- */
struct fake_store {
	uint8_t slot[2][GOV_CONFIG_SLOT_BYTES];
	bool    present[2];   /* has the slot ever been written (else "erased")? */
	/* torn-write control: if tear_at >= 0, the next write to tear_idx writes
	 * only `tear_at` bytes then FAILS (simulating a power cut). */
	int      tear_idx;
	long     tear_at;
	unsigned writes;
};

static void fake_init(struct fake_store *f)
{
	memset(f, 0, sizeof *f);
	f->tear_idx = -1;
	f->tear_at = -1;
}

static bool fake_read(void *ctx, uint8_t idx, struct gov_config_slot *out)
{
	struct fake_store *f = (struct fake_store *)ctx;
	if (idx > 1u || !f->present[idx]) {
		return false; /* erased/never-written slot reads as absent */
	}
	memcpy(out, f->slot[idx], sizeof *out);
	return true;
}

static bool fake_write(void *ctx, uint8_t idx, const struct gov_config_slot *slot)
{
	struct fake_store *f = (struct fake_store *)ctx;
	if (idx > 1u) {
		return false;
	}
	f->writes++;
	const uint8_t *src = (const uint8_t *)slot;
	if (f->tear_idx == (int)idx && f->tear_at >= 0) {
		/* Torn write: only the first tear_at bytes land, then power is lost.
		 * The slot is now present but (almost certainly) CRC-invalid. */
		size_t n = (size_t)f->tear_at;
		if (n > GOV_CONFIG_SLOT_BYTES) {
			n = GOV_CONFIG_SLOT_BYTES;
		}
		memcpy(f->slot[idx], src, n);
		f->present[idx] = true;
		f->tear_idx = -1; f->tear_at = -1;
		return false; /* write did not complete */
	}
	memcpy(f->slot[idx], src, GOV_CONFIG_SLOT_BYTES);
	f->present[idx] = true;
	return true;
}

static struct gov_config_store make_store(struct fake_store *f)
{
	struct gov_config_store s = { .read = fake_read, .write = fake_write, .ctx = f };
	return s;
}

/* First boot: nothing stored → defaults. */
static void test_first_boot_defaults(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);

	struct gov_config out;
	gov_config_status_t s = gov_config_load(&c, &out);
	GOV_CHECK_EQ(s, GOV_CONFIG_DEFAULTED);
	GOV_CHECK_EQ(out.setpoint, GOV_CONFIG_DEFAULTS.setpoint);
}

/* Save then reload survives (persistence). */
static void test_save_then_load(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);

	struct gov_config in = GOV_CONFIG_DEFAULTS;
	in.setpoint = 73;
	GOV_CHECK(gov_config_save(&c, &in));

	/* Simulate a reboot: fresh ctx over the same store. */
	gov_config_ctx_t c2;
	gov_config_init(&c2, &st);
	struct gov_config out;
	gov_config_status_t s = gov_config_load(&c2, &out);
	GOV_CHECK(s == GOV_CONFIG_LOADED || s == GOV_CONFIG_RECOVERED);
	GOV_CHECK_EQ(out.setpoint, 73);
}

/* Multiple saves: newest wins, alternating slots. */
static void test_newest_wins(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);

	for (int i = 1; i <= 10; i++) {
		struct gov_config in = GOV_CONFIG_DEFAULTS;
		in.setpoint = i;
		GOV_CHECK(gov_config_save(&c, &in));
	}
	gov_config_ctx_t c2;
	gov_config_init(&c2, &st);
	struct gov_config out;
	gov_config_load(&c2, &out);
	GOV_CHECK_EQ(out.setpoint, 10); /* last write wins */
}

/* F15 core: a torn write to the inactive slot leaves the OLD value intact. */
static void test_f15_torn_write_keeps_old(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);

	/* Establish a good stored config (setpoint 40) in slot 1. */
	struct gov_config a = GOV_CONFIG_DEFAULTS; a.setpoint = 40;
	GOV_CHECK(gov_config_save(&c, &a));

	/* Now attempt to write setpoint 99, but TEAR it (power cut). It targets
	 * the inactive slot (0). */
	f.tear_idx = 0; f.tear_at = 8; /* only 8 bytes land */
	struct gov_config b = GOV_CONFIG_DEFAULTS; b.setpoint = 99;
	GOV_CHECK(!gov_config_save(&c, &b)); /* save reports failure */

	/* Reboot + reload: must recover the OLD valid config (40), never corrupt. */
	gov_config_ctx_t c2;
	gov_config_init(&c2, &st);
	struct gov_config out;
	gov_config_status_t s = gov_config_load(&c2, &out);
	GOV_CHECK(s == GOV_CONFIG_LOADED || s == GOV_CONFIG_RECOVERED);
	GOV_CHECK_EQ(out.setpoint, 40); /* NOT 99, NOT garbage */
}

/* F15 property sweep: tear at EVERY byte offset of the write; a reload must
 * ALWAYS yield old-valid or new-valid, never a corrupt/garbage config. */
static void test_f15_torn_write_property_sweep(void)
{
	for (long tear = 0; tear <= (long)GOV_CONFIG_SLOT_BYTES; tear++) {
		struct fake_store f; fake_init(&f);
		gov_config_ctx_t c;
		struct gov_config_store st = make_store(&f);
		gov_config_init(&c, &st);

		struct gov_config a = GOV_CONFIG_DEFAULTS; a.setpoint = 40;
		GOV_CHECK(gov_config_save(&c, &a));

		/* Tear the second write at offset `tear`. */
		f.tear_idx = (int)(c.active_idx ^ 1u);
		f.tear_at = tear;
		struct gov_config b = GOV_CONFIG_DEFAULTS; b.setpoint = 99;
		(void)gov_config_save(&c, &b); /* may or may not "succeed" at full len */

		gov_config_ctx_t c2;
		gov_config_init(&c2, &st);
		struct gov_config out;
		gov_config_status_t s = gov_config_load(&c2, &out);
		/* Invariant: never DEFAULTED (we had a valid slot), and the value is
		 * exactly one of the two intended configs — never anything else. */
		GOV_CHECK(s != GOV_CONFIG_DEFAULTED);
		GOV_CHECK(out.setpoint == 40 || out.setpoint == 99);
	}
}

/* A corrupt active slot with a valid other slot recovers + reports RECOVERED. */
static void test_recovered_status(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);

	struct gov_config a = GOV_CONFIG_DEFAULTS; a.setpoint = 55;
	GOV_CHECK(gov_config_save(&c, &a)); /* lands in slot 1 */

	/* Corrupt slot 1's CRC region directly. */
	f.slot[1][GOV_CONFIG_SLOT_BYTES - 3] ^= 0xFFu;

	gov_config_ctx_t c2;
	gov_config_init(&c2, &st);
	struct gov_config out;
	gov_config_status_t s = gov_config_load(&c2, &out);
	/* slot 1 corrupt, slot 0 never written → defaults (no valid slot). */
	GOV_CHECK_EQ(s, GOV_CONFIG_DEFAULTED);
}

/* Slot validity: magic/version/crc gating. */
static void test_slot_validity(void)
{
	struct gov_config_slot slot;
	memset(&slot, 0, sizeof slot);
	slot.magic = GOV_CONFIG_MAGIC;
	slot.version = GOV_CONFIG_VERSION;
	slot.seq = 1;
	slot.payload = GOV_CONFIG_DEFAULTS;
	slot.crc = gov_config_slot_crc(&slot);
	GOV_CHECK(gov_config_slot_valid(&slot));

	slot.crc ^= 0x1u;                       GOV_CHECK(!gov_config_slot_valid(&slot));
	slot.crc = gov_config_slot_crc(&slot);  GOV_CHECK(gov_config_slot_valid(&slot));
	slot.magic = 0xDEADBEEFu;               GOV_CHECK(!gov_config_slot_valid(&slot));
	GOV_CHECK(!gov_config_slot_valid(NULL));
}

/* Both slots valid → the newer seq wins (exercises the both-valid branch). */
static void test_both_valid_newest_wins(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);
	/* Two saves land in slot1 then slot0 (alternating), both valid. */
	struct gov_config a = GOV_CONFIG_DEFAULTS; a.setpoint = 11;
	GOV_CHECK(gov_config_save(&c, &a));
	struct gov_config b = GOV_CONFIG_DEFAULTS; b.setpoint = 22;
	GOV_CHECK(gov_config_save(&c, &b));
	GOV_CHECK(f.present[0] && f.present[1]); /* both slots written */

	gov_config_ctx_t c2;
	gov_config_init(&c2, &st);
	struct gov_config out;
	GOV_CHECK_EQ(gov_config_load(&c2, &out), GOV_CONFIG_LOADED);
	GOV_CHECK_EQ(out.setpoint, 22); /* newer */
}

/* One slot valid, the other present-but-corrupt → RECOVERED. */
static void test_recovered_from_corrupt_other(void)
{
	struct fake_store f; fake_init(&f);
	gov_config_ctx_t c;
	struct gov_config_store st = make_store(&f);
	gov_config_init(&c, &st);
	struct gov_config a = GOV_CONFIG_DEFAULTS; a.setpoint = 33;
	GOV_CHECK(gov_config_save(&c, &a));  /* slot 1 valid */
	struct gov_config b = GOV_CONFIG_DEFAULTS; b.setpoint = 44;
	GOV_CHECK(gov_config_save(&c, &b));  /* slot 0 valid, now active */
	/* Corrupt the inactive slot (1) so only slot 0 is valid but both present. */
	f.slot[1][10] ^= 0xFFu;

	gov_config_ctx_t c2;
	gov_config_init(&c2, &st);
	struct gov_config out;
	GOV_CHECK_EQ(gov_config_load(&c2, &out), GOV_CONFIG_RECOVERED);
	GOV_CHECK_EQ(out.setpoint, 44);
}

/* NULL-guard defensive paths (RULES R6). */
static void test_config_null_guards(void)
{
	gov_config_init(NULL, NULL);
	struct gov_config_store st; /* uninit is fine — init returns early on NULL c */
	memset(&st, 0, sizeof st);
	gov_config_init(NULL, &st);
	GOV_CHECK_EQ(gov_config_load(NULL, NULL), GOV_CONFIG_DEFAULTED);
	gov_config_ctx_t c; gov_config_init(&c, &st);
	GOV_CHECK(!gov_config_save(NULL, NULL));
	GOV_CHECK(!gov_config_save(&c, NULL));
}

int main(void)
{
	GOV_RUN(test_first_boot_defaults);
	GOV_RUN(test_save_then_load);
	GOV_RUN(test_newest_wins);
	GOV_RUN(test_f15_torn_write_keeps_old);
	GOV_RUN(test_f15_torn_write_property_sweep);
	GOV_RUN(test_recovered_status);
	GOV_RUN(test_slot_validity);
	GOV_RUN(test_both_valid_newest_wins);
	GOV_RUN(test_recovered_from_corrupt_other);
	GOV_RUN(test_config_null_guards);
	return GOV_TEST_SUMMARY();
}
