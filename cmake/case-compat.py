#!/usr/bin/env python3
"""Make the case-insensitive SDK includes resolvable on a case-sensitive file system.

The Deus Ex headers were written on Windows, so a few of them spell each other's
names with the wrong case (Core.h asks for "UnCid.h", the file is "UnCId.h").
This scans the given include directories and writes a directory of symlinks under
the alternate spellings, which the build puts last on the include path.
"""
import os
import sys
import re

INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def main(out_dir, src_dirs):
    known = {}          # lowercase name -> real path
    for d in src_dirs:
        for name in os.listdir(d):
            known.setdefault(name.lower(), os.path.join(d, name))

    wanted = set()
    for d in src_dirs:
        for name in os.listdir(d):
            path = os.path.join(d, name)
            if not os.path.isfile(path):
                continue
            with open(path, 'rb') as f:
                for m in INCLUDE.finditer(f.read()):
                    wanted.add(m.group(1).decode('latin-1').replace('\\', '/'))

    os.makedirs(out_dir, exist_ok=True)
    made = 0
    for name in sorted(wanted):
        if '/' in name:
            continue
        real = known.get(name.lower())
        if real is None or os.path.basename(real) == name:
            continue    # not ours, or already spelled correctly
        link = os.path.join(out_dir, name)
        if os.path.islink(link):
            os.unlink(link)
        os.symlink(os.path.abspath(real), link)
        made += 1
    print(f"case-compat: {made} alternate spelling(s) in {out_dir}")


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
