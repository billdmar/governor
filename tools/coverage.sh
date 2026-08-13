#!/usr/bin/env bash
# tools/coverage.sh — line coverage on the host-portable modules (registry §6:
# >= 90% target). Uses LLVM source-based coverage. Run from repo root:
#   source tools/env.sh && bash tools/coverage.sh
set -euo pipefail

R="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
CC="${GOV_HOST_CC:-/opt/homebrew/opt/llvm/bin/clang}"
BINDIR="$(dirname "$CC")"
PROF="${BINDIR}/llvm-profdata"
COV="${BINDIR}/llvm-cov"
OUT="${R}/build_cov"
rm -rf "$OUT"
mkdir -p "$OUT"

CF="-fprofile-instr-generate -fcoverage-mapping -I${R}/lib/common -I${R}/tests -std=c11 -O0 -g"

# module | test src | module srcs (space-sep) | include dir
run() {
	local name="$1" tsrc="$2" srcs="$3" inc="$4"
	# shellcheck disable=SC2086
	"$CC" $CF -I"$inc" "$tsrc" $srcs -o "${OUT}/t_${name}"
	LLVM_PROFILE_FILE="${OUT}/${name}.profraw" "${OUT}/t_${name}" >/dev/null 2>&1 || true
}

run proto   "${R}/tests/proto/test_proto.c" \
	"${R}/lib/proto/frame.c ${R}/lib/proto/crc16.c ${R}/lib/proto/reliable.c" "${R}/lib/proto"
run control "${R}/tests/control/test_control.c" \
	"${R}/lib/control/pid.c ${R}/lib/control/plant.c" "${R}/lib/control"
run telem   "${R}/tests/telem/test_telem.c" \
	"${R}/lib/telem/telem.c ${R}/lib/telem/ring.c ${R}/lib/telem/health.c" "${R}/lib/telem"
run safety  "${R}/tests/safety/test_safety.c" \
	"${R}/lib/safety/safety.c" "${R}/lib/safety"
# config links crc16 from proto; -I both so the header resolves.
"$CC" $CF -I"${R}/lib/config" -I"${R}/lib/proto" \
	"${R}/tests/config/test_config.c" "${R}/lib/config/config.c" "${R}/lib/proto/crc16.c" \
	-o "${OUT}/t_config"
LLVM_PROFILE_FILE="${OUT}/config.profraw" "${OUT}/t_config" >/dev/null 2>&1 || true

"$PROF" merge -sparse "${OUT}"/*.profraw -o "${OUT}/all.profdata"

echo "=== Per-module line coverage (host-portable modules) ==="
for name in proto control telem safety config; do
	"$COV" report "${OUT}/t_${name}" -instr-profile="${OUT}/all.profdata" \
		"${R}/lib/${name}" 2>/dev/null | tail -1 | \
		awk -v n="$name" '{printf "  %-8s lines: %s covered\n", n, $(NF-3)}'
done

echo "=== TOTAL across all portable modules (line coverage) ==="
# Aggregate export across all four binaries: sum covered/total lines from the
# per-binary JSON exports (a single `report` only sees one binary's objects).
python3 - "$COV" "${OUT}/all.profdata" "$OUT" "$R" <<'PY'
import json, subprocess, sys
cov, prof, out, root = sys.argv[1:5]
mods = {"proto":"t_proto","control":"t_control","telem":"t_telem","safety":"t_safety","config":"t_config"}
cov_lines = tot_lines = 0
for name, binf in mods.items():
    j = json.loads(subprocess.check_output(
        [cov, "export", f"{out}/{binf}", f"-instr-profile={prof}",
         f"{root}/lib/{name}"]))
    s = j["data"][0]["totals"]["lines"]
    cov_lines += s["covered"]; tot_lines += s["count"]
pct = 100.0 * cov_lines / tot_lines if tot_lines else 0.0
print(f"  {cov_lines}/{tot_lines} lines = {pct:.2f}%  (target >= 90%)")
PY
