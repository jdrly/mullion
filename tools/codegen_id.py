#!/usr/bin/env python3
# The converter's codegen id: what the shader cache is keyed on in place of AIRCONV_VERSION, a number raised by hand
# (docs/swtor/caches.md). SHA-256 over, in this order:
#
# - every file under src/airconv and libs/DXBCParser (the converter and the parser it reads DXBC with), and every file
#   of the source tree they include with quotes, and what those include, found by name under src, include and libs.
#   Each as its path relative to the source root, its length and its bytes, sorted by path: no absolute path, no
#   time, no order of the file system's;
# - the LLVM version the converter is built with (LLVM_VERSION_STRING of <llvm>/include/llvm/Config/llvm-config.h);
# - the Metal compiler's version (the first line of `xcrun -sdk macosx metal --version`; the others name its folder),
#   which compiles the converter's .metal helpers into what it links;
# - AIRCONV_VERSION.
#
# A source hash, so that the runner's build and a Mac's build of one tree have one id. Any byte of those files, a
# comment too, gives a new id and so a cold cache, never a stale one.
#
#   codegen_id.py [--llvm <dir> | --llvm-version <v>] [--metal-version <v>] [--header <file>] <source root>
#
# prints the id, and with --header writes `#define MULLION_CODEGEN_ID "<id>"` there when it differs from what the file
# holds, so the build recompiles only what includes it, and only when the id changes. Fails when an input can't be
# read: a build without an id doesn't build.
import argparse
import hashlib
import os
import re
import subprocess
import sys

DIRECTORIES = ('src/airconv', 'libs/DXBCParser')
# where an included name is looked for
SEARCHED = ('src', 'include', 'libs')
INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"]+)"', re.M)


def tree_files(root, top):
    found = []
    for directory, subdirectories, files in os.walk(os.path.join(root, top)):
        found += [os.path.relpath(os.path.join(directory, f), root).replace(os.sep, '/') for f in files]
    return found


def inputs(root):
    """the source files the id hashes, relative to root, sorted"""
    searched = [f for top in SEARCHED for f in tree_files(root, top)]
    known = set(searched)
    files = set(f for top in DIRECTORIES for f in tree_files(root, top))
    pending = sorted(files)
    while pending:
        path = pending.pop()
        with open(os.path.join(root, path), 'rb') as f:
            names = INCLUDE.findall(f.read())
        for name in names:
            name = name.decode('utf-8', 'replace')
            beside = os.path.normpath(os.path.join(os.path.dirname(path), name)).replace(os.sep, '/')
            # beside the file that includes it, else every file of that name in the tree: LLVM's and the build's own
            # headers (air_denorm.h, from shaders/air_denorm.metal) are found nowhere and are left to the versions
            found = [beside] if beside in known else [f for f in searched if f == name or f.endswith('/' + name)]
            for f in found:
                if f not in files:
                    files.add(f)
                    pending.append(f)
    return sorted(files)


def codegen_id(root, llvm_version, metal_version):
    h = hashlib.sha256()

    def part(name, data):
        h.update(name.encode() + b'\0' + str(len(data)).encode() + b'\0' + data)

    for path in inputs(root):
        with open(os.path.join(root, path), 'rb') as f:
            part('file ' + path, f.read())
    with open(os.path.join(root, 'src/airconv/airconv_public.h'), 'rb') as f:
        airconv = re.search(rb'^#define AIRCONV_VERSION (\d+)', f.read(), re.M)
    if not airconv:
        raise SystemExit('codegen_id: no AIRCONV_VERSION in src/airconv/airconv_public.h')
    part('llvm', llvm_version.encode())
    part('metal', metal_version.encode())
    part('airconv', airconv.group(1))
    return h.hexdigest()


def llvm_version(llvm):
    with open(os.path.join(llvm, 'include/llvm/Config/llvm-config.h'), 'rb') as f:
        found = re.search(rb'^#define LLVM_VERSION_STRING "([^"]+)"', f.read(), re.M)
    if not found:
        raise SystemExit(f'codegen_id: no LLVM_VERSION_STRING in {llvm}/include/llvm/Config/llvm-config.h')
    return found.group(1).decode()


def metal_version():
    out = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '--version'], check=True, capture_output=True, text=True)
    first = out.stdout.splitlines()[0].strip() if out.stdout.strip() else ''
    if not first:
        raise SystemExit('codegen_id: `xcrun -sdk macosx metal --version` printed nothing')
    return first


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--llvm', help="the converter's LLVM, the folder with include/llvm/Config/llvm-config.h")
    parser.add_argument('--llvm-version')
    parser.add_argument('--metal-version')
    parser.add_argument('--header')
    parser.add_argument('root')
    args = parser.parse_args()
    if not args.llvm_version and not args.llvm:
        parser.error('--llvm or --llvm-version')
    llvm = args.llvm_version or llvm_version(args.llvm)
    metal = args.metal_version or metal_version()
    found = codegen_id(args.root, llvm, metal)
    if args.header:
        text = ('#pragma once\n// tools/codegen_id.py: the converter\'s codegen id, which keys the shader cache '
                '(docs/swtor/caches.md)\n' f'#define MULLION_CODEGEN_ID "{found}"\n')
        try:
            with open(args.header) as f:
                same = f.read() == text
        except FileNotFoundError:
            same = False
        if not same:
            with open(args.header, 'w') as f:
                f.write(text)
        print(f'codegen id {found} (LLVM {llvm}, {metal})')
    else:
        print(found)


if __name__ == '__main__':
    sys.exit(main())
