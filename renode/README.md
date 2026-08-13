# governor — Renode emulated-hardware fault matrix

This directory holds the Renode STM32F103 machine and the Robot Framework
scenarios that verify the governor node on **emulated hardware**. Everything
here runs in CI via `antmicro/renode-test-action` (see
`.github/workflows/renode.yml`); Renode is not required locally.

> **Honest labeling:** all Renode/QEMU results are **virtual-time / EMULATION** —
> they validate logic, integration, and timing *structure*, never silicon
> performance (the design notes, docs/DESIGN.md).

## Machine

- `governor.repl` — Renode's stock `stm32f103.repl` (a real Cortex-M3 STM32F103:
  usart1/2, i2c1, gpio, timers) **plus** two additions the stock platform lacks:
  - **`iwdg`** — a `Python.PythonPeripheral` surrogate at the real IWDG address
    `0x40003000`. Renode 1.16.1 ships no STM32 IWDG model; the surrogate accepts
    the key-register writes the Zephyr `st,stm32-watchdog` driver makes so
    bring-up succeeds. It never auto-trips — F10's reset is driven explicitly.
- `governor.resc` — interactive bring-up: loads the ELF, puts the console on
  usart1 and the framed telemetry link on usart2 as a raw TCP socket
  (`tcp:3456`) the Python ground station can drive.

## The fault matrix is proven in LAYERS (every row has a green proof)

`config/registry.md` defines 16 rows. F15/F16 are . The remaining 14 are each
proven in the layer that can demonstrate the required outcome **deterministically**:

| Row | Fault | Proven in | Why here |
|-----|-------|-----------|----------|
| F01 dropout | sensor | ztest `tests/ontarget` (fake_bus) + host `tests/drivers` + **`tests/integration`** (T6 recovery leg) | Deterministic sensor scripting; the DEGRADED→RUN recovery outcome is proven in the integration suite |
| F02 stuck | sensor | host `tests/drivers` + **`tests/integration`** (recovery) | " |
| F03 garbage/range | sensor | ztest `tests/ontarget` + host `tests/drivers` | Reject-sample/hold-last-good is driver + on-target |
| F04 I2C NAK | driver | host `tests/drivers` (fake_bus) + **`tests/integration`** (recovery) | Renode 1.16.1 has no scriptable-NAK I2C sensor; fake_bus is authoritative for the retry→flag logic, integration proves recover-on-ACK |
| F05 I2C bus error | driver | host `tests/drivers` (fake_bus) + **`tests/integration`** (T7 degrade-max escalation) | Flag in isolation; the persist→SAFE_STOP escalation outcome is proven in the integration suite |
| F06 UART bit corruption | link | ztest `tests/ontarget` + host `tests/proto` | CRC-reject is decoder logic; deterministic in ztest |
| F07 frame loss → LINK_FAULT | link | **Renode `link_faults.robot`** + host `tests/proto` | On-target ARQ starvation on the real UART |
| F08 duplicate frame | link | host `tests/proto` (dedup) | Dedup is decoder logic |
| F09 garbage burst → resync | link | **Renode `link_faults.robot`** + ztest | End-to-end resync + delivery on the real UART |
| F10 watchdog reset | system | **Renode `system_faults.robot`** | Reset-cause → sticky FAULT_WATCHDOG on real STM32 (IWDG surrogate documented above) |
| F11 deadline miss | system | ztest `tests/ontarget` + **`tests/integration`** (recover-when-met-again) | Deterministic event injection; the recovery leg is proven in the integration suite |
| F12 divergence | control | ztest `tests/ontarget` + host `tests/integration` | Deterministic plant disturbance |
| F13 operator e-stop | operator | **Renode `system_faults.robot`** + ztest | Real CMD frame over the real UART → SAFE_STOP |
| F14 self-test fail | system | ztest `tests/ontarget` | Deterministic |
| F15 reset mid config-write | persistence | host `tests/config` (torn-write property sweep) + **Renode `persistence.robot`** (on-target config subsystem init/load) | See the NVS note below |
| F16 reset mid in-flight frame | persistence+link | **Renode `persistence.robot`** + ztest/host framing | Real reset mid-frame → decoder resyncs, no partial delivery |

