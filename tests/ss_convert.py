#!/usr/bin/env python3
"""Convert SingleStepTests (6502/v1/xx.json) to a compact binary format.

record: u16 pc,u8 s,a,x,y,p, u8 nram, (u16 addr,u8 val)*nram,
        u16 pc,u8 s,a,x,y,p, u8 nram, (u16 addr,u8 val)*nram, u8 ncycles
"""
import json, struct, sys, os
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
for fn in sorted(os.listdir(src)):
    if not fn.endswith('.json'): continue
    tests = json.load(open(os.path.join(src, fn)))
    out = bytearray()
    for t in tests:
        for part in ('initial', 'final'):
            st = t[part]
            out += struct.pack('<HBBBBBB', st['pc'], st['s'], st['a'], st['x'], st['y'], st['p'], len(st['ram']))
            for a, v in st['ram']:
                out += struct.pack('<HB', a, v)
        out += struct.pack('<B', len(t['cycles']))
    open(os.path.join(dst, fn.replace('.json', '.bin')), 'wb').write(out)
