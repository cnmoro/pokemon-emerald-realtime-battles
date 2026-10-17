#!/bin/bash
# usage: tt.sh script.txt
# Boots debug.gba to the overworld (boot.txt -> field.ss), then runs the script.
# Placeholders: {symbol} / {symbol+hexoffset} from syms.txt, {HARNESS} = this folder, {OUT} = $OUT (default /tmp/rt_out)
H="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/rt_out}"; mkdir -p "$OUT"
expand() {
python3 - "$1" "$H" "$OUT" <<'PY'
import sys,re
t=open(sys.argv[1]).read().replace('{HARNESS}',sys.argv[2]).replace('{OUT}',sys.argv[3])
syms={}
for l in open(sys.argv[2]+'/syms.txt'):
    p=l.split()
    if len(p)==2 and p[1].startswith('0x'): syms[p[0]]=int(p[1],16)
def rep(m):
    off=int(m.group(2),16) if m.group(2) else 0
    return '%08x'%(syms[m.group(1)]+off)
print(re.sub(r'\{(\w+)(?:\+([0-9a-fA-Fx]+))?\}',rep,t))
PY
}
cd "$H"
expand boot.txt > "$OUT/boot_expanded.txt"; ./run debug.gba "$OUT/boot_expanded.txt" >/dev/null
expand "$1" > "$OUT/script_expanded.txt"; ./run debug.gba "$OUT/script_expanded.txt"
