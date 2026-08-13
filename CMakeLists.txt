# governor — top-level Zephyr application CMake.
# The host-portable libraries (lib/proto, lib/control, lib/telem, lib/safety)
# are plain C with NO Zephyr includes; here they are compiled into the firmware
# via a thin Zephyr adapter (app/main.c drives app/node_core.c). The same
# sources are built host-side (tests/, fuzz/) by their own standalone Makefiles.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(governor LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  app/node_core.c
  lib/safety/safety.c
  lib/control/pid.c
  lib/control/plant.c
  lib/telem/telem.c
  lib/telem/ring.c
  lib/telem/health.c
  lib/proto/frame.c
  lib/proto/crc16.c
  lib/proto/reliable.c
  lib/config/config.c
  drivers/sensor.c
)

# The Zephyr I2C adapter binds a real controller; compile it only where the
# board has I2C (the STM32 emulated-hardware target). qemu_cortex_m3/native_sim
# exercise the sensor logic via the host fake_bus instead.
target_sources_ifdef(CONFIG_I2C app PRIVATE drivers/hal_zephyr.c)
# Persistence adapter binds lib/config to settings/NVS; only where CONFIG_SETTINGS.
target_sources_ifdef(CONFIG_SETTINGS app PRIVATE app/persist.c)

target_include_directories(app PRIVATE
  ${CMAKE_SOURCE_DIR}/app
  ${CMAKE_SOURCE_DIR}/lib/common
  ${CMAKE_SOURCE_DIR}/lib/proto
  ${CMAKE_SOURCE_DIR}/lib/control
  ${CMAKE_SOURCE_DIR}/lib/telem
  ${CMAKE_SOURCE_DIR}/lib/safety
  ${CMAKE_SOURCE_DIR}/lib/config
  ${CMAKE_SOURCE_DIR}/drivers
)
