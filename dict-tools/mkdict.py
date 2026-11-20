#!/usr/bin/env python3
"""mkdict.py: dizionario dictd (FreeDict) -> formato di BrainDesk (view_dict.c).

  NOME.dict  testo delle voci (decompresso: accesso diretto)
  NOME.idx   righe "chiave\\tvoce\\tposizione\\tlunghezza\\n" ordinate per chiave
             (chiave = voce in minuscolo, NFKC), per la ricerca per prefisso
  NOME.ofs   posizione (uint32 LE) di ogni riga di NOME.idx: ricerca binaria
             con mmap, senza caricare l'indice in memoria
uso: mkdict.py INDICE DICT.DZ USCITA/NOME
"""
import gzip, struct, sys, unicodedata

B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
def b64(s):
    n = 0
    for ch in s:
        n = n * 64 + B64.index(ch)
    return n

idx, dz, out = sys.argv[1:4]
data = gzip.open(dz).read()
rows = []
for line in open(idx, encoding='utf-8'):
    p = line.rstrip('\n').split('\t')
    if len(p) < 3 or p[0].startswith('00database') or p[0].startswith('00-database'):
        continue
    head, off, ln = p[0], b64(p[1]), b64(p[2])
    key = unicodedata.normalize('NFKC', head).casefold()
    rows.append((key, head, off, ln))
rows.sort(key=lambda r: (r[0].encode('utf-8'), r[2]))
open(out + '.dict', 'wb').write(data)
ofs = []
with open(out + '.idx', 'wb') as f:
    pos = 0
    for key, head, off, ln in rows:
        b = ('%s\t%s\t%d\t%d\n' % (key.replace('\t', ' '), head.replace('\t', ' '), off, ln)).encode('utf-8')
        ofs.append(pos)
        f.write(b)
        pos += len(b)
with open(out + '.ofs', 'wb') as f:
    f.write(struct.pack('<%dI' % len(ofs), *ofs))
print(out, len(rows), 'voci', len(data) // 1024, 'KB')
