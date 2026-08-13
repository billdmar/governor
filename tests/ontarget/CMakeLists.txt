# tests/ontarget — on-target ztest suite (native_sim + qemu_cortex_m3 via twister).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(governor_ontarget LANGUAGES C)

set(GOV_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_sources(app PRIVATE
  src/main.c
  ${GOV_ROOT}/app/node_core.c
  ${GOV_ROOT}/lib/safety/safety.c
  ${GOV_ROOT}/lib/control/pid.c
  ${GOV_ROOT}/lib/control/plant.c
  ${GOV_ROOT}/lib/telem/telem.c
  ${GOV_ROOT}/lib/proto/frame.c
  ${GOV_ROOT}/lib/proto/crc16.c
  ${GOV_ROOT}/lib/proto/reliable.c
  ${GOV_ROOT}/drivers/sensor.c
  ${GOV_ROOT}/tests/drivers/fake_bus.c
)

target_include_directories(app PRIVATE
  ${GOV_ROOT}/app
  ${GOV_ROOT}/lib/common
  ${GOV_ROOT}/lib/proto
  ${GOV_ROOT}/lib/control
  ${GOV_ROOT}/lib/telem
  ${GOV_ROOT}/lib/safety
  ${GOV_ROOT}/drivers
  ${GOV_ROOT}/tests/drivers
)
