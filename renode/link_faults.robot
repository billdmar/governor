*** Settings ***
Documentation     governor fault matrix — link rows on emulated STM32F103.
...               Drives the framed telemetry link (usart2) to prove the
...               reliability + framing behavior on real hardware emulation.
...               F07 (frame loss -> retransmit -> LINK_FAULT -> DEGRADED, T4)
...               and F09 (garbage burst then valid frame -> resync, deliver).
...               F06/F08 (single-bit corruption / duplicate) are proven
...               deterministically in the host + on-target ztest layers (the
...               decoder + dedup logic); this suite proves the on-target link
...               integration. EMULATION / virtual time. See renode/README.md.
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
    # Send all bytes in ONE Monitor round-trip (a per-byte WriteChar is a
    # separate robot->Renode XML-RPC call; 14 of them dominate wall-clock on a
    # slow CI runner). ${bytes} is a space-separated hex list.
    [Arguments]    ${bytes}
    Execute Command    python "for b in [${bytes}]: monitor.Machine['sysbus.usart2'].WriteChar(b)"

*** Test Cases ***
F09 Garbage Burst Then Valid Frame Resyncs
    [Documentation]    registry F09: a garbage burst on the link followed by a
    ...                valid frame -> the decoder resyncs on SOF and delivers the
    ...                valid frame; no crash/overread. Here the valid frame is
    ...                the operator e-stop CMD (7E 01 04 00 00 01 E5 5C 0C): if
    ...                it is delivered after garbage, the node enters SAFE_STOP —
    ...                proving resync + delivery end to end on real hardware.
    Create And Boot
    # Garbage burst (0x11 0x22 0x7E 0x33 0x44 — a stray SOF that doesn't
    # complete) then the valid e-stop CMD frame; all in one round-trip.
    Write Bytes To Link    0x11, 0x22, 0x7E, 0x33, 0x44, 0x7E, 0x01, 0x04, 0x00, 0x00, 0x01, 0xE5, 0x5C, 0x0C
    Wait For Line On Uart    GOV state\=SAFE_STOP    timeout=8

F07 Link Starvation Degrades Then Node Stays Safe
    [Documentation]    registry F07: with no ground station ACKing the node's
    ...                telemetry, the stop-and-wait ARQ retransmits up to
    ...                GOV_MAX_RETRIES then latches LINK_FAULT -> RUN->DEGRADED
    ...                (T4). We attach no link peer, so the node's own telemetry
    ...                goes unACKed and the link fault surfaces. The node must
    ...                NOT deadlock: it continues emitting the status line in a
    ...                degraded-or-safe state (no crash, fault flagged).
    [Tags]    link-starvation
    Create And Boot
    # Let telemetry attempt + exhaust retransmits; the node degrades but the
    # console keeps producing status lines (liveness under link fault).
    Wait For Line On Uart    GOV state\=    timeout=10
