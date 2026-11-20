#!/usr/bin/env python3
"""Minimal Windows CE ROM (NK) module extractor for ref/wince/nk.img.
Image is a raw XIP image; file offset = VA - 0x80200000."""
import struct, json, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, '..', 'nk.img')
BASE = 0x80200000
data = open(IMG, 'rb').read()

def off(va): return va - BASE
def u32(va): return struct.unpack_from('<I', data, off(va))[0]
def cstr(va):
    o = off(va); e = data.index(b'\0', o); return data[o:e].decode('latin1')

def romhdr():
    assert data[0x40:0x44] == b'ECEC'
    p = struct.unpack_from('<I', data, 0x44)[0]
    f = struct.unpack_from('<17I', data, off(p))
    names = ['dllfirst','dlllast','physfirst','physlast','nummods','ulRAMStart','ulRAMFree','ulRAMEnd',
             'ulCopyEntries','ulCopyOffset','ulProfileLen','ulProfileOffset','numfiles','ulKernelFlags',
             'ulFSRamPercent','ulDrivglobStart','ulDrivglobLen']
    return p, dict(zip(names, f))

def modules():
    p, h = romhdr()
    toc = p + 84
    out = []
    for i in range(h['nummods']):
        e = struct.unpack_from('<IQIIIII', data, off(toc + i*32))
        attr, ft, size, pname, e32, o32, load = e
        out.append(dict(name=cstr(pname), attr=attr, size=size, e32=e32, o32=o32, load=load))
    files = []
    ftoc = toc + h['nummods']*32
    for i in range(h['numfiles']):
        attr, ft, real, comp, pname, loadoff = struct.unpack_from('<IQIIII', data, off(ftoc + i*28))
        files.append(dict(name=cstr(pname), attr=attr, real=real, comp=comp, load=loadoff))
    return out, files

def e32(va):
    o = off(va)
    objcnt, flags, entry, vbase, smaj, smin, stackmax, vsize, s14rva, s14size, ts = struct.unpack_from('<HHIIHHIIIII', data, o)
    units = struct.unpack_from('<18I', data, o + 36)
    return dict(objcnt=objcnt, flags=flags, entry=entry, vbase=vbase, vsize=vsize, ts=ts,
                units=[(units[i*2], units[i*2+1]) for i in range(9)])

def o32s(va, n):
    r = []
    for i in range(n):
        vsize, rva, psize, dataptr, realaddr, flags = struct.unpack_from('<6I', data, off(va + i*24))
        r.append(dict(vsize=vsize, rva=rva, psize=psize, dataptr=dataptr, realaddr=realaddr, flags=flags))
    return r

if __name__ == '__main__':
    p, h = romhdr()
    print('ROMHDR @ %08x' % p, {k: hex(v) for k, v in h.items()})
    mods, files = modules()
    for m in mods:
        e = e32(m['e32']); ss = o32s(m['o32'], e['objcnt'])
        print('%-26s vbase=%08x entry=%08x vsize=%06x' % (m['name'], e['vbase'], e['entry'], e['vsize']))
        if len(sys.argv) > 1:
            for s in ss:
                print('     rva=%06x vsize=%06x psize=%06x dataptr=%08x real=%08x flags=%08x' %
                      (s['rva'], s['vsize'], s['psize'], s['dataptr'], s['realaddr'], s['flags']))
    print('FILES')
    for f in files:
        print('  %-30s real=%d comp=%d load=%08x attr=%x' % (f['name'], f['real'], f['comp'], f['load'], f['attr']))
