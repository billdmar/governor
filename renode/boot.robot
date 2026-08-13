*** Settings ***
Documentation     governor — boot-to-RUN on the emulated STM32F103 (Renode).
...               Proves the full RTOS node (5 threads, safety SM, drivers,
...               watchdog, link UART) boots on real ARM STM32 hardware
...               emulation and the safety SM reaches RUN (INIT->RUN, T1). This
...               is the  emulated-hardware bring-up gate. EMULATION / virtual
...               time — validates logic + integration, not silicon timing.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}
Test Timeout      540 seconds

*** Variables ***
${ELF}            ${CURDIR}/../build_stm32/zephyr/zephyr.elf
${REPL}           ${CURDIR}/governor.repl

*** Keywords ***
Create Governor Machine
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription @${REPL}
    Execute Command           sysbus LoadELF @${ELF}
    Create Terminal Tester    sysbus.usart1

*** Test Cases ***
Boot Reaches RUN On Emulated STM32
    [Documentation]    The node boots on the real STM32F103 emulation and the
    ...                safety SM enters RUN. The "state=RUN" line is emitted by
    ...                main() right after self-test (INIT->RUN, T1), before the
    ...                worker threads start — a deterministic, sensor-independent
    ...                bring-up proof.
    Create Governor Machine
    Start Emulation
    Wait For Line On Uart     governor boot: node online
    Wait For Line On Uart     GOV state\=RUN

Emits Structured Telemetry Status Line
    [Documentation]    The telemetry thread's structured console line (the
    ...                stable format the fault scenarios assert on) appears once
    ...                the node is running.
    Create Governor Machine
    Start Emulation
    Wait For Line On Uart     GOV state\=    timeout=8
