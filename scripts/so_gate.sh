#!/bin/sh
# Load gate for the Move: dsp.so is aarch64, needs glibc <= 2.35, has no libmvec symbols, and exports
# exactly one symbol, move_plugin_init_v2.   Usage: scripts/so_gate.sh [path/to/dsp.so]
set -eu
SO=${1:-modules/sound_generators/quark/dsp.so}
[ -f "$SO" ] || { echo "so_gate: no $SO" >&2; exit 1; }
if command -v readelf >/dev/null 2>&1; then
  readelf -h "$SO" | grep -q "AArch64" || { echo "so_gate: not aarch64" >&2; exit 1; }
  UNDEF=$(readelf --dyn-syms -W "$SO" | awk '$7 == "UND"')
  DEF=$(readelf --dyn-syms -W "$SO" | awk '$7 != "UND" && $7 != "Ndx" && ($5 == "GLOBAL" || $5 == "WEAK") {print $8}' | sed 's/@.*//' | grep -v '^$' || true)
else   # macOS: llvm nm reads ELF
  UNDEF=$(nm -D --undefined-only "$SO")
  DEF=$(nm -D --defined-only "$SO" | awk '{print $3}')
fi
MAXV=$(echo "$UNDEF" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n | tail -1)
echo "glibc needed: ${MAXV:-none}"
echo "$MAXV" | awk -F. '{ if ($1 > 2 || ($1 == 2 && $2 > 35)) exit 1 }' || { echo "so_gate: glibc $MAXV > 2.35" >&2; exit 1; }
if echo "$UNDEF" | grep -q '_ZGV\|libmvec'; then echo "so_gate: libmvec symbols" >&2; exit 1; fi
[ "$(echo "$DEF" | tr -s ' \n' '\n' | grep -v '^$')" = "move_plugin_init_v2" ] || { echo "so_gate: exports: $DEF" >&2; exit 1; }
echo "so_gate OK: aarch64, glibc <= 2.35, one export (move_plugin_init_v2)"
