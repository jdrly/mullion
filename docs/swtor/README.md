# The SWTOR fork's switches and log lines

Every `MULLION_*` switch this fork adds or changes, and every log line it writes. A pull request that adds or changes
a switch or a log line updates this file in the same pull request. Upstream's own variables are in
[DEBUGGING.md](../DEBUGGING.md), [tracing.md](../tracing.md) and [`mullion.conf`](../../mullion.conf).

How a switch is declared: [ADR 0001](adr/0001-switches.md).

## Switches

A fork switch is declared once, at namespace scope, in the file that reads it, through
[`src/util/util_switch.hpp`](../../src/util/util_switch.hpp):

```cpp
static sw::Switch<bool> kFoo{"MULLION_FOO", true, sw::notZero}; // kFoo() reads the environment at each call
```

The declaration gives the name, the default and the grammar (`sw::notZero` for `!= "0"`, `sw::exactlyOne` for
`== "1"`, or a parser of its own). Declaring it puts it on the session header's `switches` line with its value at
that moment; nothing else is edited. Upstream's variables aren't declared: the header's `env` line shows the ones that
are set.

| Variable | Default | What it does | Off restores |
|---|---|---|---|
| `MULLION_LAYER_WAIT` | `1` | `1`: a present that changes its layer (size, pixel format, scale, colour space) waits for the main thread to set the change, as upstream does. `0`: it goes on, the main thread sets the change later in the order asked (`src/winemetal/unix/layer.c`), and the present draws to the size of the drawable it got. Read once per swap chain. | `1` is upstream's behaviour |
| `MULLION_SESSION_HEADER` | off (`1` turns it on) | Writes the session header (below) when the first Direct3D 12 device is created, with one read-only bridge call for what the Windows side can't read. Off until the Mac run shows the header right on an M1 and an M5 Max; SWTOR on Mac's DX12 dev build turns it on | No header and no bridge call for it |
| `MULLION_STATS` | off (`1` turns it on) | Writes a `STATS` line (below) each second of presenting, and counts what it reports. Read once, when `d3d12core.dll` loads. SWTOR on Mac's DX12 dev build turns it on | No line, and nothing counted: each place that counts tests one flag (below) |
| `MULLION_HITCH_MS` | `50` | With `MULLION_STATS=1`, a frame (Present to Present) this many ms long or longer gets a `HITCH` line, and its `COMPILE` line is at `warn`. Whole ms, 1 to 60000; anything else is 50. Read once, at the first Present counted | Nothing to restore: it only moves a threshold, and `MULLION_STATS` off writes no `HITCH` |
| `MULLION_GPU_ERROR_DETAIL` | off (`1` turns it on) | Every Metal 3 command buffer asks Metal for each encoder's status (`MTLCommandBufferErrorOptionEncoderExecutionStatus`), and every pass's encoder gets a label, so `MULLION-GPU-ERROR` lists the encoders that didn't complete by name (below). Costs a label string a pass and Metal's per-encoder status; off until its frame time is measured on an M5 Max. `MULLION_D3D12_GPU_ERRORS` and `MULLION_TRACE_GPU=heavy` imply it. The bridge reads it too, for the status | No status asked for and no labels but upstream's: `ClearPass`, `ResolvePass`, and the profile's and the heavy pass trace's |

A test hook, not a switch, and only in a build with the tests (`-Denable_tests=true`): `MULLION_TEST_GPU_ERROR=<code>`
makes the bridge read every command buffer that completes as failed with that code of `MTLCommandBufferErrorDomain`,
its encoders a completed one, a faulted one labelled `test` and an affected one. It drives `MULLION-GPU-ERROR`, its
limit and the device's removal without a GPU fault; `tests/d3d12/d3d12_gpu_error.cpp` checks that the device is then
removed with `DXGI_ERROR_DEVICE_HUNG`. A release build doesn't have it.

## Log lines

