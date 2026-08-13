# BOARD_APPENDIX — taking governor to real silicon (HUMAN-OPTIONAL)

> **STATUS: OPTIONAL. HUMAN-RUN. NOT-YET-PHYSICALLY-EXECUTED.**
>
> governor is verified **emulation-first**. Everything the project gates on runs
> and passes without any physical board:
> - host-portable modules (`lib/{proto,control,telem}`, `lib/safety`, `lib/config`,
>   `drivers/sensor`) — host unit + property tests + libFuzzer/ASan/UBSan;
> - the on-target logic — ztest on **native_sim** (CI) and **qemu_cortex_m3** (local,
>   real ARM Cortex-M3 cross-compile);
> - the emulated-hardware fault matrix — a **Renode STM32F103** machine, gating in CI.
>
> This appendix is **not** part of that verification. It documents *how a human
> would take the exact same firmware image to a physical board if they wanted to.*
> **Nobody has run governor on physical hardware.** The pin map and steps below are
> derived from the board DTS (`boards/stm32f103_mini.overlay` + `.conf`) and
> standard STM32F103 practice — treat them as a starting point, not a tested
> procedure. See **§8 Honest caveats**.

---

## 1. Why this is optional

The whole point of the emulation-first approach (see `README.md`, `docs/DESIGN.md`,
`renode/README.md`) is that the fault-injection matrix, the link protocol, the
safety state machine, and the control loop are all provable **deterministically**
without hardware. Renode runs the *same* Cortex-M3 STM32F103 image that a physical
blue-pill would run, so "does it run on the chip" is already answered for the parts
that emulation can model faithfully. Physical bring-up buys you the handful of
things emulation genuinely cannot model (see **§7**) — nothing the project requires.

If you never touch a board, governor is complete and green. This appendix exists
so that "flash it to a $3 board" is a documented, low-surprise afternoon rather
than a research project.

---

## 2. Board choice

Both of these are **real, upstream-supported Zephyr boards** and both are a
**Cortex-M3 STM32F103**, which is exactly what the Renode `stm32f103.repl` machine
models — so **the same firmware image and the same source run on all three**
(qemu_cortex_m3 aside, which is a different SoC). Pick either:

| Board | Zephyr board name | Approx cost | ST-Link | Notes |
|-------|-------------------|-------------|---------|-------|
| STM32F103 "blue pill" | `stm32f103_mini` | ~$3–5 | external needed | The overlay in `boards/` targets this exact board. Cheapest path. |
| Nucleo-F103RB | `nucleo_f103rb` | ~$12–15 | **built-in (ST-Link/V2-1)** | On-board debugger + USB; no separate probe needed. Easiest path. |

- **Recommended for least friction:** Nucleo-F103RB — the ST-Link is on the board,
  so you flash over one USB cable.
- **Recommended for cheapest / closest to the repo's overlay:** the blue-pill
  (`stm32f103_mini`), which the committed overlay was written against.
- **Mission default targets** are `native_sim` + `qemu_cortex_m3` + the Renode
  STM32 machine (see `the design notes`, `docs/ENV.md`). A physical board is a
  P6-stretch item, never a required target.

> If you use `nucleo_f103rb`, the `boards/stm32f103_mini.overlay` / `.conf` are not
> auto-applied. Either add an equivalent `nucleo_f103rb.overlay`/`.conf` (same
> `chosen`/`aliases`/partition content) or pass the overlay explicitly. The pin map
> below is written for the blue-pill DTS; the Nucleo maps the same peripherals to
> the same STM32F103 pins, but confirm against its board DTS before wiring.

---

## 3. Pin map (from `boards/stm32f103_mini.overlay` + board DTS)

The overlay assigns roles; the physical pins are the STM32F103 defaults for those
peripherals (blue-pill). Confirm against the board silk-screen before wiring.

| Function | Peripheral | STM32F103 pins | Role in governor |
|----------|-----------|----------------|------------------|
| Console / log | `usart1` | **PA9** (TX), **PA10** (RX) | Human-readable console + `LOG_INF` status line (`zephyr,console`). The Renode/Robot scenarios grep this line. Board default; the overlay leaves it as console. |
| Framed link | `usart2` | **PA2** (TX), **PA3** (RX) | The binary framed telemetry/command link to the ground station (`chosen govtelemetry,link-uart`, alias `gov-link-uart`), 115200 8N1. Kept separate from the console so logs and binary frames never collide on one wire. |
| Sensor bus | `i2c1` | **PB8** (SCL), **PB9** (SDA) — remapped | Temperature-sensor bus at address **0x48** (`GOV_SENSOR_I2C_ADDR`), standard bit-rate. The HAL adapter (`drivers/hal_zephyr.c`) binds the `i2c1` controller and addresses 0x48 directly. |
| Watchdog | `iwdg` (`watchdog0`) | internal (no pins) | Independent watchdog; `WDT_FLAG_RESET_SOC`, ~500 ms window. Fed only when the whole system is scheduling correctly (health task). A hang → SoC reset → sticky `FAULT_WATCHDOG`. |
| Flash / debug | SWD | **SWDIO**, **SWCLK** (+ GND, 3V3) | Program/debug interface used by ST-Link. |

