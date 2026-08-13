/*
 * fuzz_frame.c — libFuzzer target for the byte-at-a-time frame decoder.
 *
 * Feeds every input byte through gov_decoder_push() (PROTOCOL_SPEC §4). The
 * decoder must survive ANY input with no out-of-bounds access, no UB, and no
 * hang — enforced under -fsanitize=fuzzer,address,undefined. The delivery
 * callback touches the whole reported payload so ASan catches any bogus
 * (ptr,len) the decoder might hand out.
 *
 * Build (see task VERIFY block):
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -I lib/proto \
 *     fuzz/fuzz_frame.c lib/proto/frame.c lib/proto/crc16.c -o /tmp/fuzz_frame
 */
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

/* Sink that reads every delivered byte so ASan validates the (ptr,len) pair. */
static void on_frame(uint8_t type, uint8_t seq, const uint8_t *payload,
		     uint16_t len, void *ctx)
{
	volatile uint8_t sink = 0u;
	uint32_t *deliveries = (uint32_t *)ctx;

	(void)type;
	(void)seq;
	for (uint16_t i = 0u; i < len; i++) {
		sink = (uint8_t)(sink ^ payload[i]);
	}
	(void)sink;

	if (deliveries != NULL) {
		(*deliveries)++;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gov_decoder_t dec;
	uint32_t deliveries = 0u;

	gov_decoder_init(&dec, on_frame, &deliveries);
	for (size_t i = 0u; i < size; i++) {
		gov_decoder_push(&dec, data[i]);
	}
	return 0;
}
