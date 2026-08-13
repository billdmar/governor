# tests/proto/Makefile — host unit + property tests for lib/proto.
# Builds the portable proto modules + tests under host.mk's strict flags
# (-Wall -Wextra -Werror -Wconversion + ASan/UBSan). No Zephyr.

MODULE    := proto
SRCS      := ../../lib/proto/crc16.c ../../lib/proto/frame.c ../../lib/proto/reliable.c
TEST_SRCS := test_proto.c
INCLUDES  := -I../../lib/proto -I..

include ../host.mk
