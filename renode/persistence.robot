*** Settings ***
Documentation     governor  recovery matrix — persistence + reset-recovery on
...               the emulated STM32F103. F15 (config survives reset; a torn
...               config write never corrupts the stored config) and F16 (reset
...               mid in-flight frame -> decoder starts clean, no partial
...               delivery, resyncs). EMULATION / virtual time. The torn-write
...               LOGIC is host-proven (tests/config, F15 property sweep); this
...               proves on-target persistence integration + reset recovery.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}
Test Timeout      540 seconds

*** Variables ***
${ELF}            ${CURDIR}/../build_stm32/zephyr/zephyr.elf
${REPL}           ${CURDIR}/governor.repl

*** Keywords ***
Create And Prep Storage
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription @${REPL}
    Execute Command           sysbus LoadELF @${ELF}
    # A machine Reset re-runs this macro (Monitor auto-runs the `reset` macro),
    # re-initializing .data/.bss like a real reboot while the flash MappedMemory
    # (incl. the NVS storage partition) survives (MappedMemory.Reset is a no-op).
    Execute Command           macro reset "sysbus LoadELF @${ELF}"
    # NVS assumes erased flash reads 0xFF; Renode MappedMemory zero-fills, so
    # pre-fill the 4 KB storage partition (0x0801F000) with 0xFF once at setup.
    Execute Command           python "for a in range(0x0801F000, 0x08020000, 4): monitor.Machine['sysbus'].WriteDoubleWord(a, 0xFFFFFFFF)"
    Create Terminal Tester    sysbus.usart1

Write Bytes To Link
    # One Monitor round-trip for all bytes (see link_faults.robot rationale).
    [Arguments]    ${bytes}
    Execute Command    python "for b in [${bytes}]: monitor.Machine['sysbus.usart2'].WriteChar(b)"

*** Test Cases ***
F15 Config Subsystem Initializes And Loads On Target
    [Documentation]    registry F15 (on-target integration half): the node's
    ...                settings/NVS-backed config subsystem initializes on the
    ...                real STM32 flash and load returns a valid config at boot
    ...                (defaults on a clean store), which the node applies as its
    ...                setpoint. The TORN-WRITE guarantee itself ("a reset mid
    ...                config write leaves old-valid or new-valid, never
    ...                corrupt") is proven exhaustively + deterministically in
    ...                the host layer (tests/config, F15 property sweep over
    ...                every byte offset). See renode/README.md for why the
    ...                survives-a-full-reset demonstration lives host-side:
    ...                Renode 1.16.1 has no STM32F1 flash-erase model, so NVS's
    ...                erase-to-0xFF semantics across machine Reset aren't
    ...                faithfully emulable — the surrogate proves the driver runs
    ...                + the config loads on target, not the flash erase cycle.
    Create And Prep Storage
    Start Emulation
    # The config subsystem initialized on the STM32 flash and load succeeded
    # (defaults on a clean store); the node applied setpoint=50 from it.
    Wait For Line On Uart     config: setpoint\=50
    Wait For Line On Uart     GOV state\=RUN

F16 Reset Mid In-Flight Frame Recovers Clean
    [Documentation]    registry F16: a reset while a framed message is partially
    ...                in flight must leave the decoder clean (HUNT_SOF) with no
    ...                partial delivery; the link re-syncs on the next whole
    ...                frame. We send a PARTIAL frame (SOF + a few header bytes,
    ...                no CRC), reset, then send a COMPLETE estop CMD frame — it
    ...                must be delivered (node -> SAFE_STOP), proving the mid-
    ...                frame partial was discarded and framing recovered.
    Create And Prep Storage
    Start Emulation
    Wait For Line On Uart     GOV state\=RUN
    # Partial frame: SOF + partial header, then nothing (interrupted).
    Write Bytes To Link       0x7E, 0x01, 0x04
    # Reset mid-frame — decoder state is wiped; must not deliver the partial.
    Execute Command           machine Reset
    Create Terminal Tester    sysbus.usart1
    Start Emulation
    Wait For Line On Uart     GOV state\=RUN
    # A COMPLETE estop CMD frame (7E 01 04 00 00 01 E5 5C 0C) — delivery after
    # the mid-frame reset proves the decoder resynced cleanly.
    Write Bytes To Link       0x7E, 0x01, 0x04, 0x00, 0x00, 0x01, 0xE5, 0x5C, 0x0C
    Wait For Line On Uart     GOV state\=SAFE_STOP
