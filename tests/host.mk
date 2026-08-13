# tests/host.mk — shared host-test rules for the host-portable C modules.
# Included by each module's test Makefile. Compiles the module + its unit tests
# with the host compiler under -Wall -Wextra -Werror (+ optional sanitizers) and
# runs them. NO Zephyr — these modules are pure C by contract (docs/DESIGN.md D5).
#
# Usage (from a module test dir, e.g. tests/proto/Makefile):
#   MODULE   := proto
#   SRCS     := ../../lib/proto/frame.c ../../lib/proto/crc16.c
#   TEST_SRCS:= test_frame.c test_crc16.c
#   INCLUDES := -I../../lib/proto
#   include ../host.mk

CC      ?= cc
CSTD    ?= -std=c11
WARN    := -Wall -Wextra -Werror -Wconversion -Wshadow -Wpointer-arith
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all
DBG     := -g -O1
CFLAGS  := $(CSTD) $(WARN) $(DBG) $(SAN) $(INCLUDES)

BUILD   := build
BIN     := $(BUILD)/test_$(MODULE)
OBJS    := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o) $(TEST_SRCS:.c=.o)))

VPATH   := $(sort $(dir $(SRCS) $(TEST_SRCS)))

.PHONY: test clean
test: $(BIN)
	@echo "== running $(MODULE) host tests =="
	./$(BIN)

$(BIN): $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
