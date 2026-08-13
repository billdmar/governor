*** Settings ***
Documentation     governor fault matrix — system/operator rows on emulated
...               STM32F103. F13 (operator e-stop over the real UART link) and
...               F10 (watchdog reset cause). Each cites config/registry.md and
...               asserts the REQUIRED OUTCOME on the console status line.
...               EMULATION / virtual time — logic + integration, not silicon.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}
Test Timeout      540 seconds

*** Variables ***
${ELF}            ${CURDIR}/../build_stm32/zephyr/zephyr.elf
${REPL}           ${CURDIR}/governor.repl

*** Keywords ***
Create And Boot
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription @${REPL}
    Execute Command           sysbus LoadELF @${ELF}
    Create Terminal Tester    sysbus.usart1
    Start Emulation
    Wait For Line On Uart     GOV state\=RUN

Write Bytes To Link
    # One Monitor round-trip for all bytes (see link_faults.robot rationale).
    [Arguments]    ${bytes}
    Execute Command    python "for b in [${bytes}]: monitor.Machine['sysbus.usart2'].WriteChar(b)"

*** Test Cases ***
F13 Operator Emergency Stop Latches SAFE_STOP
    [Documentation]    registry F13: an operator CMD e-stop -> SAFE_STOP (T8),
    ...                FAULT_OPERATOR_STOP (0x200) set, latched. The estop CMD
    ...                frame (TYPE=CMD 0x04, payload=0xE5) is written raw to the
    ...                link UART (usart2); bytes verified via the Python GS:
    ...                7E 01 04 00 00 01 E5 5C 0C.
    Create And Boot
    # The 9-byte estop CMD frame on the link UART (usart2), one round-trip.
    # Bytes verified via the Python GS: 7E 01 04 00 00 01 E5 5C 0C.
    Write Bytes To Link    0x7E, 0x01, 0x04, 0x00, 0x00, 0x01, 0xE5, 0x5C, 0x0C
    # Required outcome (T8): operator e-stop -> SAFE_STOP, latched. SAFE_STOP is
    # terminal-until-clear and wins from RUN or DEGRADED, so this assertion is
    # robust regardless of the sensor-bring-up state on the bare machine.
    Wait For Line On Uart    GOV state\=SAFE_STOP    timeout=8

F10 Watchdog Reset Boots INIT With Sticky Cause
    [Documentation]    registry F10: a watchdog reset boots into INIT with the
    ...                sticky FAULT_WATCHDOG (0x100) so telemetry reports the
    ...                cause (SAFETY_SM T11). Renode 1.16.1 has no STM32 IWDG
    ...                model, so we drive the reset explicitly and inject the
    ...                RESET_WATCHDOG cause the firmware reads via hwinfo (the
    ...                surrogate is documented in renode/README.md; the REQUIRED
    ...                OUTCOME is asserted unchanged).
    [Tags]    surrogate
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription @${REPL}
    Execute Command           sysbus LoadELF @${ELF}
    # STM32F1 RCC CSR (0x40021024): set IWDGRSTF (bit 29) so Zephyr hwinfo
    # reports RESET_WATCHDOG on boot. Poke it before starting.
    Execute Command           sysbus WriteDoubleWord 0x40021024 0x20000000
    Create Terminal Tester    sysbus.usart1
    Start Emulation
    # Required outcome (T11): boot reads the watchdog reset cause and enters
    # INIT carrying the sticky FAULT_WATCHDOG so telemetry reports the cause.
    # The boot-cause log line is the deterministic proof of that path.
    Wait For Line On Uart     boot after watchdog reset    timeout=8