| Line | When | Where it lands | Who reads it |
|---|---|---|---|
| `Mullion: session <version> (jdrly/mullion swtor-on-mac) d3d12 fl= metal4= shaderMetal= airconv= codegen=` | With `MULLION_SESSION_HEADER=1`, once per process, when the first Direct3D 12 device is created, before its feature level is checked | `d3d12.log` (Wine's output, `wine.log` in the app), at `info` | The app's session report; me, first in every log |
| `Mullion: mac macOS= build= gpu="" family= masked= ram= workingSet= arch= supportsPlacementSparse=` | With the session line | The same | The same |
| `Mullion: switches <NAME=value …>` or `none` | With the session line | The same | The same |
| `Mullion: env <NAME=value …>` or `none` | With the session line | The same | The same |
| `Mullion: cache shaders= entries= sizeMiB=` | With the session line | The same | The same |
| `Mullion: cacheOpen shaders= codegen= build= entries= sizeMiB= droppedTables= droppedShards= droppedMiB=` | When a process opens a shader cache, once per Metal version and DLL: at its first pipeline, or with `MULLION_SESSION_HEADER=1` just before the header, which opens it. Whatever the switches | `d3d12.log` (`d3d11.log` for Direct3D 11), at `info` | Me: which cache, which codegen id, and what an update dropped ([caches.md](caches.md)) |
| `Mullion: layer wait=0` | The first swap chain made with `MULLION_LAYER_WAIT=0`, once per process | `d3d12.log` (Wine's output, `wine.log` in the app), at `info` | Me: the session's presents didn't wait for the main thread |
| `mullion: layer mainThreadWaitMs=<N> applied=no` | A change to a layer has waited 1 s, 5 s and 30 s without the main thread applying one: once at each, with either value of `MULLION_LAYER_WAIT`. `<N>`: ms since the main thread last applied a change, or was sent one with none waiting | The process's stderr (`wine.log` in the app), not `MULLION_LOG_PATH`: the bridge's watch thread writes it | Me, on a freeze: the main thread stopped answering |
| `mullion: layer mainThreadWaitMs=<N> applied=yes` | The main thread applies a change after an `applied=no` line | The same, from the main thread | The same |
| `STATS t= fps= ms=avg/p99/max slow33= slow50= \| cb= rp= cp= draws= \| sh= pso= wait=n/ms cache=hits/misses \| allocMiB= \| at=` | With `MULLION_STATS=1`, at the Present that ends each second of presenting; none for a second without a Present | `d3d12.log` (`wine.log`), at `warn` | The app's session report, which reads d9mt's line of that name; me |
| `<NAME> k=v …` | Right after `STATS`, one line for each registered section that has something to say | The same | The tickets that register one |
| `CACHE rejected=` | The `STATS` section of a window in which Metal refused stored shaders, each then deleted from the cache and converted again | The same | Me: a damaged cache, or one a macOS update made stale |
| `HITCH frame= dtMs= inDriverMs= (pct%) \| <category>=n/ms … or none \| queue: gpuMs= encodeMs= behind= \| at=` | With `MULLION_STATS=1`, at the Present that ends a frame of `MULLION_HITCH_MS` or longer, before that second's `STATS` line | `d3d12.log` (`wine.log`), at `warn` | The app's session report (its ticket 13 reads `inDriverMs` and `pipelineWait`); me, after a stutter |
| `COMPILE frame= dtMs= shaders=n/ms psos=n/ms renderWait=n/ms cache=hits/misses queued=` | With `MULLION_STATS=1`, at the Present that ends a frame in which a shader was converted, a Metal pipeline compiled or a draw waited, or that took 33.4 ms or more; right after its `HITCH` line | The same, at `warn` with a `HITCH` line, else `info` | The app's session report, which reads d9mt's line of that name |
| `MULLION-PIPELINE at= kind= ms= late= variant= name= [[n earlier lines suppressed]]` | Always, for each pipeline made that took 20 ms or more, or was made after the first Present, at most 30 lines in 10 s. Replaces `D3D12 pipeline made: <kind>; <name>; <ms> ms`, which only `MULLION_D3D12_GPU_ERRORS` wrote, and only for late pipelines | `d3d12.log` (`wine.log`), at `info` | Me, after a stutter: which pipeline, and which variant |
| `MULLION-PIPELINE-TOTALS late= ms= longestMs=` | With `MULLION_D3D12_GPU_ERRORS`, at each queue's release: the pipelines made after the first Present, their total and longest ms. Replaces `D3D12 pipeline totals: …` | The same | Me |
| `MULLION-GPU-ERROR at= queue= cmdbuf= frame= error=Name(code) [domain=] first= gpu= encoders= [#i state 'label' …] [+k more] \| <Metal's text> [[n earlier lines suppressed]]` | Always, for each command buffer the GPU failed, at most 30 lines in 10 s. Replaces Direct3D 12's `Device error: queue <pointer>, command buffer <n>: …` and its `fails too` lines in the trace log | `d3d12.log`, at `err`, whatever `MULLION_LOG_LEVEL` is short of `none`; the trace log too while one is open | The app's session report; me, after a GPU fault |

### The session header

What a session ran with, in five lines written together, at `info`, with `MULLION_SESSION_HEADER=1`. Without it (the
default), or with `MULLION_LOG_LEVEL=warn` or above, nothing is written or read for them.
[`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) is a sample;
`host_session` fails when the formatter stops writing it, and the app's session report tests copy it. The field names
and their order are the contract: a field is added at the end of its line, and a renamed or moved field changes the
fixture in the same pull request.

- `session`: `MULLION_VERSION` (`git describe` of the build); the API; `fl` the device's feature level; `metal4` `on`,
  `off` (`MULLION_D3D12_METAL4=0`) or `unsupported` (no Metal 4 family); `shaderMetal` the Metal version shaders are
  converted for (310, 320, 400); `airconv` `AIRCONV_VERSION`; `codegen` the first 12 hex digits of the converter's codegen
  id, which keys the shader cache ([caches.md](caches.md)).
- `mac`: `macOS` the version; `build` the macOS build (`kern.osversion`); `gpu` Metal's device name; `family` the
  GPU's own newest Apple family, past `MULLION_GPU_FAMILY`; `masked` the family the bridge answers as when
  `MULLION_GPU_FAMILY` is set, else `no`; `ram` and `workingSet` (the device's recommended maximum) in MiB; `arch` the
  DLL's architecture, `-rosetta` when the process is translated; `supportsPlacementSparse` 1 or 0, the bridge's
  answer (0 below macOS 26.4).
- `switches`: every switch declared in `d3d12.dll`, by name, with its value now: a bool as `on` or `off`, a number as
  its digits.
- `env`: every `MULLION_*`, `DXMT_*` and `MTL_*` variable set (names of letters, digits and `_`), by name.
- `cache`: the shader cache's file below the user's cache directory (`off` with `MULLION_SHADER_CACHE=0`); `entries`
  the rows of this codegen id's table, which the header gets by opening the cache as the first pipeline would: the
  bridge counts them as a writer, under the writers' lock, when the cache opens (`WMTCachePrepare`), so no other
  process writes during the count (`n/a` when it couldn't); and the file's size in MiB (0 without it), from a
  `stat`.

`build`, `family`, `masked`, `ram`, `arch`'s `-rosetta` and the cache's size come from the bridge
(`WMTGetSessionFacts`), which only reads: it keeps no state and makes no Metal object. A value it can't read is `n/a`.

### The GPU error line

One line for each command buffer the GPU failed, written by the queue that committed it before the device is
removed. [`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) has a sample, which
`host_gpu_error` formats from fixed values; the field names and their order are the contract, as for the header.

- `at`: the local time, `HH:MM:SS.mmm`: the time of day of macOS's unified log, so the line can be matched with the
  `IOGPUMetalError` lines of the app's `gpu-errors.txt` and with Apple's `gpuEvent-*.ips` of that minute.
- `queue`: the queue's number among its device's, from 1, in the order they were made. `cmdbuf`: the queue's own number
  of the command buffer. `frame`: the process's Presents so far.
- `error`: the error's name and Metal's code. The names are the bridge's table of Metal's constants
  (`MTLCommandBufferError`: Internal, Timeout, PageFault, AccessRevoked, NotPermitted, OutOfMemory, InvalidResource,
  Memoryless, StackOverflow; `MTL4CommandQueueError`: Timeout, NotPermitted, OutOfMemory, DeviceRemoved, AccessRevoked,
  Internal); a code the table doesn't know is `Unknown(<code>)`. `domain` only for an error of neither domain.
- `first`: `yes` for the device's first. Metal fails the command buffers after a fault too (`no`): the first is the
  one to read, but one Metal calls an innocent victim of a GPU recovery leaves the one at fault to another command buffer.
- `gpu`: Metal's GPU start and end of the command buffer, in milliseconds of the Mac's uptime clock; `n/a` when Metal
  gave none.
- `encoders`: how many encoders Metal's error lists, then each that didn't complete (`affected`, `pending`,
  `faulted`), by its place from 0 and its label, at most 8, and `+k more`. `n/a` without
  `MULLION_GPU_ERROR_DETAIL=1`, which asks Metal for the list. Metal 4's command buffers have no such option, so on
  Metal 4 the list is whatever Metal's error has (not seen yet on a Mac).
- A label, with `MULLION_GPU_ERROR_DETAIL=1`, is the pass's kind (`render`, `clear`, `compute`, `blit`,
  `acceleration`, and the driver's own `indirect`, `indirect copy`, `upscale`, `timestamps`, `later`, `present copy`,
  `frame dump`, `ResolvePass`), its first pipeline by the first 8 hexadecimal digits of each of its shaders' SHA-1 (as
  `MULLION_SHADER_DUMP_PATH` names them), joined by `+`, and a render or clear pass's targets,
  ` rt0=1920x1080 RGBA8 "SceneColor" ds=1920x1080 D32F`, with the name the game gave the resource (`SetName`) when it
  gave one. The line cuts each label to 40 bytes, and a `'` in it prints as `"`. Metal's label keeps up to 120 bytes.
  The upscaler's and the presenter's own encoders, outside the queue, have no label.
