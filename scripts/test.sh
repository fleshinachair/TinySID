#!/usr/bin/env bash
# Builds and runs the unit tests natively under UBSan (+ ASan where the
# toolchain supports it), and validates module.json.
#
#   CC="gcc"   override the host compiler (default: gcc, clang, then `zig cc`)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/tests"
GEN="$ROOT/build/gen"
PY=""
for cand in python3 python py; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import sys' >/dev/null 2>&1; then PY="$cand"; break; fi
done
[ -n "$PY" ] || { echo "python not found" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT" "$GEN"
cp "$ROOT/src/dsp/demo_sid.h" "$GEN/default_sid.h"       # tests always run against the bundled demo
"$PY" "$ROOT/tools/gen_ui_json.py" "$ROOT/module.json" "$GEN/ui_json.h" --json="$GEN/json"
"$PY" "$ROOT/tools/gen_ui_chain.py" "$ROOT/src/ui/ui_chain.template.js" "$ROOT/canvas.js" "$GEN/json/ui_hierarchy.json" "$GEN/ui_chain.js"

if [ -n "${CC:-}" ]; then read -r -a CCV <<< "$CC"
elif command -v gcc >/dev/null 2>&1; then CCV=(gcc)
elif command -v clang >/dev/null 2>&1; then CCV=(clang)
elif command -v zig >/dev/null 2>&1; then CCV=(zig cc)
else echo "no C compiler found" >&2; exit 1; fi

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

# Pick the strongest sanitizer set that actually links and runs here.
SAN=""
cat > "$OUT/probe.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
int main(void) { char *p = malloc(8); p[0] = 1; int r = p[0]; free(p); return r - 1; }
C
for cand in "-fsanitize=address,undefined" "-fsanitize=undefined"; do
    if "${CCV[@]}" $cand "$OUT/probe.c" -o "$OUT/probe$EXE" >/dev/null 2>&1 && "$OUT/probe$EXE" >/dev/null 2>&1; then
        SAN="$cand"; break
    fi
done
if [ -z "$SAN" ]; then echo "warning: no sanitizer support in this toolchain" >&2; fi
echo "compiler:   ${CCV[*]}"
echo "sanitizers: ${SAN:-none}"
case "$SAN" in *address*) ;; *) echo "note: ASan unavailable, so leaks are checked only by the counting allocator in test_engine" ;; esac
echo

CFLAGS=(-std=c11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer -I"$ROOT/src/dsp" -I"$GEN" -I"$ROOT/tests"
        "-DTINYSID_ASSET_DIR=\"$ROOT/assets\"")
if [ -n "$SAN" ]; then CFLAGS+=($SAN -fno-sanitize-recover=undefined); fi
CORE=("$ROOT"/src/dsp/{cpu6502,sid,psid,tinysid}.c)

"${CCV[@]}" "${CFLAGS[@]}" "$ROOT/tests/test_cpu.c"    "$ROOT/src/dsp/cpu6502.c" -o "$OUT/test_cpu$EXE" -lm
"${CCV[@]}" "${CFLAGS[@]}" "$ROOT/tests/test_psid.c"   "${CORE[@]}" -o "$OUT/test_psid$EXE" -lm
"${CCV[@]}" "${CFLAGS[@]}" "$ROOT/tests/test_engine.c" "${CORE[@]}" -o "$OUT/test_engine$EXE" -lm
"${CCV[@]}" "${CFLAGS[@]}" "$ROOT/tests/test_plugin.c" "${CORE[@]}" "$ROOT/src/dsp/plugin.c" -o "$OUT/test_plugin$EXE" -lm

status=0
for t in test_cpu test_psid test_engine test_plugin; do
    echo "== $t"
    "$OUT/$t$EXE" || status=1
    echo
done

echo "== piano roll page (node; the host-code test needs host_ref/, see tools/fetch_host_ref.sh)"
if command -v node >/dev/null 2>&1; then
    node "$ROOT/tests/test_piano_roll.js" | tail -n 1
    node "$ROOT/tests/test_piano_roll.js" >/dev/null || status=1
    node "$ROOT/tests/test_host_pages.mjs" || status=1
    node "$ROOT/tests/test_ui_chain.mjs" || status=1
else
    echo "node not found: piano roll screen test skipped"
fi
echo

echo "== headless pre-scan benchmark (no sanitizers)"
BENCH_ASSETS=()
for f in "$ROOT"/assets/*.sid; do [ -f "$f" ] && BENCH_ASSETS+=("$f"); done
"${CCV[@]}" -std=c11 -O2 -Wall -Wextra -Werror -I"$ROOT/src/dsp" -I"$GEN" "$ROOT/tests/bench_scan.c" "${CORE[@]}" -o "$OUT/bench_scan$EXE" -lm
"$OUT/bench_scan$EXE" "${BENCH_ASSETS[@]}" || status=1
echo

echo "== manifest"
"$PY" "$ROOT/tools/check_manifest.py" "$ROOT/module.json" || status=1

if [ $status -eq 0 ]; then echo; echo "ALL TESTS PASSED"; else echo; echo "TESTS FAILED"; fi
exit $status
