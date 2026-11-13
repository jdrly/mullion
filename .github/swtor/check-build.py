#!/usr/bin/env python3
# The checks on the build the app installs, as Stage leaves it in <dir> (.github/workflows/swtor.yml). Prints one
# line per check, "ok" or "FAIL", and exits 1 when one fails:
#
# - <dir> holds exactly the files the archive ships (FILES);
# - x86_64-unix/winemetal.so: no LLVM source paths, which only LLVM's assertions write into a binary; built for
#   macOS 15.0 (minos of LC_BUILD_VERSION); an ad hoc signature that codesign verifies;
# - the internal Metal library (src/mullion/mullion_command.metal), which every DLL that links it carries as a byte
#   array: a container version macOS 26 loads, macOS 15.0 as its OS, and every function built as Metal 3.2.
#
#   python3 -I check-build.py <dir>
import os
import re
import struct
import subprocess
import sys
from collections import Counter

FILES = sorted([
    'LICENSE', 'LICENSE-LLVM.txt', 'SOURCE', 'VERSION',
    'licenses/dxmt-mit.txt', 'licenses/dxmt.txt',
    'licenses/mingw-w64/COPYING', 'licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt',
    'licenses/mingw-w64/COPYING.MinGW-w64.txt', 'licenses/mingw-w64/COPYING.winpthreads.txt',
    'licenses/mingw-w64/COPYING.winstorecompat.txt',
    'x86_64-unix/winemetal.so',
    'x86_64-windows/d3d10core.dll', 'x86_64-windows/d3d11.dll', 'x86_64-windows/d3d12.dll',
    'x86_64-windows/d3d12core.dll', 'x86_64-windows/dxgi.dll', 'x86_64-windows/winemetal.dll',
])
SO = 'x86_64-unix/winemetal.so'
MINOS = (15, 0, 0)
# a container tagged 2.9 gave a black screen on every macOS 26 Mac (d9mt #110); 2.8 is the most macOS 26 reads
CONTAINER_MAX = (2, 8)
# src/mullion/meson.build: -std=metal3.2, which is AIR 2.7
FUNCTION_VERSION = (2, 7, 3, 2)

failed = False


def report(ok, text):
    global failed
    failed |= not ok
    print(('ok    ' if ok else 'FAIL  ') + text)


def check_files(root):
    found = sorted(os.path.relpath(os.path.join(d, f), root) for d, _, fs in os.walk(root) for f in fs)
    for f in sorted(set(FILES) - set(found)):
        report(False, f'missing file: {f}')
    for f in sorted(set(found) - set(FILES)):
        report(False, f'unexpected file: {f}')
    if found == FILES:
        report(True, f'{len(FILES)} files, as the archive ships them')


def macho_minos(data):
    magic, cpu, _, _, ncmds = struct.unpack_from('<IiiII', data, 0)
    if magic != 0xfeedfacf or cpu != 0x01000007:
        return None
    p = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, p)
        if cmd == 0x32:  # LC_BUILD_VERSION: platform, minos as xxxx.yy.zz
            platform, minos = struct.unpack_from('<II', data, p + 8)
            return platform, (minos >> 16, minos >> 8 & 0xff, minos & 0xff)
        p += size
    return None


