# DEMO — governor in action

Real, unedited transcripts captured from this repo. Reproduce them with the
commands shown. All timing is **virtual time under QEMU/Renode** (logic/structure
validation, not silicon performance — see the README's honest-limits note).

---

## 1. Boot → RUN → track setpoint (QEMU, Cortex-M3)

```console
$ source tools/env.sh
$ west build -b qemu_cortex_m3 -t run .
*** Booting Zephyr OS build 36940db938a8 ***
I: governor boot: node online
I: state=RUN setpoint=50
I: GOV state=RUN faults=0x0 meas=0 out=0
I: GOV state=RUN faults=0x0 meas=49 out=50
I: GOV state=RUN faults=0x0 meas=49 out=50
I: GOV state=RUN faults=0x0 meas=49 out=50
```

The safety SM reaches **RUN** after self-test (transition T1); the PID loop
drives the simulated plant from 0 to the **setpoint of 50** and holds it (49–50)
with the actuator at ~50; `faults=0x0` throughout. The structured
`GOV state=… faults=0x… meas=… out=…` line is the machine-readable status the
fault scenarios assert against.

---

## 2. Full node on a real STM32F103 (Renode) with persistence

```console
$ renode renode/governor.resc   # then: start
*** Booting Zephyr OS build 36940db938a8 ***
I: governor boot: node online
I: config: setpoint=50 boot_count=0 (persisted)   # settings/NVS on STM32 flash
I: state=RUN setpoint=50
I: GOV state=RUN faults=0x0 meas=0 out=0
```

Same firmware image as an STM32F103 build. The config subsystem initializes on
the emulated STM32 internal flash and restores the persisted setpoint (F15
on-target integration; the byte-exact torn-write guarantee is proven in
`tests/config`).

---

## 3. The fault-injection matrix (Renode STM32F103, CI-gating)

```console
$ renode-test renode/*.robot
+++++ Finished test 'boot.Boot Reaches RUN On Emulated STM32'                    ... OK
+++++ Finished test 'boot.Emits Structured Telemetry Status Line'                ... OK
+++++ Finished test 'system_faults.F13 Operator Emergency Stop Latches SAFE_STOP' ... OK
+++++ Finished test 'system_faults.F10 Watchdog Reset Boots INIT With Sticky Cause' ... OK
+++++ Finished test 'link_faults.F09 Garbage Burst Then Valid Frame Resyncs'     ... OK
+++++ Finished test 'link_faults.F07 Link Starvation Degrades Then Node Stays Safe' ... OK
+++++ Finished test 'persistence.F15 Config Subsystem Initializes And Loads On Target' ... OK
+++++ Finished test 'persistence.F16 Reset Mid In-Flight Frame Recovers Clean'   ... OK
Tests finished successfully :)
```

Each scenario injects a registered fault (`config/registry.md`) on the emulated
STM32 and asserts the required safe-state outcome on the console UART. Example —
**F13**: the ground station writes a raw operator-e-stop command frame
(`7E 01 04 00 00 01 E5 5C 0C`) to the link UART; the node decodes it and the
safety SM latches **SAFE_STOP** (transition T8, `FAULT_OPERATOR_STOP`). The
remaining rows (F01–F06, F08, F11, F12, F14) are proven deterministically in the
host + on-target ztest layers — see `renode/README.md` for the layered-proof map.

---

## 4. Host verification (protocol, control, safety, config)

```console
$ make -C tests/proto CC=cc test
== 1146 checks, 0 failure(s) ==          # framing round-trip, bit-flip reject, resync, dedup

$ make -C tests/safety CC=cc test
== 105 checks, 0 failure(s) ==           # every safety transition T0–T12 + invariants

$ make -C tests/config CC=cc test
== 150 checks, 0 failure(s) ==           # A/B torn-write swept at EVERY byte offset (F15)

$ bash tools/coverage.sh
  855/871 lines = 98.16%  (target >= 90%)
```

## 5. Protocol fuzzing (libFuzzer + ASan + UBSan)

```console
$ clang -fsanitize=fuzzer,address,undefined -I lib/proto -I lib/common \
    fuzz/fuzz_frame.c lib/proto/frame.c lib/proto/crc16.c -o fuzz_frame
$ ./fuzz_frame -max_total_time=600 fuzz/corpus
...
Done 178832918 runs in 601 second(s)     # zero crashes / leaks / overreads
```

## 6. Control-loop timing + stack margins (virtual time)

`run_timing.sh` measures per-task stack high-water margins and loop-cadence
stability. Per-run period *counts* scale with the wall-clock capture window, so
they vary between runs — the invariants (100.00% adherence, 0 missed deadlines,
every task ≥25% stack headroom) do not.

```console
$ bash tools/timing/run_timing.sh
  control     1024   unused 824   80.5%  PASS     (bound >= 25%)
  link_rx     1024   unused 840   82.0%  PASS
  link_tx     1024   unused 896   87.5%  PASS
  telemetry   1024   unused 648   63.3%  PASS
  health       768   unused 640   83.3%  PASS
  RESULT: PASS — all registered threads meet the 25% margin  (EMULATION / virtual time)
  cadence stability bound: jitter <= 5.0% of mean  ->  PASS
```

True control-period adherence needs the per-tick trace (off in shipped builds,
`CONFIG_GOV_TIMING_TRACE`); enable it and re-run the parser:

```console
$ west build -b qemu_cortex_m3 -p always -d build_qemu_timing . -- \
    -DCONFIG_GOV_TIMING_TRACE=y -DCONFIG_LOG_MODE_MINIMAL=n -DCONFIG_LOG_MODE_DEFERRED=y
$ ( west build -d build_qemu_timing -t run >run.log 2>&1 ) & sleep 14; pkill -f qemu-system-arm
$ python3 tools/timing/measure_loop.py run.log
  periods=67345  mean=10.000 ms  min=10  max=10  stdev=0.000
  adherence: 100.00%  (bound >= 99.0%)
  missed deadlines: 0  (bound = 0 nominal)
```
