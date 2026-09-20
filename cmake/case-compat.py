#!/usr/bin/env python3
"""Make case-insensitive includes resolvable on a case-sensitive file system.

Windows file names do not care about case, so headers written there spell each
other's names however they like: the Deus Ex SDK's Core.h asks for "UnCid.h"
when the file is "UnCId.h", and D3D11Drv asks for <D3D11.h> when the Windows SDK
ships "d3d11.h". This scans a set of directories for the includes they ask for,
resolves them case-insensitively against a set of search directories, and writes
a directory of symlinks under the spellings that were actually used. The build
puts that directory last on the include path, so nothing shadows a real header.

  --out     where to write the symlinks
  --scan    directories whose sources are read to find the include names
  --search  directories the names are resolved against (defaults to --scan)
"""
import os
import sys
import re

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SOURCE_SUFFIXES = ('.h', '.hpp', '.c', '.cpp', '.cc', '.inl')


def main(argv):
    out_dir = None
    scan_dirs = []
    search_dirs = []
    current = None
    for arg in argv:
        if arg == '--out':
            current = 'out'
        elif arg == '--scan':
            current = 'scan'
        elif arg == '--search':
            current = 'search'
        elif current == 'out':
            out_dir = arg
        elif current == 'scan':
            scan_dirs.append(arg)
        elif current == 'search':
            search_dirs.append(arg)
        else:
            raise SystemExit('case-compat: unexpected argument %r' % arg)

    if not out_dir or not scan_dirs:
        raise SystemExit('case-compat: --out and --scan are required')
    if not search_dirs:
        search_dirs = scan_dirs

    known = {}          # lowercase name -> real path
    for d in search_dirs:
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            known.setdefault(name.lower(), os.path.join(d, name))

    wanted = set()
    for d in scan_dirs:
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            path = os.path.join(d, name)
            if not os.path.isfile(path):
                continue
            if not name.lower().endswith(SOURCE_SUFFIXES):
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
        elif os.path.exists(link):
            continue
        os.symlink(os.path.abspath(real), link)
        made += 1
    print("case-compat: %d alternate spelling(s) in %s" % (made, out_dir))


if __name__ == '__main__':
    main(sys.argv[1:])
