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
    # OPTION menu with the real-time rows
    hook(rom, v['CB2_OptionsMenuFromStartMenu'], syms['CB2_OptionsMenuFromStartMenu'])
    # summary screen: cooldown in seconds where the PP were
    hook(rom, v['BufferMonMoveI'], syms['RtLc_BufferMonMoveI'])
    hook(rom, v['PokeSum_PrintMoveName'], syms['RtLc_PokeSum_PrintMoveName'])
    # TEXT SPEED INSTANT and the 3x faster battle start transition
    hook(rom, v['RunTextPrinters'], syms['RtLc_RunTextPrinters'])
    hook(rom, v['GetTextSpeedSetting'], syms['RtLc_GetTextSpeedSetting'])
    hook(rom, v['Task_BattleTransition'], syms['RtLc_Task_BattleTransition'])
    # the engine's RAM sits at the top of gHeap
    hook(rom, v['InitHeap'], syms['RtLc_InitHeap'])

    tramps = syms['RtLc_Trampolines']
    if rom[tramps & 0x1FFFFFF:(tramps & 0x1FFFFFF) + 4] != b'RTLT':
        raise SystemExit('trampoline table not found')
    # Pokemon Center: talking to the nurse heals at once (no dialogue, no Poke Ball machine)
    nurse = v['EventScript_PkmnCenterNurse'] & 0x1FFFFFF
    script = (bytes([0x25]) + struct.pack('<H', 0x187)                      # special GetQuestLogState
              + bytes([0x21]) + struct.pack('<HH', 0x800D, 1)              # compare VAR_RESULT, TRUE
              + bytes([0x06, 1]) + struct.pack('<I', v['EventScript_ReleaseEnd'])  # goto_if_eq ReleaseEnd
              + bytes([0xC3, 15])                                          # incrementgamestat USED_POKECENTER
              + bytes([0x25]) + struct.pack('<H', 0x000)                   # special HealPlayerParty
              + bytes([0x2F]) + struct.pack('<H', 1)                       # playse SE_USE_ITEM
              + bytes([0x03]))                                             # return
    rom[nurse:nurse + len(script)] = script

    # party menu MOVES (move relearner): the cursor option table gets one more entry in the blob
    opts = v['sCursorOptions'] & 0x1FFFFFF
    newopts = syms['RtLc_CursorOptions']
    no = newopts & 0x1FFFFFF
    rom[no:no + 30 * 8] = rom[opts:opts + 30 * 8]
    refs = [i for i in range(0, 0x1000000, 4) if struct.unpack_from('<I', rom, i)[0] == v['sCursorOptions']]
    if len(refs) != 4:
        raise SystemExit('expected 4 references to sCursorOptions, found %d' % len(refs))
    for i in refs:
        struct.pack_into('<I', rom, i, newopts)
    hook_with_trampoline(rom, v['SetPartyMonFieldSelectionActions'], syms['RtLc_SetPartyMonFieldSelectionActions'], 8,
                         tramps + 32 * 1, bytes.fromhex('f0b55746 4e464546'))
    hook_with_trampoline(rom, v['ToggleFieldMoveDescriptionWindow'], syms['RtLc_ToggleFieldMoveDescriptionWindow'], 8,
                         tramps + 32 * 2, bytes.fromhex('30b585b0 0006000e'))
    # level-up moves + the family's egg moves
    hook_with_trampoline(rom, v['GetMoveRelearnerMoves'], syms['RtLc_GetMoveRelearnerMoves'], 8,
                         tramps + 32 * 3, bytes.fromhex('f0b55746 4e464546'))
    hook(rom, v['GetNumberOfRelearnableMoves'], syms['RtLc_GetNumberOfRelearnableMoves'])
    # HMs (and TMs) of field moves work from the bag
    hook(rom, v['ScrCmd_checkpartymove'], syms['RtLc_ScrCmd_checkpartymove'])
    hook(rom, v['PartyHasMonWithSurf'], syms['RtLc_PartyHasMonWithSurf'])
    hook(rom, v['SetDefaultFlashLevel'], syms['RtLc_SetDefaultFlashLevel'])
    hook(rom, v['IsMoveHm'], syms['RtLc_IsMoveHm'])
    hook(rom, v['IsWildLevelAllowedByRepel'], syms['RtLc_IsWildLevelAllowedByRepel'])
    # ... and the relearner goes back to the party menu when it was opened from there
    f = v['MoveRelearnerStateMachine'] & 0x1FFFFFF
    lits = [i for i in range(f, f + 0x600, 4) if struct.unpack_from('<I', rom, i)[0] == (v['CB2_ReturnToField'] | 1)]
    if len(lits) != 1:
        raise SystemExit('expected 1 CB2_ReturnToField literal in MoveRelearnerStateMachine, found %d' % len(lits))
    struct.pack_into('<I', rom, lits[0], syms['RtLc_RelearnerExit'] | 1)

    # LEVEL CAP: RARE CANDY has no effect at the cap
    hook_with_trampoline(rom, v['ItemUseCB_RareCandy'], syms['RtLc_ItemUseCB_RareCandy'], 8,
                         tramps + 32 * 6, bytes.fromhex('f0b54746 80b48846'))
    # SUMMARY, SKILLS page: A cycles stats / IVs / EVs / talents
    hook_with_trampoline(rom, v['Task_InputHandler_Info'], syms['RtLc_Task_InputHandler_Info'], 8,
                         tramps + 32 * 4, bytes.fromhex('f0b54746 80b481b0'))
    hook_with_trampoline(rom, v['BufferMonSkills'], syms['RtLc_BufferMonSkills'], 12,
                         tramps + 32 * 5, bytes.fromhex('f0b55746 4e464546 e0b485b0'))

    # shiny odds 1/64: every "shiny value < 8" test becomes "shiny value >> 10 == 0" (value < 1024)
    def shiny(at, reg, branch_from, branch_to):
        o = at & 0x1FFFFFF
        cmp = 0x2807 | (reg << 8)
        ins, br = struct.unpack_from('<HH', rom, o)
        if ins != cmp or (br >> 8) != branch_from:
            raise SystemExit('unexpected shiny test at %08x: %04x %04x' % (at, ins, br))
        struct.pack_into('<HH', rom, o, 0x0800 | (10 << 6) | (reg << 3) | reg, (branch_to << 8) | (br & 0xFF))
    shiny(0x0803DB5E, 1, 0xD9, 0xD0)   # CreateBoxMon, OT_ID_RANDOM_NO_SHINY: retry while shiny (bls -> beq)
    shiny(0x08044120, 0, 0xD9, 0xD0)   # GetMonSpritePalFromSpeciesAndPersonality (bls -> beq)
    shiny(0x08044196, 0, 0xD9, 0xD0)   # GetMonSpritePalStructFromOtIdPersonality (bls -> beq)
    shiny(0x080444B0, 2, 0xD8, 0xD1)   # IsShinyOtIdPersonality (bhi -> bne)

    # reusable TMs: Task_LearnedMove never takes the TM from the bag (bhi over RemoveBagItem -> b)
    o = 0x08125C7A & 0x1FFFFFF
    if rom[o:o + 2] != bytes.fromhex('03d8'):
        raise SystemExit('unexpected code in Task_LearnedMove')
    rom[o:o + 2] = bytes.fromhex('03e0')
    # ... nor when the TM is taught from the TM CASE (CB2_UseItem) or over a forgotten move
    # (CB2_UseTMHMAfterForgettingMove): same 'item < HM01 -> RemoveBagItem' test, branch always over it
    for at, fn in ((0x08124EA6, 'CB2_UseItem'), (0x08124F72, 'CB2_UseTMHMAfterForgettingMove')):
        o = at & 0x1FFFFFF
        if rom[o:o + 2] != bytes.fromhex('03d8'):
            raise SystemExit('unexpected code in ' + fn)
        rom[o:o + 2] = bytes.fromhex('03e0')

    # items: the fanfare plays to its end without holding the player, and no "put the item in the pocket"
    # message (waitfanfare waitmessage msgbox -> waitmessage waitbuttonpress)
    for at, tail in ((0x081A674E, 0), (0x081A6816, 0), (0x081A689B, 0), (0x081A6908, 0), (0x081A8D6E, 3)):
        o = at & 0x1FFFFFF
        n = 10 + tail
        if rom[o:o + 2] != b'\x32\x66' or rom[o + 2 + tail:o + 4 + tail] != b'\x0f\x00' or rom[o + n - 2:o + n] != b'\x09\x04':
            raise SystemExit('unexpected item script at %08x: %s' % (at, rom[o:o + n].hex()))
        rom[o:o + n] = b'\x66\x6d' + b'\x00' * (n - 2)

    # Goldenrod Dept. Store: the vitamins counter also sells every evolution stone
    vit = struct.pack('<I', 0x08816AC2)
    n = 0
    for i in range(0, 0x1000000 - 5):
        if rom[i] == 0x86 and rom[i + 1:i + 5] == vit:
            rom[i + 1:i + 5] = struct.pack('<I', syms['RtLc_MartVitaminsAndStones'])
            n += 1
    if n != 2:
        raise SystemExit('expected 2 pokemart scripts with the Goldenrod vitamins, found %d' % n)

    if debug:
        rom[syms['gRtDebugEnabled'] & 0x1FFFFFF] = 1
        # debug commands polled from the overworld input handler (push {r4-r7,lr}; mov r7,r8; push {r7}; sub sp,#8)
        # frame-rate measurements: count the main loop iterations
        hook(rom, v['PlayTimeCounter_Update'], syms['RtLc_PlayTimeCounter_Update'])
        hook_with_trampoline(rom, v['ProcessPlayerFieldInput'], syms['RtLc_ProcessPlayerFieldInput'], 8,
                             tramps + 32 * 0, bytes.fromhex('f0b54746 80b482b0'))
    # round the size up to 64 KB
    n = (len(rom) + 0xFFFF) & ~0xFFFF
    rom += b'\xff' * (n - len(rom))
    open(out, 'wb').write(rom)
    print('wrote %s (%.1f MB)%s' % (out, len(rom) / 1048576, ' [debug]' if debug else ''))

main()
