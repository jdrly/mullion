# contract: the converter's codegen id (tools/codegen_id.py, docs/swtor/caches.md) changes with one byte of any file
# under src/airconv or libs/DXBCParser, of a file of the tree they include, of the LLVM or Metal compiler's version or
# of AIRCONV_VERSION; not with a file outside those inputs, the tree's place on disk or the order its files were made
# in; and the same tree gives the same id twice
import importlib.util
import os
import shutil
import sys
import tempfile
from pathlib import Path

failures = 0


def check(ok, what):
    global failures
    if not ok:
        failures += 1
        print(f'failed: {what}')


def load(path):
    sys.dont_write_bytecode = True  # no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location('codegen_id', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# a small source tree in the shape the script reads, its files made in the order given
TREE = [
    ('src/airconv/airconv_public.h', '#define AIRCONV_VERSION 104\n'),
    ('src/airconv/dxbc_converter.cpp', '#include "airconv_public.h"\n#include "../winemetal/winemetal.h"\n'
                                       '#include "tl/generator.hpp"\n#include "llvm/IR/Module.h"\n'
                                       '#include "air_denorm.h"\nint convert;\n'),
    ('src/airconv/shaders/air_denorm.metal', 'kernel void denorm() {}\n'),
    ('src/airconv/nt/air_builder.cpp', '#include "../dxbc_converter.hpp"\n'),
    ('src/airconv/dxbc_converter.hpp', 'struct converter;\n'),
    ('libs/DXBCParser/ShaderBinary.h', '#include "d3dcommon.h"\n'),
    ('include/native/directx/d3dcommon.h', 'typedef int D3D_NAME;\n'),
    ('include/tl/generator.hpp', '#include "adt.hpp"\n'),
    ('include/adt.hpp', 'template <class T> struct adt;\n'),
    ('src/winemetal/winemetal.h', 'enum WMTPixelFormat { };\n'),
    ('src/winemetal/winemetal_thunks.c', 'int thunk;\n'),
    ('src/d3d12/d3d12_device.cpp', '#include "../airconv/airconv_public.h"\nint device;\n'),
    ('tests/host/session.cpp', 'int test;\n'),
]


def make(root, files):
    for path, text in files:
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(text)


def main():
    script = load(sys.argv[1])
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        root = base / 'one'
        make(root, TREE)

        def id_of(where=root, llvm='15.0.7', metal='Apple metal version 32023.620 (metalfe-32023.620)'):
            return script.codegen_id(str(where), llvm, metal)

        first = id_of()
        check(len(first) == 64 and all(c in '0123456789abcdef' for c in first), f'64 hex digits: {first}')
        check(id_of() == first, 'the same tree, the same id')

        # the same files elsewhere, made in the opposite order, through a relative path
        other = base / 'elsewhere' / 'two'
        make(other, list(reversed(TREE)))
        check(id_of(other) == first, 'another place, another order of making')
        here = os.getcwd()
        os.chdir(base / 'elsewhere')
        try:
            check(id_of(Path('two')) == first, 'a relative root')
        finally:
            os.chdir(here)

        inputs = script.inputs(str(root))
        check(inputs == sorted(inputs), 'its inputs in order')
        for path in ('include/adt.hpp', 'include/tl/generator.hpp', 'src/winemetal/winemetal.h',
                     'include/native/directx/d3dcommon.h', 'src/airconv/shaders/air_denorm.metal'):
            check(path in inputs, f'{path} is an input: {inputs}')
        for path in ('src/winemetal/winemetal_thunks.c', 'src/d3d12/d3d12_device.cpp', 'tests/host/session.cpp'):
            check(path not in inputs, f'{path} is no input')

        def changed(path, change):
            target = root / path
            saved = target.read_bytes()
            target.write_bytes(change(saved))
            try:
                return id_of()
            finally:
                target.write_bytes(saved)

        flip = lambda data: data[:-2] + bytes([data[-2] ^ 1]) + data[-1:]
        for path, _ in TREE:
            got = changed(path, flip)
            # every file of the converter, and every file it includes from the tree, down to adt.hpp through
            # tl/generator.hpp; no other file
            want_change = path in inputs
            check((got != first) == want_change,
                  f'one byte of {path} {"changes" if want_change else "keeps"} the id')
        check(changed('src/airconv/dxbc_converter.cpp', lambda d: d + b'// a comment\n') != first,
              'a comment changes it')
        check(changed('src/d3d12/d3d12_device.cpp', lambda d: d + b'// a comment\n') == first,
              'a file outside its inputs keeps it')

        (root / 'src/airconv/transforms').mkdir()
        (root / 'src/airconv/transforms/new.cpp').write_text('int added;\n')
        check(id_of() != first, 'a new file under src/airconv changes it')
        (root / 'src/airconv/transforms/new.cpp').rename(root / 'src/airconv/transforms/renamed.cpp')
        renamed = id_of()
        check(renamed != first, 'a renamed file changes it')
        shutil.rmtree(root / 'src/airconv/transforms')
        check(id_of() == first, 'and back')

        check(id_of(llvm='15.0.6') != first, 'the LLVM version changes it')
        check(id_of(metal='Apple metal version 32023.621 (metalfe-32023.621)') != first,
              'the Metal compiler version changes it')
        check(changed('src/airconv/airconv_public.h', lambda d: d.replace(b'104', b'105')) != first,
              'AIRCONV_VERSION changes it')

        # the build's header: written once, then left alone while the id is the same
        header = base / 'mullion_codegen.h'
        llvm = base / 'llvm/include/llvm/Config'
        llvm.mkdir(parents=True)
        (llvm / 'llvm-config.h').write_text('#define LLVM_VERSION_STRING "15.0.7"\n')
        check(script.llvm_version(str(base / 'llvm')) == '15.0.7', 'the LLVM version from llvm-config.h')
        argv = sys.argv
        try:
            sys.argv = ['codegen_id.py', '--llvm', str(base / 'llvm'), '--metal-version',
                        'Apple metal version 32023.620 (metalfe-32023.620)', '--header', str(header), str(root)]
            script.main()
            text = header.read_text()
            check(f'#define MULLION_CODEGEN_ID "{first}"\n' in text, f'the header holds the id: {text}')
            os.utime(header, (1, 1))
            script.main()
            check(header.stat().st_mtime == 1, 'an unchanged id leaves the header as it was')
        finally:
            sys.argv = argv

    # the real tree: an id, and src/airconv's files among its inputs
    tree = Path(sys.argv[1]).resolve().parent.parent
    if (tree / 'src/airconv/airconv_public.h').is_file():
        real = script.inputs(str(tree))
        check('src/airconv/airconv_public.h' in real and 'libs/DXBCParser/ShaderBinary.h' in real,
              'the tree\'s converter and parser are inputs')
        check(not any(p.startswith(('src/d3d12/', 'src/d3d11/', 'tests/')) for p in real),
              f'no Direct3D or test file is: {real}')

    print(f'{"failed" if failures else "passed"}: {failures} wrong results')
    return failures != 0


if __name__ == '__main__':
    sys.exit(main())
