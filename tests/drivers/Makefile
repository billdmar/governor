# tests/drivers/Makefile — host unit tests for the sensor driver logic.
# Compiles the host-portable driver (drivers/sensor.c) + the test-only fake bus
# and unit tests under -Wall -Wextra -Werror -Wconversion + ASan/UBSan (host.mk).
# NO Zephyr — the driver logic is pure C by contract (docs/DESIGN.md, the project docs).

MODULE    := sensor
SRCS      := ../../drivers/sensor.c fake_bus.c
TEST_SRCS := test_sensor.c
# -I../../drivers  : hal.h, sensor.h (the driver under test)
# -I.              : fake_bus.h (test-only HAL implementation)
# -I..             : gov_test.h (shared host harness in tests/)
INCLUDES  := -I../../drivers -I. -I..

include ../host.mk
