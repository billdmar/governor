# tests/integration/Makefile — end-to-end node coordinator test.
# Links the full portable stack: safety + control + telem + the app coordinator.
MODULE    := node
SRCS      := ../../app/node_core.c \
             ../../lib/safety/safety.c \
             ../../lib/control/pid.c \
             ../../lib/control/plant.c \
             ../../lib/telem/telem.c
TEST_SRCS := test_node.c
INCLUDES  := -I../../app -I../../lib/safety -I../../lib/control \
             -I../../lib/telem -I../../lib/common
LDLIBS    := -lm
include ../host.mk