Notes:
- `i2c1` is the **remapped** mapping (PB8/PB9) rather than the default PB6/PB7 —
  this is what the board DTS/overlay selects. Verify against your board's silk
  before wiring the sensor.
- `usart2` at 115200 is set explicitly in the overlay (`current-speed = <115200>`).
- The config-persistence storage partition lives at flash `0x1f000` (4 KB, two
  2 KB pages at the top of the 128 KB flash) — internal flash, no pins.

---

## 4. Hardware you need

| Item | Needed? | Notes |
|------|---------|-------|
| STM32F103 board | required | Blue-pill (`stm32f103_mini`) or Nucleo-F103RB. |
| ST-Link V2 probe | required for blue-pill; **built-in on Nucleo** | Flash + SWD debug. Any clone ST-Link V2 works with OpenOCD/pyOCD. |
| USB-UART adapter (**3.3 V**) | required to run the ground station | Wire to `usart2` (PA2/PA3) for the framed link. **Must be 3.3 V logic** — 5 V will damage the STM32. Cross TX↔RX and share GND. |
| I2C temp sensor @ 0x48 | optional | An LM75 / TMP102 (or compatible) at address 0x48 exercises the real sensor path. **Without one**, the firmware still boots and runs: the sensor driver reports **dropout** (no ACK/data at 0x48) and the safety machine handles it exactly as fault F01 — a legitimate, observable state, not a crash. |
| Jumper wires / breadboard | as needed | For the UART and optional sensor. |

Wiring the `usart2` link (3.3 V!):

```
  STM32F103            USB-UART (3.3 V)
  PA2 (usart2 TX) ───▶ RX
  PA3 (usart2 RX) ◀─── TX
  GND             ─── GND
```

---

## 5. Build + flash (real commands)

The pinned toolchain (Zephyr v3.7.0, SDK 0.16.8 `arm-zephyr-eabi`, west, CMake
3.31 in the venv) already covers this board — it's the same ARM cross-compile the
qemu/Renode targets use. See `docs/ENV.md`.

```bash
# From the repo root, once per shell:
source tools/env.sh                       # PATH, ZEPHYR_BASE, ZEPHYR_SDK_INSTALL_DIR, venv cmake

# --- blue-pill ---
west build -b stm32f103_mini .            # picks up boards/stm32f103_mini.overlay + .conf automatically
west flash --runner openocd               # or: west flash --runner stlink / pyocd

# --- Nucleo-F103RB (built-in ST-Link) ---
west build -b nucleo_f103rb .             # see §2 note: supply an equivalent overlay/.conf
west flash                                # default runner (openocd) drives the on-board ST-Link
```

