/*
 * fake_bus.c — scriptable fake HAL bus for host tests (see fake_bus.h).
 */
#include <stddef.h>

#include "fake_bus.h"

static struct gov_fake_bus *self(void *ctx)
{
	return (struct gov_fake_bus *)ctx;
}

/* Current scripted step: the one at idx, or the last one once exhausted. */
static const struct gov_fake_step *current_step(struct gov_fake_bus *fb)
{
	size_t i;

	if (fb->n == 0) {
		return NULL;
	}
	i = (fb->idx < fb->n) ? fb->idx : (fb->n - 1);
	return &fb->steps[i];
}

static gov_bus_status_t fake_read(void *ctx, uint8_t addr, uint8_t reg,
				  uint8_t *buf, size_t len)
{
	struct gov_fake_bus *fb = self(ctx);
	const struct gov_fake_step *step;

	(void)addr;
	(void)reg;

	fb->read_calls++;

	step = current_step(fb);
	if (step == NULL) {
		return GOV_BUS_ERR; /* unscripted read = misconfigured test */
	}

	if (step->status == GOV_BUS_OK) {
		/* Encode value big-endian into buf, matching sensor decode. */
		uint32_t raw = (uint32_t)step->value;
		for (size_t i = 0; i < len; i++) {
			size_t shift = (len - 1 - i) * 8u;
			buf[i] = (uint8_t)((raw >> shift) & 0xFFu);
		}
	}

	/* Advance only while steps remain; the last entry sticks. */
	if (fb->idx < fb->n) {
		fb->idx++;
	}

	return step->status;
}

static gov_bus_status_t fake_write(void *ctx, uint8_t addr, uint8_t reg,
				   const uint8_t *buf, size_t len)
{
	struct gov_fake_bus *fb = self(ctx);
	const struct gov_fake_step *step;

	(void)addr;
	(void)reg;
	(void)buf;
	(void)len;

	fb->write_calls++;

	/* Writes honour the current step's status (so bus-error scripts affect
	 * writes too) but never consume/advance the read script. */
	step = current_step(fb);
	if (step == NULL) {
		return GOV_BUS_OK;
	}
	return (step->status == GOV_BUS_OK) ? GOV_BUS_OK : step->status;
}

static gov_bus_status_t fake_probe(void *ctx, uint8_t addr)
{
	struct gov_fake_bus *fb = self(ctx);
	const struct gov_fake_step *step;

	(void)addr;

	step = current_step(fb);
	if (step == NULL) {
		return GOV_BUS_OK;
	}
	return (step->status == GOV_BUS_ERR) ? GOV_BUS_ERR : GOV_BUS_OK;
}

void gov_fake_bus_init(struct gov_fake_bus *fb)
{
	fb->bus.read = fake_read;
	fb->bus.write = fake_write;
	fb->bus.probe = fake_probe;
	fb->bus.ctx = fb;
	fb->n = 0;
	fb->idx = 0;
	fb->read_calls = 0;
	fb->write_calls = 0;
}

void gov_fake_push(struct gov_fake_bus *fb, gov_bus_status_t status, int32_t value)
{
	if (fb->n >= GOV_FAKE_MAX_STEPS) {
		return; /* saturate: never overflow the static script buffer */
	}
	fb->steps[fb->n].status = status;
	fb->steps[fb->n].value = value;
	fb->n++;
}

void gov_fake_push_repeat(struct gov_fake_bus *fb, int32_t value, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		gov_fake_push(fb, GOV_BUS_OK, value);
	}
}

void gov_fake_set(struct gov_fake_bus *fb, gov_bus_status_t status, int32_t value)
{
	fb->n = 0;
	fb->idx = 0;
	gov_fake_push(fb, status, value);
}
