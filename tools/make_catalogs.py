#!/usr/bin/env python3
"""Writes po/<lang>.po and locale/<lang>/LC_MESSAGES/icmon.mo from tools/translations.py (no gettext tools needed)."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from translations import T

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'

def write_mo(path, d):
    keys = sorted(d)
    ids = b''; strs = b''; offs = []
    for k in keys:
        kb, vb = k.encode(), d[k].encode()
        offs.append((len(ids), len(kb), len(strs), len(vb)))
        ids += kb + b'\0'; strs += vb + b'\0'
    n = len(keys); start = 7 * 4 + n * 16
    out = struct.pack('<7I', 0x950412de, 0, n, 28, 28 + n * 8, 0, 0)
    out += b''.join(struct.pack('<2I', l, start + o) for o, l, _, _ in offs)
    out += b''.join(struct.pack('<2I', l, start + len(ids) + o) for _, _, o, l in offs)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(out + ids + strs)

for idx, lang in ((0, 'ru'), (1, 'uk')):
    d = {'': 'Content-Type: text/plain; charset=UTF-8\nLanguage: %s\nPlural-Forms: nplurals=1; plural=0;\n' % lang}
    po = ['msgid ""', 'msgstr ""', q(d[''])]
    for k, v in T.items():
        d[k] = v[idx]
        po += ['', 'msgid ' + q(k), 'msgstr ' + q(v[idx])]
    os.makedirs(os.path.join(root, 'po'), exist_ok=True)
    open(os.path.join(root, 'po', lang + '.po'), 'w').write('\n'.join(po) + '\n')
    write_mo(os.path.join(root, 'locale', lang, 'LC_MESSAGES', 'icmon.mo'), d)
print('ok')