- On the blue-pill with a clone ST-Link, `--runner openocd` is the usual reliable
  path; `stlink` (ST's own tooling) or `pyocd` also work if installed.
- Nucleo's on-board ST-Link means `west flash` "just works" over the single USB
  cable — no external probe.
- After flashing, open the **console** on `usart1` (PA9/PA10, or the Nucleo's
  ST-Link VCP) at 115200 8N1 to see `governor boot: node online` and the
  `GOV state=... faults=0x... meas=... out=...` status line.

---

## 6. Running the ground station against real hardware

The Python ground station lives in `host/governor_gs`. Its protocol layer
(`protocol.py`) is a byte-for-byte mirror of `PROTOCOL_SPEC.md` and is transport-
agnostic: `ScenarioRunner` drives a `ReliableEndpoint` over any `Transport`.

**Today it ships two transports** (`host/governor_gs/scenario.py`):
- `LoopbackTransport` — in-process, deterministic, virtual-time (used by the host
  tests);
- `SocketTransport` — a **TCP** stream (what the Renode `.resc` exposes on
  `tcp:3456`, and what a native_sim socket bridge would use).

**Honest gap — a serial transport is not yet written.** To talk to a physical
board you wire the USB-UART adapter to `usart2` (§4) and point the ground station
at the serial device (e.g. `/dev/tty.usbserial-XXXX` on macOS, `/dev/ttyUSB0` on
Linux) at 115200 8N1. That needs a small **`SerialTransport`** that implements the
same `Transport` interface (`send(bytes)` / non-blocking `recv() -> bytes`) on top
of **pyserial**. It's a **documented TODO, roughly ~20 lines, and not yet
written** — the shape is identical to `SocketTransport` (wrap the device, set it
non-blocking / zero read-timeout, drain in `recv`). Once added:

```python
# sketch — NOT yet in the repo (pyserial dependency + SerialTransport class TODO)
# import serial
# from governor_gs.scenario import ScenarioRunner
# t = SerialTransport(serial.Serial("/dev/tty.usbserial-XXXX", 115200, timeout=0))
# runner = ScenarioRunner(transport=t)
# ... runner.send_emergency_stop(); runner.advance(...); assert ...
```

Until that lands, the ground station can still be driven against the **Renode TCP
bridge** (`tcp:3456`) via the existing `SocketTransport` — which is how the
emulated-hardware link scenarios already run. The physical-serial path is the only
missing piece, and it is additive (no protocol change).

---

## 7. What's faithful on silicon that emulation cannot model

This is the honest flip side of the emulation-limits notes in `renode/README.md`.
On real hardware you *additionally* get, for free, the things Renode had to
surrogate or could not model:

| Aspect | Emulation (Renode/QEMU) | Real STM32F103 silicon |
|--------|-------------------------|------------------------|
| RCC clock tree | `Python.PythonPeripheral` surrogate forces HSE/PLL-ready bits so the STM32F1 clock driver proceeds. | Real RCC, real HSE crystal + PLL lock. **The surrogate is unnecessary — the real peripheral exists.** |
| IWDG watchdog | `Python.PythonPeripheral` surrogate (Renode 1.16.1 ships no STM32 IWDG); never auto-trips, F10 reset driven explicitly. | Real independent watchdog with **real reset timing**; a genuine hang trips it on its own. Surrogate unnecessary. |
| Flash erase / NVS-across-reset | No STM32F1 flash-erase controller model; `MappedMemory` zero-fills instead of erase-to-0xFF, so **NVS-survives-a-full-reset can't be faithfully emulated** (torn-write invariant proven exhaustively in the host layer instead). | Real flash write/erase with true erase-to-0xFF. **NVS actually survives a reset** — `boot_count` increments persist across power cycles; the F15 persistence story completes end-to-end on hardware. |
| Control-loop timing | Virtual time — validates period *structure* and jitter *logic*, never silicon performance. | True clock jitter, real interrupt latency, real scheduling under actual load — the honest "silicon performance" number emulation is explicitly forbidden from claiming. |
| I2C bus | fake_bus / scripted ACK-NAK for F01–F05 (Renode 1.16.1 has no scriptable-NAK I2C sensor). | Real I2C timing, real bus errors, real clock-stretching / arbitration from a physical sensor at 0x48. |

**In short:** on silicon the RCC/IWDG/flash Python surrogates in `governor.repl`
simply aren't needed — the real peripherals are present — and the one invariant
emulation *couldn't* fully close (NVS surviving a hard reset) becomes directly
observable. Nothing regresses; a few things that were proven in a different layer
become provable on the metal.

---

## 8. Honest caveats (read before you wire anything)

- **Not yet run on physical hardware.** governor is emulation-verified. No line of
  this appendix has been executed on a real board. The pin map and commands are
  derived from the DTS (`boards/stm32f103_mini.overlay` + `.conf`) and standard
  STM32F103 practice.
- **A real bring-up may need minor tweaks**, e.g.:
  - **Clock source:** the blue-pill's HSE crystal presence/value can vary between
    clone boards; if the clock driver stalls, check the HSE config vs. the actual
    populated crystal (this is precisely the RCC behavior the Renode surrogate
    papers over).
  - **Sensor address / part:** confirm your temp sensor actually answers at 0x48
    (some parts strap it via address pins) and matches the 2-byte register read the
    driver expects (`GOV_SENSOR_I2C_REG`, `read_len = 2`).
  - **I2C remap:** verify PB8/PB9 vs. PB6/PB7 against your specific board's DTS.
  - **Flash runner:** clone ST-Links sometimes need `--runner openocd` (or a
    specific OpenOCD interface config) rather than the default.
  - **usart2 pins vs. USB:** on some blue-pill clones PA2/PA3 or the USB pins are
    shared with other functions — check the silk.
- **The ground-station serial path is a TODO** (§6): the `SerialTransport`/pyserial
  piece is not written. Until it is, drive the link via the Renode TCP bridge with
  the existing `SocketTransport`.
- **3.3 V only** on the `usart2` USB-UART adapter — a 5 V adapter can damage the MCU.

None of the above blocks the emulation-verified mission; they are the expected,
documented rough edges of a first physical bring-up.
