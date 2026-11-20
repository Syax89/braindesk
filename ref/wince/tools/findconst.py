#!/usr/bin/env python3
"""Find modules whose code builds a given 32-bit constant (literal pool, or mov+orr pair)."""
import sys, struct
import romx
def rot_imm(w):
    imm = w & 0xff; rot = ((w >> 8) & 0xf) * 2
    return ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
def scan(target):
    mods, _ = romx.modules()
    for m in mods:
        e = romx.e32(m['e32']); ss = romx.o32s(m['o32'], e['objcnt'])
        for s in ss:
            if not (s['flags'] & 0x20): continue
            raw = romx.data[romx.off(s['dataptr']):romx.off(s['dataptr']) + min(s['psize'], s['vsize'])]
            n = len(raw) & ~3
            words = struct.unpack('<%dI' % (n // 4), raw[:n])
            for i, w in enumerate(words):
                va = s['realaddr'] + i * 4
                if w == target:
                    print('%-24s %08x literal' % (m['name'], va))
                # mov rX,#imm  (cond AL, I=1, opcode MOV 0x3a0)
                if (w & 0x0fef0000) == 0x03a00000:
                    v = rot_imm(w); rd = (w >> 12) & 0xf
                    for j in range(i + 1, min(i + 6, len(words))):
                        w2 = words[j]
                        if (w2 & 0x0fe00000) == 0x03800000 and ((w2 >> 16) & 0xf) == rd:   # orr rY, rX, #imm
                            if (v | rot_imm(w2)) == target:
                                print('%-24s %08x mov+orr' % (m['name'], va))
                        if (w2 & 0x0fe00000) == 0x02800000 and ((w2 >> 16) & 0xf) == rd:   # add
                            if (v + rot_imm(w2)) & 0xffffffff == target:
                                print('%-24s %08x mov+add' % (m['name'], va))
if __name__ == '__main__':
    for a in sys.argv[1:]:
        print('== 0x%08x' % int(a, 16)); scan(int(a, 16))
