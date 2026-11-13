# Caches

What the driver keeps between starts, what each one is keyed on, and what makes it start cold. The decision behind
the converted-shader cache's key: [ADR 0003](adr/0003-shader-cache-keyed-on-codegen.md).

| Cache | Where | Key | Starts cold when |
|---|---|---|---|
| Converted shaders | `<user cache>/mullion/<exe>/shaders_<metal>.db` (SQLite, WAL), table `cache_<tag>` | SHA-1 of the codegen id, the macOS build, the device's `registryID`, the function name, the shaders' digests and every compile argument (`CompileFunction`, `src/d3d12/d3d12_pipeline_graphics.cpp`) | The codegen id changes, or the macOS build |
| Pipeline archives | `pipelines_<metal>_<tag>_<n>.metallib` beside it (`MTLBinaryArchive`, `src/d3d12/d3d12_device.cpp`) | Metal's own, by the function's content; the name keeps one identity's shards together | The same |
| A game's pipeline library | The game's file (`ID3D12PipelineLibrary`) | Its header's version is the tag's low 32 bits; its shaders are stored under the keys above | The same: another tag's library fails to load with `D3D12_ERROR_DRIVER_VERSION_MISMATCH`, and the game builds a new one |

`<metal>` is the Metal version shaders are converted for (310, 320, 400). `<tag>` is 16 hex digits, the first 8
bytes of SHA-1 over the codegen id and the macOS build (`src/mullion/mullion_cache_identity.hpp`).

## The codegen id

`tools/codegen_id.py`, which the build runs every time (`meson.build`), writes `MULLION_CODEGEN_ID` into
`mullion_codegen.h`, and the build's output prints it. It is SHA-256 over:

- every file under `src/airconv` and `libs/DXBCParser`, and every file of the tree that they include with quotes,
  found by name under `src`, `include` and `libs`, and what those include. Today that adds `include/adt.hpp`,
  `include/ftl.hpp`, `include/tl/generator.hpp`, the Direct3D headers of `include/native/directx` the converter reads,
  `src/mullion/mullion_command_constants.hpp` and `src/winemetal/winemetal.h`. Each file as its path, its length and
  its bytes, by path;
- LLVM's version (`LLVM_VERSION_STRING` of the LLVM the converter links);
- the first line of `xcrun -sdk macosx metal --version`, the compiler of the converter's `.metal` helpers;
- `AIRCONV_VERSION`.

It is a hash of the sources, not of the object code as in d9mt (ADR 0003): the runner and a Mac build one tree to
one id. The cost is that any byte of those files changes it, a comment too, and with it every player converts every
shader again once. `src/winemetal/winemetal.h` is the biggest such input: every new bridge call changes it.
`AIRCONV_VERSION` stays in the tree as upstream keeps it; raising it changes the id, and forgetting to raise it no
longer serves a stale shader.

The build checks itself: `.github/swtor/check-build.py` computes the id again from the checkout and requires it in
`d3d11.dll`, `d3d12core.dll` and `VERSION` (docs/swtor/README.md § CI).

## When the cache opens

Once per process, Metal version and DLL, at the first pipeline (or just before the session header, which opens it):

1. Under the writers' lock (`<file>-lock`), the bridge makes this tag's table, drops every other `cache_*` table in one
   transaction, and counts this table's rows (`WMTCachePrepare`, `src/winemetal/unix/cache_prepare.h`). Every writer
   takes that lock for each write, so nothing is written during the count.
2. It removes the archives of this Metal version whose names don't carry this tag, those of upstream's naming
   (`pipelines_<metal>_<n>.metallib`) included.
3. The driver writes one line: `Mullion: cacheOpen shaders= codegen= build= entries= sizeMiB= droppedTables=
   droppedShards= droppedMiB=` (docs/swtor/README.md § Log lines).

A dropped table's pages stay in the file, free, and new entries fill them: the file doesn't shrink, and doesn't grow
until the new table is larger than the old one. Dropping a table reads it once, on the first start after an update.

A stored shader Metal refuses is deleted and converted again, as before; the `STATS` line's `CACHE rejected=` section
counts them.

## What a release note says

A build whose `VERSION` codegen line differs from the build before:

> The first start after this update compiles shaders again, and takes longer. The starts after it don't.

A macOS update does the same without a new build. The `cacheOpen` line of that first start shows `entries=0` and the
old table in `droppedTables`.
