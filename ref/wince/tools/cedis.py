#!/usr/bin/env python3
"""Annotated ARM disassembly of a WinCE ROM module.
usage: python3 cedis.py <module name>...  -> writes dis/<module>.dis (needs: pip install capstone)"""
import sys, os, struct, re
import romx
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

class Mod:
    def __init__(self, name, resolve=True):
        mods, _ = romx.modules()
        m = [x for x in mods if x['name'].lower() == name.lower()][0]
        self.name = m['name']
        self.e = romx.e32(m['e32']); self.secs = romx.o32s(m['o32'], self.e['objcnt'])
        self.vbase = self.e['vbase']
        self.mem = []
        for s in self.secs:
            n = min(s['psize'], s['vsize'])
            raw = romx.data[romx.off(s['dataptr']):romx.off(s['dataptr']) + n]
            raw = raw + b'\0' * (s['vsize'] - len(raw))
            self.mem.append((s['realaddr'], raw, s))
        self.imports = {}
        self._imports()
        self.funcs = self._pdata()
        self.exports = self._exports()
        if resolve: self._resolve_ordinals()
    def rd(self, va, n):
        for base, raw, s in self.mem:
            if base <= va < base + len(raw):
                return raw[va - base: va - base + n]
        return None
    def u32(self, va):
        b = self.rd(va, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None
    def cstr(self, va):
        o = b''
        while True:
            c = self.rd(va, 1)
            if not c or c == b'\0': break
            o += c; va += 1
        return o.decode('latin1')
    def string_at(self, va):
        if va is None: return None
        b = self.rd(va, 200)
        if not b: return None
        m = re.match(rb'(?:[\x20-\x7e\r\n\t]\x00){3,}', b)
        if m: return 'L"%s"' % m.group().decode('utf-16le').replace('\n', '\\n').replace('\r', '\\r')
        m = re.match(rb'[\x20-\x7e\r\n\t]{4,}', b)
        if m: return '"%s"' % m.group().decode().replace('\n', '\\n').replace('\r', '\\r')
        return None
    def _imports(self):
        rva, size = self.e['units'][1]
        if not rva: return
        p = self.vbase + rva
        while True:
            d = self.rd(p, 20)
            if not d: break
            oft, ts, fc, nm, ft = struct.unpack('<5I', d)
            if not nm: break
            dll = self.cstr(self.vbase + nm)
            i = 0
            while True:
                t = self.u32(self.vbase + (oft or ft) + i * 4)
                if not t: break
                if t & 0x80000000: nmn = '%s!#%d' % (dll, t & 0xffff)
                else: nmn = '%s!%s' % (dll, self.cstr(self.vbase + t + 2))
                self.imports[self.vbase + ft + i * 4] = nmn
                i += 1
            p += 20
    def ordmap(self):
        rva, size = self.e['units'][0]
        om = {}
        if not rva: return om
        d = struct.unpack('<IIHHIIIIIII', self.rd(self.vbase + rva, 40))
        base, nfun, nnam, afun, anam, aord = d[5], d[6], d[7], d[8], d[9], d[10]
        for i in range(nnam):
            n = self.cstr(self.vbase + self.u32(self.vbase + anam + i * 4))
            o = struct.unpack('<H', self.rd(self.vbase + aord + i * 2, 2))[0]
            om[base + o] = n
        return om
    _omcache = {}
    def _resolve_ordinals(self):
        for k, v in list(self.imports.items()):
            dll, f = v.split('!')
            if not f.startswith('#'): continue
            if dll.lower() not in Mod._omcache:
                try: Mod._omcache[dll.lower()] = Mod(dll, resolve=False).ordmap()
                except Exception as ex: Mod._omcache[dll.lower()] = {}
            n = Mod._omcache[dll.lower()].get(int(f[1:]))
            if n: self.imports[k] = '%s!%s' % (dll, n)
    def rva2va(self, rva, size):
        # kernel modules have overlapping RVAs; pick the read-only section that matches rva and holds data
        cands = [s for s in self.secs if s['rva'] <= rva < s['rva'] + max(s['vsize'], 1) and s['psize']]
        cands.sort(key=lambda s: (s['flags'] & 0x80000000) != 0)
        if cands: return cands[0]['realaddr'] + (rva - cands[0]['rva'])
        return self.vbase + rva
    def _pdata(self):
        rva, size = self.e['units'][3]
        f = []
        pva = self.rva2va(rva, size) if rva else 0
        for i in range(size // 8):
            a, b = struct.unpack('<II', self.rd(pva + i * 8, 8))
            f.append((a, ((b >> 8) & 0x3fffff), (b >> 30) & 1, b & 0xff))
        return f
    def _exports(self):
        rva, size = self.e['units'][0]
        ex = {}
        if not rva: return ex
        d = struct.unpack('<IIHHIIIIIII', self.rd(self.vbase + rva, 40))
        base, nfun, nnam, afun, anam, aord = d[5], d[6], d[7], d[8], d[9], d[10]
        for i in range(nnam):
            n = self.cstr(self.vbase + self.u32(self.vbase + anam + i * 4))
            o = struct.unpack('<H', self.rd(self.vbase + aord + i * 2, 2))[0]
            ex[self.vbase + self.u32(self.vbase + afun + o * 4)] = n
        for i in range(nfun):
            a = self.vbase + self.u32(self.vbase + afun + i * 4)
            ex.setdefault(a, 'ord_%d' % (base + i))
        return ex

def disasm(mod, out):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM); md.detail = False
    text_base, text, s = mod.mem[0]
    fstart = {a: (ln, is32) for a, ln, is32, pl in mod.funcs}
    # literal pool detection: collect targets of pc-relative loads first
    lits = set()
    for off in range(0, len(text) & ~3, 4):
        w_ = struct.unpack_from('<I', text, off)[0]
        if (w_ & 0x0f7f0000) == 0x051f0000 and (w_ >> 28) != 0xf:  # ldr rX,[pc,#imm]
            imm = w_ & 0xfff
            if not (w_ & 0x00800000): imm = -imm
            t = text_base + off + 8 + imm
            if text_base <= t < text_base + len(text): lits.add(t)
    names = dict(mod.exports)
    for a in fstart: names.setdefault(a, 'sub_%08x' % a)
    # import thunks: ldr ip,[pc,#4]; ldr ip,[ip]; bx ip; .word __imp
    for off in range(0, (len(text) & ~3) - 12, 4):
        w0, w1, w2, w3 = struct.unpack_from('<4I', text, off)
        if w0 == 0xe59fc004 and w1 == 0xe59cc000 and w2 == 0xe12fff1c and w3 in mod.imports:
            names[text_base + off] = mod.imports[w3].split('!')[1]
        if w0 == 0xe59fc000 and w1 == 0xe59cf000 and w2 in mod.imports:
            names[text_base + off] = mod.imports[w2].split('!')[1]
    def sym(v):
        if v in mod.imports: return '__imp_' + mod.imports[v]
        if v in names: return names[v]
        s = mod.string_at(v)
        if s: return s
        return None
    w = out.write
    w('; module %s vbase=%08x entry=%08x\n' % (mod.name, mod.vbase, mod.vbase + mod.e['entry']))
    for k, v in sorted(mod.imports.items()): w('; import %08x %s\n' % (k, v))
    for k, v in sorted(mod.exports.items()): w('; export %08x %s\n' % (k, v))
    a = text_base; end = text_base + s['vsize']
    while a < end:
        if a in fstart:
            w('\n; ======== %s  (len %d insns)\n' % (names[a], fstart[a][0]))
        word = mod.u32(a)
        if word is None: break
        if a in lits:
            sy = sym(word)
            w('%08x:  .word    0x%08x%s\n' % (a, word, ('   ; ' + sy) if sy else ''))
            a += 4; continue
        ins = list(md.disasm(mod.rd(a, 4), a))
        if not ins:
            if word is None: break
            sy = sym(word)
            w('%08x:  .word    0x%08x%s\n' % (a, word, ('   ; ' + sy) if sy else ''))
            a += 4; continue
        i = ins[0]
        cmt = ''
        if (word & 0x0f7f0000) == 0x051f0000:
            imm = word & 0xfff
            if not (word & 0x00800000): imm = -imm
            t = a + 8 + imm; v = mod.u32(t)
            if v is not None:
                sy = sym(v)
                cmt = '   ; =0x%08x%s' % (v, (' ' + sy) if sy else '')
        elif i.mnemonic.startswith('bl') or i.mnemonic == 'b' or re.match(r'^b(eq|ne|gt|lt|ge|le|hi|ls|hs|lo|mi|pl|cs|cc)$', i.mnemonic):
            m = re.match(r'#0x([0-9a-f]+)', i.op_str)
            if m:
                t = int(m.group(1), 16)
                if t in names: cmt = '   ; ' + names[t]
        w('%08x:  %-8s %s%s\n' % (a, i.mnemonic, i.op_str, cmt))
        a += 4
    # data sections dump
    for base, raw, s in mod.mem[1:]:
        w('\n; ---- section real=%08x vsize=%x psize=%x flags=%08x\n' % (base, s['vsize'], s['psize'], s['flags']))
        for o in range(0, len(raw) & ~3, 4):
            v = struct.unpack_from('<I', raw, o)[0]
            sy = sym(v) if v else None
            w('%08x: %08x%s\n' % (base + o, v, ('   ; ' + sy) if sy else ''))

if __name__ == '__main__':
    os.makedirs(os.path.join(romx.HERE, 'dis'), exist_ok=True)
    for n in sys.argv[1:]:
        m = Mod(n)
        p = os.path.join(romx.HERE, 'dis', m.name + '.dis')
        with open(p, 'w') as f: disasm(m, f)
        print(p, len(m.funcs), 'funcs', len(m.imports), 'imports')
