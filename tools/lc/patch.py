#!/usr/bin/env python3
"""Applies the real-time battle engine to a Pokemon Liquid Crystal ROM.
usage: patch.py liquid_crystal.gba build/lc/rt_blob.bin build/lc/rt_blob.map out.gba [--debug]
"""
import re, struct, sys

BLOB_ADDR = 0x09000000

def load_syms(mapfile):
    syms = {}
    for m in re.finditer(r'^\s+0x([0-9a-f]{8,16})\s+(\w+)$', open(mapfile).read(), re.M):
        syms[m.group(2)] = int(m.group(1), 16)
    return syms

def load_vanilla(symfile):
    v = {}
    for l in open(symfile):
        p = l.split()
        if len(p) >= 4:
            v.setdefault(p[3], int(p[0], 16))
    return v

def hook(rom, at, target, expect=None):
    """Overwrite the start of the thumb function at `at` with a jump to the thumb function `target`."""
    o = at & 0x1FFFFFF
    if expect is not None and rom[o:o + len(expect)] != expect:
        raise SystemExit('unexpected code at %08x: %s' % (at, rom[o:o + 8].hex()))
    if at % 4 == 0:
        code = struct.pack('<HHI', 0x4B00, 0x4718, target | 1)          # ldr r3,[pc,#0]; bx r3; .word
    else:
        code = struct.pack('<HHHI', 0x4B01, 0x4718, 0x46C0, target | 1)  # ldr r3,[pc,#4]; bx r3; nop; .word
    rom[o:o + len(code)] = code

def jump_code(at, target):
    """thumb code placed at `at` that jumps to the thumb address `target` (clobbers r3)"""
    if at % 4 == 0:
        return struct.pack('<HHI', 0x4B00, 0x4718, target | 1)
    return struct.pack('<HHHI', 0x4B01, 0x4718, 0x46C0, target | 1)

def hook_with_trampoline(rom, at, target, ncopy, tramp_addr, expect):
    """Like hook(), but the overwritten instructions (ncopy bytes, none PC-relative) are copied to the
    trampoline slot at tramp_addr, followed by a jump back to at + ncopy."""
    o = at & 0x1FFFFFF
    if rom[o:o + len(expect)] != expect:
        raise SystemExit('unexpected code at %08x: %s' % (at, rom[o:o + 12].hex()))
    if ncopy < len(jump_code(at, target)):
        raise SystemExit('hook at %08x needs at least %d bytes' % (at, len(jump_code(at, target))))
    t = tramp_addr & 0x1FFFFFF
    code = bytes(rom[o:o + ncopy]) + jump_code(tramp_addr + ncopy, at + ncopy)
    if len(code) > 32:
        raise SystemExit('trampoline too long')
    rom[t:t + len(code)] = code
    hook(rom, at, target)

def main():
    src, blob, mapf, out = sys.argv[1:5]
    debug = '--debug' in sys.argv
    rom = bytearray(open(src, 'rb').read())
    vanilla = bytes(open(src, 'rb').read())
    if rom[0xAC:0xB0] != b'BPRE':
        raise SystemExit('not a FireRed (BPRE) based ROM')
    data = open(blob, 'rb').read()
    if data[:4] != b'RTLC':
        raise SystemExit('bad blob')
    syms = load_syms(mapf)
    v = load_vanilla(__file__.rsplit('/', 1)[0] + '/pokefirered_bpre10.sym')
    end = (BLOB_ADDR & 0x1FFFFFF) + len(data)
    if len(rom) < end:
        rom += b'\xff' * (end - len(rom))
    rom[BLOB_ADDR & 0x1FFFFFF:end] = data

    # battle start -> real-time engine when supported
    hook(rom, v['CB2_InitBattle'], syms['RtLc_CB2_InitBattle'], vanilla[(v['CB2_InitBattle'] & 0x1FFFFFF):][:4])
    # the engine's RAM sits at the top of gHeap
    hook(rom, v['InitHeap'], syms['RtLc_InitHeap'])

    tramps = syms['RtLc_Trampolines']
    if rom[tramps & 0x1FFFFFF:(tramps & 0x1FFFFFF) + 4] != b'RTLT':
        raise SystemExit('trampoline table not found')
    if debug:
        rom[syms['gRtDebugEnabled'] & 0x1FFFFFF] = 1
        # debug commands polled from the overworld input handler (push {r4-r7,lr}; mov r7,r8; push {r7}; sub sp,#8)
        hook_with_trampoline(rom, v['ProcessPlayerFieldInput'], syms['RtLc_ProcessPlayerFieldInput'], 8,
                             tramps + 32 * 0, bytes.fromhex('f0b54746 80b482b0'))
    # round the size up to 64 KB
    n = (len(rom) + 0xFFFF) & ~0xFFFF
    rom += b'\xff' * (n - len(rom))
    open(out, 'wb').write(rom)
    print('wrote %s (%.1f MB)%s' % (out, len(rom) / 1048576, ' [debug]' if debug else ''))

main()
