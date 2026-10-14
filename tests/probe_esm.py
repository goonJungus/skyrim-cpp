#!/usr/bin/env python3
"""Structural probe for a Skyrim ESM/ESP, independent of the C++ reader.

This exists to cross-check the module against an independent implementation.
It is a developer diagnostic, never shipped, and takes the file path as an
argument so no user path is baked in.

It reports:
  * top-level group histogram;
  * CELL / REFR / ACHR / WRLD counts, and how many CELLs are compressed;
  * exterior cells (those with XCLC), counting compressed cells too, distinct
    (worldspace, x, y) coordinates and how many coordinate collisions remain;
  * an optional editor-id substring search.

Usage:
    python probe_esm.py <file.esm> [editorIdSubstring]
"""
import struct
import sys
import zlib
from collections import Counter

REC_HDR = 24
GRP_HDR = 24
SUB_HDR = 6
COMPRESSED = 0x00040000
WORLD_CHILDREN = 1


def rt(b, o):
    return b[o:o + 4].decode('latin1')


def unpack_subrecords(payload):
    """Return {type: data} for a record payload."""
    out = {}
    q = 0
    while q + SUB_HDR <= len(payload):
        t = payload[q:q + 4].decode('latin1')
        size = struct.unpack_from('<H', payload, q + 4)[0]
        out[t] = payload[q + 6:q + 6 + size]
        q += SUB_HDR + size
    return out


def top_level_groups(data, start):
    hist = Counter()
    p = start
    while p + REC_HDR <= len(data):
        t = rt(data, p)
        size = struct.unpack_from('<I', data, p + 4)[0]
        if t == 'GRUP':
            hist[rt(data, p + 8)] += 1
            p += size
        else:
            hist[t] += 1
            p += REC_HDR + size
    return hist


def main():
    path = sys.argv[1]
    needle = sys.argv[2].lower() if len(sys.argv) > 2 else None
    with open(path, 'rb') as f:
        data = f.read()

    tes4 = struct.unpack_from('<I', data, 4)[0]
    start = REC_HDR + tes4

    stats = Counter()
    comp = Counter()
    exterior = Counter()          # (worldspace, x, y) -> occurrences
    matches = []

    def walk(p, end, depth, worldspace):
        while p + REC_HDR <= end:
            t = rt(data, p)
            size = struct.unpack_from('<I', data, p + 4)[0]
            flags = struct.unpack_from('<I', data, p + 8)[0]
            if t == 'GRUP':
                gt = struct.unpack_from('<i', data, p + 12)[0]
                label = struct.unpack_from('<I', data, p + 8)[0]
                child_ws = label if gt == WORLD_CHILDREN else worldspace
                walk(p + GRP_HDR, min(p + size, end), depth + 1, child_ws)
                p += size
                continue
            body = p + REC_HDR
            if body + size > end:
                return
            if t in ('CELL', 'REFR', 'ACHR', 'WRLD'):
                stats[t] += 1
                if t == 'CELL':
                    if flags & COMPRESSED:
                        comp['cell_compressed'] += 1
                        try:
                            pl = zlib.decompress(data[body + 4:body + size])
                        except Exception:
                            pl = b''
                    else:
                        pl = data[body:body + size]
                    d = unpack_subrecords(pl)
                    if 'XCLC' in d and len(d['XCLC']) >= 8:
                        gx, gy = struct.unpack_from('<ii', d['XCLC'])
                        exterior[(worldspace, gx, gy)] += 1
                        stats['cell_exterior'] += 1
                    else:
                        stats['cell_interior'] += 1
                    if needle is not None:
                        edid = d.get('EDID', b'').split(b'\x00')[0].decode('latin1')
                        if needle in edid.lower():
                            matches.append((edid, worldspace, d.get('XCLC')))
            p += REC_HDR + size

    walk(start, len(data), 0, 0)

    print("file:", path, f"({len(data)} bytes)")
    print("\ntop-level groups:")
    for name, n in top_level_groups(data, start).most_common():
        print(f"  {name}: {n}")
    print("\nrecord counts:")
    for k in ('CELL', 'cell_exterior', 'cell_interior', 'REFR', 'ACHR', 'WRLD'):
        print(f"  {k}: {stats[k]}")
    print(f"  cell_compressed: {comp['cell_compressed']}")
    dups = sum(1 for v in exterior.values() if v > 1)
    print(f"\nexterior cells: {stats['cell_exterior']}")
    print(f"distinct (worldspace,x,y): {len(exterior)}")
    print(f"duplicate coordinates: {dups}")

    if needle is not None:
        print(f"\ncells matching {needle!r}: {len(matches)}")
        for edid, ws, xclc in matches[:25]:
            if xclc and len(xclc) >= 8:
                gx, gy = struct.unpack_from('<ii', xclc)
                print(f"  ws=0x{ws:08x} ({gx},{gy}) {edid}")
            else:
                print(f"  [interior] {edid}")


if __name__ == '__main__':
    main()
