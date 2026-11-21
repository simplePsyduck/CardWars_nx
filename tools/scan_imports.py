#!/usr/bin/env python3
"""scan_imports.py -- write tools/imports_needed.txt from the game's libraries.

Same output as `runtime/tools/gen_imports.py --libs DIR`, but with no
third-party dependency (that one needs pyelftools).  It reads the section
headers of each 32-bit ARM ELF, collects the undefined dynamic symbols and
drops those that another listed module exports.  Symbol NAMES are an
interface, not game content, so the result is safe to commit.

  python3 tools/scan_imports.py <dir with the .so files>
  (libs: unzip -j <your.apk> 'lib/armeabi-v7a/*' -d /tmp/libs)

MIT.
"""
import os, re, struct, sys

def read_dynsyms(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or struct.unpack_from("<H", d, 18)[0] != 40:
        sys.exit(f"{path}: not a 32-bit ARM ELF")
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
    und, exp = {}, set()
    for (name, typ, flags, addr, off, size, link, info, align, entsize) in secs:
        if typ != 11:                       # SHT_DYNSYM
            continue
        stroff, strsize = secs[link][4], secs[link][5]
        for i in range(size // 16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, off + i * 16)
            if not st_name:
                continue
            e = d.index(b"\0", stroff + st_name)
            n = d[stroff + st_name:e].decode("ascii", "replace")
            bind = st_info >> 4
            if st_shndx == 0:
                und[n] = und.get(n, True) and bind == 2      # weak only if weak everywhere
            elif bind != 0:
                exp.add(n)
    return und, exp

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    libdir = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    cfg = open(os.path.join(here, "imports.cfg")).read()
    mods = []
    for line in cfg.splitlines():
        line = line.split("#", 1)[0].split()
        if line and line[0].upper() == "MODULES":
            mods += line[1:]
    und, exp = {}, set()
    for m in mods:
        u, e = read_dynsyms(os.path.join(libdir, m))
        for n, w in u.items():
            und[n] = und.get(n, True) and w
        exp |= e
    need = {n: w for n, w in und.items() if n not in exp}
    with open(os.path.join(here, "imports_needed.txt"), "w") as f:
        f.write("# symbol weak(0/1) -- regenerate with tools/scan_imports.py (or gen_imports.py --libs)\n")
        for n in sorted(need):
            f.write(f"{n} {int(need[n])}\n")
    print(f"{len(mods)} modules, {len(need)} imports no module exports -> tools/imports_needed.txt")

main()
