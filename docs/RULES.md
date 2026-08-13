# RULES — MISRA-C-inspired rule subset (governor)

A pragmatic subset of MISRA-C:2012 intent, enforced by `-Wall -Wextra -Werror`,
clang-tidy, and cppcheck on the **host-portable C** (`lib/**`). Each rule states
its rationale; deviations are logged at the bottom with justification (the MISRA
"deviation" discipline). This is a subset, honestly labeled — not full MISRA
certification.

| # | Rule | Rationale | Enforced by |
|---|------|-----------|-------------|
| R1 | No dynamic memory after init (`malloc`/`calloc`/`realloc`/`free`, `k_malloc`). | Bounded, deterministic memory; no fragmentation/OOM in steady state. | cppcheck + grep check + `CONFIG_HEAP_MEM_POOL_SIZE=0` |
| R2 | All warnings are errors (`-Wall -Wextra -Werror`, `-Wconversion` on portable libs). | A warning is a latent bug; fix at the source. | compiler flags |
| R3 | Fixed-width integer types (`uint8_t`, `int32_t`…), never bare `int` for wire/state data. | Portable, unambiguous sizes across host/target. | clang-tidy, review |
| R4 | Every `switch` on an enum handles all cases or has an explicit `default` that flags an error. | No silent fall-through on an unexpected state/event (SAFETY_SM T12). | `-Wswitch-enum`, review |
| R5 | No implicit narrowing conversions; casts are explicit and commented. | Silent truncation is a classic defect class. | `-Wconversion` |
| R6 | Functions single-exit where practical; all error paths set a status, none swallow errors. | Traceable control flow; no silent failure (mission anti-goal). | review |
| R7 | No recursion in firmware/portable modules. | Bounded stack depth (measured margins, TASKS §3). | cppcheck, review |
| R8 | Fixed bounds on every buffer/loop touching external input; length-checked before use. | Fuzz-safety of the decoder (PROTOCOL_SPEC §4); no overreads. | ASan/UBSan, review |
| R9 | `const`-correctness on read-only params; `static` for file-local linkage. | Minimal surface, compiler-checked immutability. | clang-tidy |
| R10 | No undefined behavior: no signed overflow reliance, no aliasing violations, initialized reads only. | Caught by UBSan in the fuzz + test builds. | UBSan |

## clang-tidy / cppcheck configuration
- clang-tidy checks: `.clang-tidy` at repo root (bugprone-*, cert-*, misc-*,
  readability subset, `-Werror` on the enabled set).
- cppcheck: `--enable=warning,style,performance,portability --error-exitcode=1`
  with an inline suppression policy (every suppression carries a reason).

## Deviations (justified)
- **cppcheck `constParameterCallback`** — `drivers/hal_zephyr.c` `zbus_read/write/probe`.
  cppcheck suggests declaring the `ctx` parameter `const`. Rejected: these are
  `struct gov_bus` vtable callbacks (drivers/hal.h); their signature must match
  the function-pointer type exactly, and the host `fake_bus` implementation
  mutates its ctx. Making `ctx` const would break the ABI. Suppressed inline
  with a documented reason (R9 const-correctness yields to the vtable contract).
