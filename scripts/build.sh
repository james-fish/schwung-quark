#!/bin/sh
# Build Quark: codegen, tests, aarch64 dsp.so for the Move, the module package and the browser bench.
#
#   scripts/build.sh            everything
#   scripts/build.sh --no-test  skip tests
#   scripts/build.sh --profile  dsp.so with the read-only cpu_us key (render time in us; not for release)
#
# Needs: python3, a C compiler (cc), node (tests), zig (cross-compile + wasm):
#   brew install zig      or      python3 -m pip install ziglang     (set ZIG=/path/to/zig to override)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
TEST=1; PROFILE=0
for a in "$@"; do
  case "$a" in
    --no-test) TEST=0 ;;
    --profile) PROFILE=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
PY=${PYTHON:-python3}
CC=${CC:-cc}
find_zig() {
  if [ -n "${ZIG:-}" ]; then echo "$ZIG"; return; fi
  if command -v zig >/dev/null 2>&1; then command -v zig; return; fi
  "$PY" -c 'import ziglang, os; print(os.path.join(os.path.dirname(ziglang.__file__), "zig"))' 2>/dev/null || true
}
ZIG=$(find_zig)
[ -n "$ZIG" ] && [ -x "$ZIG" ] || { echo "zig not found: brew install zig  or  python3 -m pip install ziglang" >&2; exit 1; }
FLAGS="-std=c11 -O2 -ffast-math -fno-finite-math-only -Wall -Wextra -Wshadow"
MOD="$ROOT/modules/sound_generators/quark"
mkdir -p build "$MOD"

echo "== gen"
"$PY" tools/gen.py

if [ "$TEST" = 1 ]; then
  echo "== native tests + benchmark"
  $CC $FLAGS -I src/dsp -o build/quark_test src/dsp/*.c tests/test_main.c -lm
  ./build/quark_test
  echo "== UI drawers"
  node tests/test_ui.mjs
fi

echo "== dsp.so (aarch64-linux-gnu, zig $("$ZIG" version))"
DEFS=""
[ "$PROFILE" = 1 ] && DEFS="-DQUARK_PROFILE"
"$ZIG" cc -target aarch64-linux-gnu.2.28 $FLAGS -fPIC -shared -s -fvisibility=hidden $DEFS -ffile-prefix-map="$ROOT"=. \
  -I src/dsp -o "$MOD/dsp.so" src/dsp/*.c -lm
rm -f "$MOD/dsp.so.o"

echo "== module folder"
cp src/module.json src/help.json "$MOD/"
cp build/ui/canvas.js build/ui/cards.js "$MOD/"
cp LICENSE THIRD_PARTY_NOTICES.md "$MOD/" 2>/dev/null || true
COPYFILE_DISABLE=1 tar -czf "$ROOT/dist/quark-module.tar.gz" --exclude '.DS_Store' -C "$ROOT/modules/sound_generators" quark 2>/dev/null || {
  mkdir -p "$ROOT/dist"; COPYFILE_DISABLE=1 tar -czf "$ROOT/dist/quark-module.tar.gz" --exclude '.DS_Store' -C "$ROOT/modules/sound_generators" quark; }

echo "== quark.wasm + dist/quark.html"
"$ZIG" cc -target wasm32-wasi -mexec-model=reactor -DQUARK_WASM -std=c11 -O2 -ffast-math -fno-finite-math-only \
  -ffile-prefix-map="$ROOT"=. -I src/dsp -o build/quark.wasm src/dsp/*.c web/wasm_host.c -lm \
  -Wl,--no-entry -Wl,-z,stack-size=1048576 -Wl,--strip-all -Wl,--initial-memory=33554432 \
  -Wl,--max-memory=134217728 -Wl,--export-dynamic
[ "$TEST" = 1 ] && node tests/test_wasm.mjs
"$PY" scripts/build_web.py
echo "built: $MOD (dsp.so, module.json, canvas.js, cards.js, help.json)"
echo "       dist/quark-module.tar.gz   dist/quark.html (open in Chrome/Safari/Firefox)"
mkdir -p docs && cp dist/quark.html docs/index.html   # GitHub Pages copy (Settings > Pages > /docs)
sh scripts/so_gate.sh "$MOD/dsp.so"