Plus `boot.robot`: the full node boots on the emulated STM32F103 and the safety
SM reaches RUN — the  emulated-hardware bring-up proof.

### F15 persistence — layered proof + honest NVS/flash-erase limitation
The F15 REQUIRED OUTCOME — *"a reset mid config-write leaves the stored config
either old-valid or new-valid, never corrupt"* — is proven **exhaustively and
deterministically in the host layer** (`tests/config`, `lib/config`): an A/B
double-buffer with CRC + monotonic sequence, and a property test that tears the
write at **every byte offset** and asserts the reload is always old-valid or
new-valid. On target, `app/persist.c` binds that logic to Zephyr settings/NVS on
the STM32 internal-flash `storage_partition`, and `persistence.robot` proves the
config subsystem **initializes and loads on the real STM32 flash** (the node
applies the persisted setpoint). What is *not* asserted in Renode is
NVS-survives-a-full-`machine Reset`: Renode 1.16.1 has **no STM32F1 flash-erase
controller model** (only F0/F4/H7/L0/L5/WBA exist), and its `MappedMemory`
zero-fills rather than erasing to 0xFF, so NVS's erase-to-0xFF garbage-collection
semantics across a reset aren't faithfully emulable. The `flashctl` surrogate in
`governor.repl` returns BSY-idle/unlocked so the driver runs and the config loads
on target; the torn-write guarantee itself is the host suite's job (where it's
provable to the byte). No required outcome is weakened — the invariant is proven
where it can be proven deterministically.

**Nothing is weakened or dropped.** The Renode layer proves what only real
hardware emulation can (STM32 boot, the real UART link, the watchdog reset
path); the ztest/host layers authoritatively prove the rows whose fault is
injected in pure logic (sensor validation, CRC, dedup) where determinism is
exact.

### Where each layer runs in CI (honest status)
- **ztest `tests/ontarget`** runs on **native_sim** in CI (deterministic there)
  and on **qemu_cortex_m3 locally** (`west twister -p qemu_cortex_m3`, ~15 s,
  100% pass — the real ARM cross-compiled image). qemu+twister is flaky on the
  shared GitHub runner (hangs past a 120 s harness timeout), so CI gates on
  native_sim and smoke-builds qemu; the qemu run is a local/on-target proof.
- **host suites** (`tests/{proto,control,telem,drivers,safety,integration}`,
  `host/tests`) run in CI every push.
- **Renode STM32 matrix** (this dir): runs headless (native — no xvfb) in the
  `renode-matrix` job via prebuilt Renode 1.16.1, and is **gating**. All 6
  scenarios pass: boot-to-RUN, telemetry line, F13 (operator e-stop over the
  real usart2 link), F10 (watchdog reset-cause), F09 (garbage-burst resync +
  delivery), F07 (link starvation liveness).

### The RCC clock fix (why the STM32 now boots in Renode)
Renode's stock `stm32f103.repl` models RCC only as a static Tag, so Zephyr's
STM32F1 clock driver spun forever waiting for HSE/PLL-ready and the sysclk
SWS-switch bits that never appeared — the firmware never left `clock_control_init`,
virtual time froze, and every `Wait For Line` blocked (the CI-hang root cause).
`governor.repl` replaces RCC (and IWDG) with `Python.PythonPeripheral` models
that behave as read-write registers and force the clock-ready bits set
(CR: HSIRDY|HSERDY|PLLRDY; CFGR: mirror SW→SWS; CSR: LSIRDY). With that, the node
boots to RUN on the emulated STM32 exactly as on qemu. Wall-clock safety:
`Test Timeout` in each suite + `--test-timeout` on `renode-test` convert any
mis-boot into a fast failure instead of a hang.

## Running locally (optional — CI is the source of truth)

```bash
# after `west build -b stm32f103_mini -d build_stm32`
renode-test renode/boot.robot renode/system_faults.robot renode/link_faults.robot
# or interactively:
renode renode/governor.resc   # then `start`
```
