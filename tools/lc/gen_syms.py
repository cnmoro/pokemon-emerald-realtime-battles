#!/usr/bin/env python3
# Builds a linker script that defines every symbol of the original FireRed 1.0 (BPRE) ROM at its address,
# so the real-time battle code can be linked against the Liquid Crystal ROM (a binary hack of BPRE 1.0).
import sys, collections
src, overrides, out = sys.argv[1], sys.argv[2], sys.argv[3]
# names that are (thumb) functions: calls and function pointers need bit 0 set
FUNCS = set(open(sys.argv[4]).read().split()) if len(sys.argv) > 4 else set()
# names the blob defines itself (its own option menu etc.) must not be pinned to the FireRed addresses
OWN = set(open(sys.argv[5]).read().split()) if len(sys.argv) > 5 else set()
# helpers the blob links from libgcc / newlib itself (a BL from 0x09xxxxxx cannot reach 0x081Exxxx)
SKIP = {'memcpy', 'memset', 'strcmp', 'strcpy', 'strlen', 'memcmp'}
cnt = collections.Counter(); syms = {}
for l in open(src):
    p = l.split()
    if len(p) < 4:
        continue
    addr, name = int(p[0], 16), p[3]
    if name.startswith('__') or name in SKIP or not (name[0].isalpha() or name[0] == '_'):
        continue
    if '.' in name or '$' in name:
        continue
    cnt[name] += 1
    syms[name] = addr
for l in open(overrides):
    l = l.split('#')[0].split()
    if len(l) == 2:
        syms[l[0]] = int(l[1], 16); cnt[l[0]] = 1
with open(out, 'w') as f:
    for n in sorted(syms):
        if cnt[n] == 1 and n not in OWN:
            a = syms[n]
            if n in FUNCS and 0x08000000 <= a < 0x0A000000:
                a |= 1
            f.write('%s = 0x%08X;\n' % (n, a))
