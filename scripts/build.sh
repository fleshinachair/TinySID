#!/usr/bin/env bash
# Builds build/TinySID.tar.gz (dsp.so + module.json) for Move (aarch64 Linux).
#
#   CC="aarch64-linux-gnu-gcc"   override the cross compiler
#   default: aarch64-linux-gnu-gcc if installed, else `zig cc -target aarch64-linux-gnu`
#
# The default tune is assets/Commando.sid when that file exists, otherwise the
# bundled original demo (src/dsp/demo_sid.h). RELEASE=1 never embeds Commando.sid, so the
# tarball is safe to publish.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
GEN="$BUILD/gen"
STAGE="$BUILD/stage/tinysid"
PY=""
for cand in python3 python py; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'import sys' >/dev/null 2>&1; then PY="$cand"; break; fi
done
[ -n "$PY" ] || { echo "python not found" >&2; exit 1; }

rm -rf "$BUILD"
mkdir -p "$GEN" "$STAGE"

if [ -f "$ROOT/assets/Commando.sid" ] && [ "${RELEASE:-0}" != "1" ]; then
    "$PY" "$ROOT/tools/embed_sid.py" "$ROOT/assets/Commando.sid" "$GEN/default_sid.h"
    echo "default tune: assets/Commando.sid"
    echo "WARNING: this build embeds Commando.sid (copyrighted) - for local use only, do not share the tarball"
else
    cp "$ROOT/src/dsp/demo_sid.h" "$GEN/default_sid.h"
    echo "default tune: bundled demo"
fi

if [ -n "${CC:-}" ]; then
    read -r -a CCV <<< "$CC"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    CCV=(aarch64-linux-gnu-gcc)
elif command -v zig >/dev/null 2>&1; then
    CCV=(zig cc -target aarch64-linux-gnu)
else
    echo "no cross compiler: install aarch64-linux-gnu-gcc or zig, or set CC" >&2
    exit 1
fi

"$PY" "$ROOT/tools/check_manifest.py" "$ROOT/module.json"
"$PY" "$ROOT/tools/gen_ui_json.py" "$ROOT/module.json" "$GEN/ui_json.h" --json="$GEN/json"
"$PY" "$ROOT/tools/gen_ui_chain.py" "$ROOT/src/ui/ui_chain.template.js" "$ROOT/canvas.js" "$GEN/json/ui_hierarchy.json" "$STAGE/ui_chain.js"

"${CCV[@]}" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared -fvisibility=hidden \
    -I"$ROOT/src/dsp" -I"$GEN" \
    "$ROOT"/src/dsp/{cpu6502,sid,psid,tinysid,plugin}.c \
    -Wl,--strip-all -o "$STAGE/dsp.so" -lm

cp "$ROOT/module.json" "$STAGE/module.json"
cp "$ROOT/canvas.js" "$STAGE/canvas.js"              # the piano roll page (also inlined into ui_chain.js)
cp "$ROOT/help.json" "$STAGE/help.json"
cp "$ROOT/release.json" "$STAGE/release.json"
tar -czf "$BUILD/TinySID.tar.gz" -C "$BUILD/stage" tinysid

echo
echo "dsp.so          $(wc -c < "$STAGE/dsp.so") bytes"
echo "module.json     $(wc -c < "$STAGE/module.json") bytes"
echo "canvas.js       $(wc -c < "$STAGE/canvas.js") bytes"
echo "ui_chain.js     $(wc -c < "$STAGE/ui_chain.js") bytes"
echo "help.json       $(wc -c < "$STAGE/help.json") bytes"
echo "release.json    $(wc -c < "$STAGE/release.json") bytes"
echo "TinySID.tar.gz  $(wc -c < "$BUILD/TinySID.tar.gz") bytes"
tar -tzf "$BUILD/TinySID.tar.gz"