def check_so(root):
    path = os.path.join(root, SO)
    data = open(path, 'rb').read()
    # the paths LLVM's assert() and llvm_unreachable() write into a binary when LLVM is built with assertions
    n = len(re.findall(rb'/llvm/(?:lib|include/llvm)/', data))
    report(n == 0, f'{SO}: {n} LLVM source paths (assertion text)')
    found = macho_minos(data)
    if found is None:
        report(False, f'{SO}: not an x86_64 Mach-O with LC_BUILD_VERSION')
    else:
        platform, minos = found
        text = '.'.join(map(str, minos[:2] if minos[2] == 0 else minos))
        report(platform == 1 and minos == MINOS, f'{SO}: platform {platform}, minos {text} (macOS 15.0 expected)')
    verify = subprocess.run(['codesign', '--verify', '--strict', path], capture_output=True, text=True)
    shown = subprocess.run(['codesign', '--display', '--verbose=2', path], capture_output=True, text=True)
    adhoc = 'Signature=adhoc' in shown.stderr.splitlines()
    why = (verify.stderr.strip().splitlines() or [''])[-1].replace(path, SO)
    report(verify.returncode == 0 and adhoc,
           f'{SO}: ad hoc signature verifies' if verify.returncode == 0 and adhoc
           else f'{SO}: no ad hoc signature that verifies ({why or "not ad hoc"})')


def metallibs(data):
    # header: "MTLB", platform u16 at 4 (0x8001 macOS), container version u16 major and minor at 6 and 8, file type
    # at 10 (0 executable), OS at 11 (0x81 macOS), OS version u16 major and minor at 12 and 14, file size u64 at 16,
    # function list offset u64 at 24 (src/airconv/metallib_writer.hpp, MTLBHeader)
    for m in re.finditer(rb'MTLB\x01\x80', data):
        o = m.start()
        if o + 88 > len(data) or data[o + 10] != 0 or data[o + 11] != 0x81:
            continue
        size, offset = struct.unpack_from('<QQ', data, o + 16)
        if o + size <= len(data) and offset < size:
            yield o, data[o:o + size]


def functions(blob):
    # the function list: u32 count, then per function u32 size (with itself) and tags (four bytes, u16 length,
    # data) up to ENDT; VERS holds the AIR version and the Metal language version as four u16
    offset = struct.unpack_from('<Q', blob, 24)[0]
    count = struct.unpack_from('<I', blob, offset)[0]
    p = offset + 4
    for _ in range(count):
        size = struct.unpack_from('<I', blob, p)[0]
        q, name, version = p + 4, '?', None
        while blob[q:q + 4] != b'ENDT':
            length = struct.unpack_from('<H', blob, q + 4)[0]
            if blob[q:q + 4] == b'NAME':
                name = blob[q + 6:q + 6 + length].rstrip(b'\0').decode()
            elif blob[q:q + 4] == b'VERS':
                version = struct.unpack_from('<4H', blob, q + 6)
            q += 6 + length
        yield name, version
        p += size


def check_metallib(root):
    seen = 0
    for dll in [f for f in FILES if f.endswith('.dll')]:
        path = os.path.join(root, dll)
        if not os.path.isfile(path):
            continue
        for o, blob in metallibs(open(path, 'rb').read()):
            seen += 1
            container = struct.unpack_from('<HH', blob, 6)
            os_version = struct.unpack_from('<HH', blob, 12)
            fns = list(functions(blob))
            where = f'{dll}: internal metallib at 0x{o:x}'
            report(container <= CONTAINER_MAX, f'{where}: container {container[0]}.{container[1]} '
                   f'(macOS 26 reads up to {CONTAINER_MAX[0]}.{CONTAINER_MAX[1]})')
            report(os_version == MINOS[:2], f'{where}: macOS {os_version[0]}.{os_version[1]} (15.0 expected)')
            wrong = Counter('no VERS' if v is None else 'AIR %d.%d Metal %d.%d' % v for _, v in fns
                            if v != FUNCTION_VERSION)
            right = len(fns) - sum(wrong.values())
            report(right > 0 and not wrong, f'{where}: {right} of {len(fns)} functions AIR %d.%d Metal %d.%d' % FUNCTION_VERSION
                   + ''.join(f', {n} {v}' for v, n in sorted(wrong.items())))
    report(seen > 0, f'internal metallib found in {seen} DLLs')


root = sys.argv[1]
check_files(root)
if os.path.isfile(os.path.join(root, SO)):
    check_so(root)
check_metallib(root)
sys.exit(1 if failed else 0)
