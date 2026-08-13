# Security Policy

governor is a portfolio / educational embedded-firmware project: a Zephyr RTOS
telemetry-and-control node verified emulation-first (QEMU + Renode STM32). It is
**not a deployed service** and has no production users, but the link protocol is
attacker-facing input by design, so it is engineered and continuously tested to
withstand malformed and hostile input. This document is honest about what that
posture does and does not cover.

## Supported versions

| Version | Supported |
|---------|-----------|
| v1.x (`main`) | Yes — active development, all fixes land here |
| < v1.0 (pre-release tags) | No |

There is a single supported line: the tip of `main`. Fixes are not backported.

## Security posture — what is actually verified

These are real, CI-gated properties, not aspirations:

- **The frame parser is continuously fuzzed.** `fuzz/fuzz_frame.c` drives the
  byte-at-a-time frame decoder (`gov_decoder_push`, PROTOCOL_SPEC §4) under
  **libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer**. The extended
  sweep reached **178,832,918 executions with zero crashes, leaks, overreads, or
  UB**; the corpus (1112 files) is committed, and a 60 s fuzz gate runs on every
  push. The decoder must survive *any* input with no out-of-bounds access and no
  hang — that is the pass criterion (`config/registry.md` §5). Every delivered
  `(payload, len)` pair is fully read in the fuzz harness so ASan catches a bogus
  pointer/length the decoder might hand out.

- **No dynamic allocation after init.** There is **zero heap use in the steady
  state** (`CONFIG_HEAP_MEM_POOL_SIZE=0`, verified; `docs/RULES.md` R1). All
  frame handling uses fixed, bounded static buffers — the full frame is capped at
  **`GOV_FRAME_MAX` = 72 bytes** (SOF + a `GOV_MAX_PAYLOAD` = 64 payload). A peer
  cannot drive the node into unbounded memory growth or fragmentation; the memory
  footprint is the same under a flood of garbage as at idle. This is a
  deliberate DoS-resistance property of the static-allocation discipline.

- **Bounded, length-checked parsing.** Every buffer and loop that touches
  external input has a fixed bound and is length-checked before use
  (`docs/RULES.md` R8); the decoder resynchronizes on the start-of-frame byte
  after garbage and never delivers a partial or over-long frame (fault-matrix
  rows F06, F09).

- **Malformed input is rejected, never trusted.** A CRC mismatch drops the
  frame and increments a counter; a corrupt or torn config reload falls back to
  the last valid slot (A/B double-buffer, F15). Faults are flagged in telemetry,
  never silently swallowed.

## Explicit non-goal: CRC-16 is integrity, NOT authentication

The link uses **CRC-16/CCITT-FALSE** for framing. This is an **integrity check
against accidental corruption** (line noise, bit flips, truncation) — it is
**NOT** a cryptographic authentication or message-authentication mechanism, and
it does **not** protect against a deliberate adversary.

A CRC is trivially forgeable: an attacker who can write bytes to the link UART
can craft a frame with a valid CRC, replay a captured frame, or recompute the
CRC over modified contents. governor has **no confidentiality, no
authentication, and no replay protection** on the link. This is called out
explicitly because pretending a CRC provides security would be the dishonest
failure mode this project exists to avoid.

**Threat model, stated plainly:** the link is trusted at the physical layer. The
protocol defends against a *faulty or noisy* channel and a *malformed* byte
stream — not against a malicious party with write access to the wire. Deploying
this on an untrusted link would require adding a real authenticated/encrypted
transport, which is out of scope for this project.

## Scope

**In scope** (report these):

- Memory-safety or undefined-behavior defects in the host-portable protocol /
  parser (`lib/proto`) or any `lib/**` module — a buffer overread, out-of-bounds
  write, integer overflow, or UB the fuzzer/sanitizers should have caught.
- A malformed frame that causes a crash, hang, unbounded resource use, or a
  partial/incorrect delivery.
- A safety-state or fault-flag defect where a fault path fails to reach a
  defined safe state or is not flagged in telemetry.

**Out of scope:**

- The absence of link authentication/encryption/replay protection — this is a
  documented, intentional non-goal (see above), not a vulnerability.
- Anything requiring physical write access to the wire being treated as an
  attack (that is the trusted physical layer of the threat model).
- Issues in emulation-only surrogates (the Renode RCC/IWDG/flash Python
  peripherals, `renode/governor.repl`) — these exist to make the driver run in
  the emulator and are not shipped code.
- Silicon-specific behavior — this node has not been run on physical hardware;
  see the "Honest emulation limits" section of the README.

## Reporting a vulnerability

This is a single-maintainer educational project. To report a security issue:

- **Preferred:** open a [private security advisory](https://github.com/billdmar/governor/security/advisories/new)
  on the GitHub repository, or contact the repository owner (**@billdmar**) via
  GitHub.
- For a non-sensitive issue, a regular GitHub issue is fine.

Please include a reproducer where possible — ideally a corpus input for
`fuzz/fuzz_frame.c` or a failing host test — since the whole verification model
here is reproduce-then-fix. There is no formal SLA or bounty; this is a personal
project, and reports will be triaged on a best-effort basis. A confirmed defect
gets a regression test (and, for a parser bug, a committed corpus entry) so it
can never silently return.
