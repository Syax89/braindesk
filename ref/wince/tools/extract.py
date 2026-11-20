#!/usr/bin/env python3
"""Extract module sections to mods/<name>.<n>.bin + mods/<name>.json and dump strings."""
import sys, os, json, re
import romx
out = os.path.join(romx.HERE, 'mods'); os.makedirs(out, exist_ok=True)
want = sys.argv[1:]
mods, files = romx.modules()
def strings(buf, base, minlen=4):
    res = []
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, buf):
        res.append((base + m.start(), 'A', m.group().decode()))
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen, buf):
        res.append((base + m.start(), 'U', m.group().decode('utf-16le')))
    return sorted(res)
for m in mods:
    if want and m['name'] not in want: continue
    e = romx.e32(m['e32']); ss = romx.o32s(m['o32'], e['objcnt'])
    info = dict(name=m['name'], vbase=e['vbase'], entry=e['entry'], vsize=e['vsize'], units=e['units'], secs=ss)
    json.dump(info, open(os.path.join(out, m['name'] + '.json'), 'w'), indent=1)
    with open(os.path.join(out, m['name'] + '.strings.txt'), 'w') as sf:
        for i, s in enumerate(ss):
            buf = romx.data[romx.off(s['dataptr']):romx.off(s['dataptr']) + min(s['psize'], s['vsize']) if s['psize'] else 0]
            open(os.path.join(out, '%s.%d.bin' % (m['name'], i)), 'wb').write(buf)
            for a, k, t in strings(buf, s['realaddr']):
                sf.write('%08x %s %s\n' % (a, k, t))
    print(m['name'], hex(e['vbase']), [(hex(s['realaddr']), hex(s['vsize']), hex(s['psize']), hex(s['flags'])) for s in ss])
