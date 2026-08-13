<!--
governor PR — verified emulation-first, gate-by-gate (see the project docs "Acceptance criteria").
Keep the diff surgical: every changed line should trace back to the summary below.
-->

## Summary

<!-- What changed and why, in a few lines. Host-portable lib change, Zephyr adapter, Renode scenario, docs? -->

## Gate / wave

<!-- Which gate or wave this advances ( /  /  /  /  / P6), or "maintenance". -->

## Verification evidence

<!--
Paste FRESH command output — the acceptance criteria is "passes with fresh evidence", not "should pass".
Label every timing/stack number as emulated / virtual-time.
-->

```
# e.g.
# make -C tests/proto CC=cc test        → 2254 assertions, 0 failures
# west twister -p native_sim -T tests/  → N/N passed
# renode-test renode/*.robot            → 8/8 scenarios pass
# fuzz: 60s -max_total_time              → 0 crashes/leaks/overreads
```

## Reviewer checklist

<!-- Tick only what you have actually verified for this change. Cross out (~~...~~) any line that is genuinely N/A and say why. -->

- [ ] Host unit tests green (`make -C tests/<module> CC=cc test`; `west twister -p native_sim -T tests/`)
- [ ] `-Wall -Wextra -Werror -Wconversion` clean on all built targets
- [ ] clang-tidy + cppcheck clean per the configured rule set (docs/RULES.md)
- [ ] No dynamic allocation after init (no post-init `k_malloc`/`malloc`; heap budget accounted)
- [ ] Coverage held ≥ 90% on host-portable modules (report the number)
- [ ] Fuzz gate clean (libFuzzer + ASan/UBSan, corpus committed) — **required if the frame parser changed**
- [ ] No required outcome in `config/registry.md` weakened, no bound widened; any failing scenario was **fixed, not deleted**
- [ ] Frozen contracts unchanged (PROTOCOL_SPEC / SAFETY_SM / TASKS / registry) — or the change is justified below and -approved
- [ ] Docs updated (README/DESIGN/RULES current; the design decision logged in docs/DESIGN.md)
- [ ] Every emulated timing/stack figure is labeled virtual-time (never presented as silicon)
- [ ] `lib/` stays Zephyr-free (0 Zephyr includes in host-portable modules)
- [ ] Both CI workflows green: `ci` and `renode-matrix`

## Frozen-contract change justification (if any)

<!-- Fill in only if a box above is checked as a contract change. State what changed, why, and confirm no required outcome weakened / no bound widened. Otherwise: "None — additive/behavior-preserving." -->

## Linked issues

<!-- Closes #NN / Refs #NN -->
