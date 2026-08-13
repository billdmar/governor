/*
 * health.c -- health aggregator (see health.h). Pure logic, no clock, no alloc.
 */
#include "health.h"

void gov_health_init(struct gov_health *h)
{
	uint32_t i;

	if (h == NULL) {
		return;
	}
	for (i = 0u; i < (uint32_t)GOV_HEALTH_SUBSYS_COUNT; i++) {
		h->last_seen[i] = 0u;
		h->seen[i] = false;
	}
	h->consec_misses = 0u;
}

void gov_health_note_alive(struct gov_health *h, enum gov_health_subsys sys, uint32_t tick)
{
	if (h == NULL || (unsigned)sys >= (unsigned)GOV_HEALTH_SUBSYS_COUNT) {
		return;
	}
	h->last_seen[sys] = tick;
	h->seen[sys] = true;
}

void gov_health_note_deadline(struct gov_health *h, bool met)
{
	if (h == NULL) {
		return;
	}
	if (met) {
		h->consec_misses = 0u;
	} else {
		h->consec_misses++;
	}
}

bool gov_health_subsys_alive(const struct gov_health *h, enum gov_health_subsys sys, uint32_t now,
			     uint32_t threshold)
{
	uint32_t age;

	if (h == NULL || (unsigned)sys >= (unsigned)GOV_HEALTH_SUBSYS_COUNT) {
		return false;
	}
	if (!h->seen[sys]) {
		return false; /* never reported -> not alive */
	}
	/* Monotonic ticks: unsigned age; a "future" last_seen wraps large -> dead. */
	age = now - h->last_seen[sys];
	return age <= threshold;
}

bool gov_health_all_alive(const struct gov_health *h, uint32_t now, uint32_t threshold)
{
	uint32_t i;

	if (h == NULL) {
		return false;
	}
	for (i = 0u; i < (uint32_t)GOV_HEALTH_SUBSYS_COUNT; i++) {
		if (!gov_health_subsys_alive(h, (enum gov_health_subsys)i, now, threshold)) {
			return false;
		}
	}
	return true;
}

bool gov_health_timing_fault(const struct gov_health *h)
{
	if (h == NULL) {
		return false;
	}
	return h->consec_misses >= GOV_MISS_LIMIT;
}

uint32_t gov_health_consec_misses(const struct gov_health *h)
{
	if (h == NULL) {
		return 0u;
	}
	return h->consec_misses;
}

bool gov_health_feed_watchdog(const struct gov_health *h, uint32_t now, uint32_t threshold)
{
	if (h == NULL) {
		return false;
	}
	/* Feed only if the system as a whole is scheduling correctly (TASKS sec 4). */
	return gov_health_all_alive(h, now, threshold) && !gov_health_timing_fault(h);
}