- After ` | `: Metal's own text (`localizedDescription`), printable ASCII, at most 200 bytes, an address of 6
  hexadecimal digits or more as `0x?`.
- What comes from outside (Metal's text, a label, a domain) keeps the rules below: a word with `/` or `\` in it, or
  starting with `~`, prints as `(a path)` (a `file:` URL too), and a field that has the user's or the computer's name in
  it, by the session header's rule, as `(a name)`.
- `[<n> earlier lines suppressed]`: the limit, 30 lines in 10 s for the process, left out that many since the last line.

What it can't say: Metal's error has no faulting address, so the line names the pass and what the pass was built to
draw into, not the buffer a shader read out of bounds. Apple's `gpuEvent-*.ips` report names the fault's kind, and the
app's Collect Logs already picks those up.

Rules every fork log line keeps, this header first:

- `key=value` fields with a space before each key, and no line of 900 bytes or more, so Wine (1018 bytes) never splits
  one. A longer line goes on as `Mullion: <line>+ …` from the field that doesn't fit.
- No absolute host path, user name or home folder. A value from outside Mullion (a variable's, a switch's string)
  follows the app's rule for its own header: one with `/` or `\` prints as `(a path)`, the rest keeps only
  `A-Za-z0-9,.;:_+=-`, cut to 80 characters. The cache's directory prints relative to the user's cache directory,
  and as `(a path)` when `MULLION_SHADER_CACHE_PATH` makes it absolute.
- No user's or computer's name. A value with one in it, in any case, prints as `(a name)`, and a variable whose name
  has one isn't listed. The names are the environment's `USER`, `LOGNAME`, `USERNAME`, `COMPUTERNAME` and `HOSTNAME`
  and the last folder of `HOME` and `USERPROFILE`, their names in any case as Windows reads them; one of 2 or 3
  characters counts only as a whole word, as in the app's leak check. The Mac's full name and other names the app redacts aren't known here: the app's log bundle
  still redacts them.
- No field named `session`, `token`, `account`, `user`, `user_name` or `email`: the app's log bundle blanks such a
  value and drops a file that still names the user. The header's first word is `session` and a space, never
  `session=`.
- One write per line, and a header-only formatter with a host test whose control was seen failing.

### The STATS line

d9mt's `STATS` line in Mullion: one second of presenting, at `warn` so it shows at the app's normal level, from the
thread that presents. The app's session report reads it unchanged: frame rate, frame-time tail, slow frames, compiles,
compile waits, the memory peak, passes and draws. [`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log)
has sixty seconds of it; `host_stats` fails when the window stops writing them, and the app's report prints frame
rows from them.

| Field | What it is |
|---|---|
| `t` | Seconds since the first Present the line counts |
| `fps` | Presents of the window per second of it. A window ends at the first Present a second or more after it began, so a gap of 3 s is one window with its own `t` |
| `ms` | The window's Present-to-Present intervals: average, 99th percentile and longest, in ms. The 99th percentile is d9mt's: the interval at index `n × 0.99` of the window's sorted intervals, so at 100 frames a second or fewer it is the longest |
| `slow33`, `slow50` | The window's intervals longer than 33.4 ms and 50 ms (d9mt's thresholds) |
| `cb` | Per Present: command buffers the queues committed |
| `rp`, `cp` | Per Present: render passes (draw passes and clears) and compute passes in them, as the driver records them (Metal 4 may merge some) |
| `draws` | Per Present: `DrawInstanced` and `DrawIndexedInstanced` calls of the command lists executed, each time a list runs. `ExecuteIndirect` and mesh draws aren't counted |
| `sh` | Shaders of graphics and compute pipelines converted in the window (a shader cache miss, or the cache off). Ray tracing libraries aren't counted |
| `pso` | Metal pipelines compiled in the window: every one with the cache off, else the ones the program's pipeline archive didn't have |
| `wait` | Draws and dispatches that waited for their pipeline to be made, and the total ms they waited |
| `cache` | Shader cache lookups that found the shader / that didn't (and converted it). Shaders reused in the process aren't lookups |
| `allocMiB` | Metal's `currentAllocatedSize` at the line, in MiB |
| `at` | The local time of the line, `HH:MM:SS.mmm` |

The names up to `slow50`, and `sh`, `pso`, `wait` and `allocMiB`, are the report's and are never renamed. A new group
goes after a ` | `, before ` | at=`. A module's own numbers don't widen the line: it registers a section
(`stats::SectionRegistrar` in [`mullion_stats.hpp`](../../src/mullion/mullion_stats.hpp)), and each section with
something to say prints right after `STATS` as `<NAME> k=v …`, cut below 900 bytes. A section's name must not be one the
report reads for something else (`PACE`, `HITCH`, `COMPILE`, `RESTORE`, `XFER`, `THREADCPU`, `RAWMOUSE`, `STATECACHE`,
`DIAG`). The `STATS` line itself stays under 900 bytes for any numbers; `host_stats` tries the largest.

What it doesn't have yet: hidden-window seconds (the report leaves them out using d9mt's presenter visibility lines,
which Mullion doesn't write, so every second counts as shown), `PACE` and `SCREEN`.

Counting and cost. Counters any thread writes are relaxed atomics, read once a second; nothing takes a lock per frame.
Frame times measured with the line on describe a session with it on: compare like with like.

- Off (the default): each Present tests one flag (one load and one branch), as do each command buffer committed, each
  `ExecuteCommandLists`, each Metal pipeline made, each shader converted or found in the cache, and each draw or
  dispatch that has to wait for its pipeline. Each draw call adds one to its command list's count, with no atomic and
  no branch.
- On: each Present also reads the clock and stores its interval; each command buffer adds three relaxed atomics, each
  `ExecuteCommandLists` one, and each counted event above one or two. Once a second the line sorts the second's
  intervals once, reads Metal's allocated size with one bridge call and writes the lines with one log call.
- Only the first swap chain to present is counted, until it is released; then the next one to present takes over, and
  `t` goes on.

### The HITCH and COMPILE lines

d9mt's `HITCH` and `COMPILE` lines in Mullion, at the Present that ends a frame, from the thread that presents, with
`MULLION_STATS=1`. A long frame says where its time went: a compile, a wait inside the driver, the GPU, or the game.
Both come before the `STATS` line of their second, which is where the report looks for them.
[`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) has three frames of them: a 120 ms frame
the presenting thread spent outside the driver (the game's), a 45 ms one, and a 70 ms one it spent 40 ms of in the
driver, waiting for a pipeline. `host_hitch` fails when the frames stop writing them.

`HITCH`:

| Field | What it is |
|---|---|
| `frame` | Presents of the counted swap chain, the one that ends the frame included |
| `dtMs` | The frame: Present to Present, in ms |
| `inDriverMs` (`pct%`) | The time the presenting thread spent in the driver's calls that can block, during the frame, and its share of `dtMs`. This driver's name for d9mt's `inD9mtMs` |
| categories | `name=n/ms`, in this order, each with the calls and their ms: `pipelineWait`, a draw or dispatch waiting for its Metal pipeline (compiled on a compile thread, or made now); `shaderConvert`, `CreateGraphicsPipelineState`, `CreateComputePipelineState` and `CreatePipelineState`, and a shader converted for a variant at draw time; `present`, the Present call up to the end of its frame; `fenceWait`, `SetEventOnCompletion` and `SetEventOnMultipleFenceCompletion` without an event. One with nothing in it is left out; `none` when all are. A call inside another counts in the outer one only, so the categories add up to `inDriverMs` |
| `gpuMs` | GPU time of the command buffers the queues saw complete during the frame, as far as Metal had given it by then: a Metal 3 buffer whose completion was read from its event before Metal completed it counts nothing |
| `encodeMs` | Time the queues spent on command buffers ended during the frame, from taking a buffer's slot to committing it. A buffer encoded on the parallel encode threads counts from the slot on |
| `behind` | Command buffers committed and not yet complete at the line, of all queues |
| `at` | The local time of the line, `HH:MM:SS.mmm` |

`COMPILE`, of all threads, not only the presenting one: `shaders` the shaders converted and the ms of converting each,
making its Metal function and storing it in the cache; `psos` the Metal pipelines compiled (as `STATS`'s `pso`) and
their ms; `renderWait` the draws and dispatches that waited for their pipeline and their ms (as `STATS`'s `wait`);
`cache` the shader cache's hits and misses (as `STATS`'s); `queued` the Metal pipelines left to the compile threads or
to their first draw and not made yet.

`shaders`, `psos`, `renderWait`, `dtMs` and `inDriverMs` are the report's names and are never renamed. A ticket adds a
`HITCH` category with an entry in `hitch::Category` and its name ([`mullion_hitch.hpp`](../../src/mullion/mullion_hitch.hpp)),
and a scope where the thread waits. Both lines stay under 900 bytes for any numbers; `host_hitch` tries the largest.

Cost. Off: each scope tests one flag on entry and a null pointer on exit (one load, one compare and one branch, then
one test: llvm-mingw `-O2`, `cmpb $1, stats::on; je`); each shader converted, Metal pipeline compiled, command buffer
committed or completed, and deferred pipeline made tests one flag. On: a scope reads the clock twice when it is the
thread's outermost one; each Present reads the sixteen counters (relaxed loads; `STATS` read its ten once a second
before) and takes the presenting thread's frame; each shader converted, pipeline compiled and command buffer committed
reads the clock twice and adds one atomic; each command buffer completed adds one atomic, and when Metal says it
completed, two bridge calls for its GPU times (Metal 3) or one (the Metal 4 feedback). A frame with a line reads the
local time and makes one log call.

### The pipeline line

`MULLION-PIPELINE`, always on: a pipeline that took 20 ms or more to make, or was made after the first Present
(`late=yes`), which is a stutter the player sees. At most 30 lines in 10 s for the process (the limit
`MULLION-GPU-ERROR` has); the next line after says how many were left out.

| Field | What it is |
|---|---|
| `at` | The local time, `HH:MM:SS.mmm` |
| `kind` | `graphics`, `mesh` (a mesh shader pipeline), `compute` or `ray` |
| `ms` | From the start of its creation to the pipeline made: conversion, and Metal's compile when it isn't left to a compile thread |
| `late` | `yes` when it ended after the first Present any queue accepted, else `no` |
| `variant` | `base`, the pipeline the program created; `topology`, its variant for a topology with adjacency, made at the first draw of it; `targets`, its variant for the render targets, statistics or depth format a draw has; `sampler-emulation`, the variant a sampler that needs emulation brings at draw time |
| `name` | Its shaders' SHA-1s, joined with commas: what the trace log and dumped shaders call it. No path |

The trace log's `[shader] <kind> pipeline <name>: <ms> ms` line and the profile's `pipeline` span are as they were,
written now whether `MULLION_D3D12_GPU_ERRORS` is set or not (they need `MULLION_TRACE_SHADER` and the profile, as
before), and the first Present is always noted (one load a Present once it is). A mesh shader pipeline's kind there is
`mesh` now. Ray tracing pipelines are always named, which before only `MULLION_D3D12_GPU_ERRORS` did: one SHA-1 of each
library added.

## CI

`.github/workflows/swtor.yml`, workflow `SWTOR`. It runs for a pull request into `swtor-on-mac` and for a push to
`swtor-on-mac`. A change to Markdown files or `docs/` alone starts no run.

**Jobs.**

- **Lint** (`ubuntu-24.04`, pull requests only): actionlint on `swtor.yml`, with shellcheck on its scripts.
- **Build x86_64** (`xcode-27`, Xcode 27.2): the release build the app installs, `meson setup --cross-file
  build-win64.txt -Denable_d3d12=true -Denable_nvapi=false -Denable_nvngx=false --buildtype release --strip`, with
  llvm-mingw for the DLLs, 3Shain's Wine `v8.16-3shain` for Wine's files and LLVM 15.0.7 built without assertions. It
  checks out the whole history, so `git describe` finds upstream's last `v` tag. Stage signs `winemetal.so` ad hoc and
  adds `VERSION`, `SOURCE` and the licences (§ Releases). `.github/swtor/check-build.py` then checks the staged files
  on every run, the push's included, since the push's build is the one that ships: exactly the archive's 18 files;
  `winemetal.so` without LLVM assertion text, built for macOS 15.0 (`minos`), with an ad hoc signature that `codesign`
  verifies; and the internal Metal library (below). The job uploads `mullion-swtor-<commit>` (7 days). For a pull
  request it also runs the host tests in `tests/host` (`meson test 'host_*'`; `host_tessellator` and `host_mutate`
  below), breaks six checks on purpose and requires each to fail, and fails on a compiler warning that
  `.github/swtor/warnings-x86_64.txt` doesn't list. A push run builds, checks and uploads only: the pull request's run
  of the same tree has already passed the rest.

  The third break guards the bridge's availability checks. The build targets macOS 15.0, so clang warns
  (`-Wunguarded-availability-new`) wherever `src/winemetal/unix/winemetal_unix.c` uses Metal API that a newer macOS
  introduced outside an `@available` check, and the baseline lists none of those warnings. The control lowers
  `sparseResidencyInTiles`' macOS 26.4 check to 26.0, adds a use of a macOS 26.0 type outside any check, recompiles
  that one file and requires the warnings check to name both.

  `host_tessellator` takes about 200 s on the runner, which is arm64 and runs the x86_64 host tests under Rosetta; the
  others take about 18 s together. It runs only when the pull request changes what it compiles or how (`src/airconv`,
  `libs/DXBCParser`, `tests/tessellator.hpp`, `tests/reference`, `tests/host`, `tests/meson.build`, `meson.build`,
  `meson.options`, `build-win64.txt`) or the workflow, or when the step can't tell. `host_dxbc_hash` skips itself there:
  it needs directories of real shaders (`MULLION_HASHED`). `host_mutate` never runs there: it tests upstream's mutation
  and test-runner tools, not the driver, and its `test_runner_schedule` needs two short processes to overlap under
  `tests/run.py`'s admission, which reads the machine's real memory pressure; on the runner they once ran one after the
  other.

  The internal Metal library, `src/mullion/mullion_command.metal`, is compiled with `-std=metal3.2
  --target=air64-apple-macos15.0`: macOS 15's language and the driver's oldest macOS. Metal 3.1, the converter's
  helper shaders' language, rejects the file's lambdas (run 43511706762). Without the flags Xcode 27.2 on the runner
  built the same Metal 3.2, AIR 2.7, macOS 15.0 (the artifact of run 43502937095), most likely from
  `MACOSX_DEPLOYMENT_TARGET` in the workflow; a build without that variable gets what its Xcode defaults to.
  Every DLL that links it carries it as a byte array, today `d3d11.dll` and `d3d12core.dll`. The check reads it
  there: header bytes 6 to 9, the container version, at most 2.8 (a container tagged 2.9 gave a black screen on every
  macOS 26 Mac in d9mt); bytes 12 to 15, the macOS version, 15.0; and each function's `VERS` tag, AIR 2.7 and Metal
  3.2. The library isn't in the shader cache, so the change reconverts nothing.

  The fourth break, for the build checks, is a copy of the staged files with a file the archive doesn't ship and
  without `SOURCE`, its `winemetal.so` unsigned, with an LLVM source path appended and `minos` 26.0, and
  `d3d12core.dll`'s internal library tagged 2.9 with its first function AIR 2.9. Each of the seven has to fail.

  The fifth break guards `host_layer_apply`, which runs the bridge's layer setters (`src/winemetal/unix/layer.c`) on
  a `CAMetalLayer` without a device while the test's main thread is held for 2 s without its run loop. Its two patches
  in `tests/breaks/` put `dispatch_sync` back on `MULLION_LAYER_WAIT=0`'s path, and compare a change only with the
  layer, not with the change still on its way to the main thread. The test has to fail on each.

  The build checks also compute the converter's codegen id again from the checkout (`tools/codegen_id.py`,
  [caches.md](caches.md)) and require it in `d3d11.dll` and `d3d12core.dll`, which key the shader cache on it, and in
  `VERSION`. The sixth break is a converter change the build's id doesn't follow: the id of the checkout with one
  comment more in `src/airconv/dxbc_converter.cpp`, against this build. Both DLLs and `VERSION` have to fail.
- **Publish** (`ubuntu-24.04`, after the other jobs): packs the artifact into `mullion-swtor-<commit>.tar.xz` and
  checks that it unpacks to the same files, hash for hash. Only for a push to `swtor-on-mac` does it upload the archive
  (§ Releases). It is the only job with `contents: write`.

**Runner time.** One run at a time for the whole repository: a run in progress is never cancelled, and GitHub keeps one
more waiting. No schedule, no manual trigger. Toolchain caches are saved only on a miss. On a miss LLVM takes about 25
minutes to build; push nothing else while that runs. GitHub lets a pull request's run read only its own caches and
`swtor-on-mac`'s, so after a toolchain change LLVM builds twice: once in that pull request, once in the first push to
`swtor-on-mac`. When `.github/swtor/toolchain.zsh` builds LLVM differently, raise `LLVM_BUILD` in the workflow so the
old cache isn't used.

**Warnings.** The step prints every distinct warning of the build. When a change removes a warning or has to keep a new
one, copy that list into `.github/swtor/warnings-x86_64.txt` in the same pull request.

**What this fork doesn't build.** Upstream's `ci.yml` built eight variants. SWTOR's DirectX 12 client is a 64-bit
program in an x86_64 Wine under Rosetta, and the app installs only `x86_64-windows` and `x86_64-unix`, so the fork
leaves out:

- the 32-bit (`i386-windows`) build: SWTOR's DX12 client has no 32-bit part;
- ARM64EC/ARM64X and the native arm64 macOS build: the engine the app ships is x86_64;
- the gcc builds and the debugoptimized builds. Upstream's releases pack the gcc build, made with whatever Homebrew
  mingw-w64 the runner installs that day; the fork builds the DLLs once, in release, with llvm-mingw pinned by version
  and hash, as upstream's clang release job and host tests do;
- NVAPI and NGX (`nvapi64.dll`, `nvngx.dll`): Apple GPUs have neither;
- upstream's self-hosted GPU tests, which run on a rented Mac instead, and its packaging and release jobs.

**Upstream's workflows.** `ci.yml` (CI Build) and `game-reports.yml` (Game reports) are deleted on `swtor-on-mac`.
`ci.yml` had a cron every five days whose `keep-toolchains` job rebuilt the two macOS LLVMs (x86_64 and arm64) whenever
their cache had expired, and it builds every variant above; `game-reports.yml` writes upstream's wiki from its
discussions. Deleting them fails closed: a later upstream change to either file stops the merge with a modify/delete
conflict, where gating them with an `if:` would let a job upstream adds run here. An upstream merge must keep both
deleted and check that it brings no other file into `.github/workflows/`.

## Releases

Every push to `swtor-on-mac` whose run is green publishes its build as one asset of the pre-release `swtor-builds`:

    https://github.com/jdrly/mullion/releases/download/swtor-builds/mullion-swtor-<full sha>.tar.xz

- **The release.** One rolling pre-release, never marked latest. Its tag `swtor-builds` is lightweight, so
  `git describe` skips it, and it isn't a `v` tag, which belongs to upstream's numbering. The workflow creates it once,
  with the first upload, and then never moves it. An asset is never replaced; the 30 newest stay. After an upload the
  job downloads the asset by its URL, compares it with the SHA-256 and size GitHub reports, and fails if
  `releases/latest` is `swtor-builds`.
- **The archive.** `x86_64-windows/{d3d10core,d3d11,d3d12,d3d12core,dxgi,winemetal}.dll`, `x86_64-unix/winemetal.so`,
  `VERSION`, `SOURCE`, `LICENSE`, `licenses/` with llvm-mingw's MinGW-w64 runtime notices in `licenses/mingw-w64/`
  (the DLLs link that runtime statically), and `LICENSE-LLVM.txt` (LLVM 15.0.7's, checked by hash: airconv links LLVM
  into `winemetal.so`; llvm-mingw's licence for the libc++ in the DLLs is the same file). Names sorted, owner 0, the
  commit's time on every file, one xz thread: the same files give the same archive. The DLLs are llvm-mingw's
  (clang), not the gcc build upstream's releases ship.
- **`VERSION`.** Line 1 is `git describe` (`v0.5-alpha.3-<n>-g<hash>`), then `commit:` (the full sha), `codegen:`
  (the first 12 hex digits of the codegen id that keys the shader cache, then `(airconv <AIRCONV_VERSION>)`), `toolchain:` (Xcode, macOS SDK, `metal --version`, LLVM,
  llvm-mingw and its clang) and `built:` (the run's URL).
- **`SOURCE`.** The repository and the commit the files are built from, as LGPL 2.1 asks.
- **How the app pins it.** The run's summary prints the pin in the form `src/som.sh` takes: `MULLION_URL`,
  `MULLION_API_URL` (the asset's API address), `MULLION_SIZE`, `MULLION_SHA256` and `MULLION_COMMIT`. The app checks
  size and hash on download; refusing an archive whose `VERSION` names another commit comes with its pin of the fork
  (private ticket 11).
- **Who gets it.** Not players, until the GPU gate has passed on that commit (private ticket 15). Until then a fork
  build reaches only development builds of the app. A pull request's run packs the archive but uploads nothing.
