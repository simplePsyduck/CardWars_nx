#!/usr/bin/env python3
"""Dump JNINativeMethod {name, signature, fnPtr} triples from a 32-bit ARM .so
(RegisterNatives tables). Pure stdlib: reads PT_LOAD headers, scans for words."""
import struct, sys, re

def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1
    phoff, = struct.unpack_from("<I", d, 0x1c)
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2a)
    segs = []
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", d, phoff + i * phentsize)
        if t == 1:
            segs.append((va, off, fsz, msz, fl))
    return d, segs

def va2off(segs, va):
    for v, o, fsz, msz, fl in segs:
        if v <= va < v + fsz:
            return o + va - v
    return None

def cstr(d, segs, va, maxn=300):
    o = va2off(segs, va)
    if o is None: return None
    e = d.find(b"\0", o, o + maxn)
    if e < 0: return None
    try: return d[o:e].decode("ascii")
    except UnicodeDecodeError: return None

def main(path):
    d, segs = load(path)
    ident = re.compile(r"^[A-Za-z_][A-Za-z0-9_$]*$")
    sig = re.compile(r"^\((?:\[*(?:[ZBCSIJFD]|L[\w/$]+;))*\)\[*(?:[ZBCSIJFDV]|L[\w/$]+;)$")
    res = []
    for v, o, fsz, msz, fl in segs:
        if fl & 1: continue            # data segments only (not X)
        for off in range(o, o + fsz - 11, 4):
            a, b, c = struct.unpack_from("<3I", d, off)
            n = cstr(d, segs, a, 80)
            if not n or not ident.match(n): continue
            s = cstr(d, segs, b, 200)
            if not s or not sig.match(s): continue
            res.append((v + off - o, n, s, c))
    return res

if __name__ == "__main__":
    for va, n, s, fn in main(sys.argv[1]):
        print(f"{va:08x}  {n:34s} {s:60s} {fn:08x}")
