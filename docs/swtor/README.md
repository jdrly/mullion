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
| `MULLION_STATS` | off (`1` turns it on) | Writes a `STATS` line (below) each second of presenting, and counts what it reports. Read once, when `d3d12core.dll` loads. SWTOR on Mac's DX12 dev build turns it on | No line, and nothing counted: each place that counts tests one flag (below). The bridge still keeps `mainWaitMaxMs`' longest wait, one compare-and-swap for each layer change the main thread applies, which nothing reads |
| `MULLION_HITCH_MS` | `50` | With `MULLION_STATS=1`, a frame (Present to Present) this many ms long or longer gets a `HITCH` line, and its `COMPILE` line is at `warn`. Whole ms, 1 to 60000; anything else is 50. Read once, at the first Present counted | Nothing to restore: it only moves a threshold, and `MULLION_STATS` off writes no `HITCH` |
| `MULLION_MEMSTATS` | `0` | A `DIAG MEM` line (below) at each sample of the bridge's memory sampler: `1` every 5 s, `N` from 2 to 3600 every `N` s; anything else is `0`. With it on, the game's `ID3D12Heap`s are counted as they are made and released. Read once, when `d3d12core.dll` loads. SWTOR on Mac's Debug Logging turns it on (private ticket 14) | No `DIAG MEM` line and no heap count. The sampler still runs, every 30 s, for `MULLION-MEMORY-PRESSURE` |
| `MULLION_GPU_ERROR_DETAIL` | off (`1` turns it on) | Every Metal 3 command buffer asks Metal for each encoder's status (`MTLCommandBufferErrorOptionEncoderExecutionStatus`), and every pass's encoder gets a label, so `MULLION-GPU-ERROR` lists the encoders that didn't complete by name (below). Costs a label string a pass and Metal's per-encoder status; off until its frame time is measured on an M5 Max. `MULLION_D3D12_GPU_ERRORS` and `MULLION_TRACE_GPU=heavy` imply it. The bridge reads it too, for the status | No status asked for and no labels but upstream's: `ClearPass`, `ResolvePass`, and the profile's and the heavy pass trace's |
| `MULLION_D3D12_GUARD_STATIC_RANGES` | off (`1` turns it on) | Shaders keep the bounds checks of buffer views reached through descriptor table ranges that root signature 1.1 calls static (no `DESCRIPTORS_VOLATILE`, no `..._KEEPING_BUFFER_BOUNDS_CHECKS`), which upstream dropped in `6e4b44d`: an index past the view reads 0 and a store or atomic there writes nothing, as through a volatile range, instead of reaching memory past the view. Constant buffers, SRVs and UAVs of such ranges; not root descriptors, which have no length to check against (private ticket 20). Read once per process, at the first device. A compile flag (`SM50_SHADER_FLAG_GUARD_STATIC_RANGES`), so the shader cache keys on it and a shader converted one way is never served to the other. Off until the GPU gate has run `d3d12_bounds_guarded` and its frame time is measured on an M5 Max | Upstream's: without the flag the converter decides which ranges are checked as upstream does |
| `MULLION_TEX_NO_COMPRESSION` | off (`1` turns it on) | No texture Mullion makes has Metal's lossless compression (`allowGPUOptimizedContents` NO), Direct3D 11's and 12's and Mullion's own: the tester's switch for the M4's compression bugs ([gpu-families.md](gpu-families.md)), d9mt's `D9MT_TEX_PFV=1`. The bridge reads it once per process and applies it to every texture descriptor and every heap size it gives (`src/winemetal/unix/texture_compression.h`), all but the three it makes and releases at once to ask the GPU what it does (vertex values, sampler reduction, sparse residency), and says so on stderr; `d3d12.dll` declares it for the header and the `textures` line. Costs memory bandwidth on every GPU | Metal's default: every texture made as before |
| `MULLION_TEX_MSAA_COMPRESSION` | `1` | `0`: Direct3D 12's multisampled textures and their resolve targets are made without lossless compression, on every GPU. `auto`: so on a GPU before Apple10 (the family the bridge answers as, so `MULLION_GPU_FAMILY=9` takes it on an M5), Metal's default on Apple10. `1`: Metal's default. d9mt's fix for the M4's flickering 4x MSAA lines (d9mt 3145b30). Direct3D 12 doesn't say which texture a resolve writes: a resolve target is a single-sampled 2D texture of the type group, width and height of a multisampled texture the device made before it (`src/mullion/mullion_compression.hpp`). With it on, `GetResourceAllocationInfo` gives a single-sampled 2D texture room for both descriptors, as a multisampled texture made before the texture is placed turns it into a resolve target. Read as each Direct3D 12 device is made. `1` until the GPU gate turns `auto` on (private ticket 15). Anything else is `1` | `1`: Metal's default for every texture, and each texture's field 0, as before |
| `MULLION_D3D12_RESET_WAITS` | off (`1` turns it on) | A command allocator's `Reset` while lists recorded from it are still queued or on the GPU waits until they have completed, or the device is removed, then resets. Direct3D forbids that `Reset`; upstream logs `CommandAllocator: Reset while N of its submitted lists have not completed` and resets at once, giving the lists' recording back for the next list to record over while the queue may still encode it and the GPU read it ([ADR 0002](adr/0002-gpu-objects-live-until-the-gpu-is-done.md)). No time limit: a reset after a timeout is the same defect, and `MULLION-GPU-HUNG` already reports a command buffer that doesn't complete. A `Reset` with nothing pending reads one atomic, as before, and never reads the switch. Read once, at the first `Reset` with lists pending. Off until the GPU gate (private ticket 15) has run `d3d12_allocator_reset_in_flight` and its break on a Mac; then that ticket turns it on | Upstream's: the `ERR` line and the reset at once |
| `MULLION_SHADER_DUMP_PATH` | unset | Upstream's variable, with a limit now. A folder: every shader of a `MULLION-SHADER` failure (below; not `cache-blob`, not `draw-skipped`) is written there as `<sha1>.dxbc`, each once, at most 32 a process, after which the line says `dumped=limit`. Upstream wrote only the shaders of a failed parse, conversion or Metal pipeline, and without a limit. With `MULLION_TRACE_SHADER=heavy` and no folder, `shaders/` next to the trace log, as upstream. `MULLION_SHADER_DUMP_ALL=1` still writes every shader, without a limit. With a folder, each pipeline keeps a copy of its shaders' bytes, one copy of each shader for the process, so that a pipeline whose Metal compile fails on a compile thread or at draw time, when the game may have freed its bytecode, is dumped too. Off by default: the files are the game's own shaders. SWTOR on Mac sets it only when I ask a player for them, to a folder its Collect Logs doesn't read, so they never go into a log bundle | Unset writes nothing, as upstream. No value brings back unlimited dumps of failures; `MULLION_SHADER_DUMP_ALL=1` writes every shader |
| `MULLION_D3D12_CHECK_RANGES` | off (`1` turns it on) | Checks the offsets, sizes, boxes and footprints a call gives against the resource they point into, and drops the call when one runs past it, with a `MULLION-RANGE` line the first time each check fires and a count in the `RANGE` section ([§ Ranges](#ranges)). Today `CopyBufferRegion` and `CopyTextureRegion` (private ticket 20, part 1), the views of `IASetIndexBuffer` and `IASetVertexBuffers`, which it binds as no view instead (part 2), and `CreateConstantBufferView`, which makes a null view instead, and `WriteBufferImmediate` (part 3); the ticket's later parts add their calls to the same switch. Read once, when `d3d12core.dll` loads. Off until the GPU gate has run `d3d12_copy_ranges`, `d3d12_view_ranges`, `d3d12_cbv_ranges` and their controls on the rented M1 (private ticket 15) | Upstream's: each call goes to Metal as given, after the one test of a flag |

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
| `Mullion: guard staticRanges=1` | The first Direct3D 12 device made with `MULLION_D3D12_GUARD_STATIC_RANGES=1`, once per process | `d3d12.log` (Wine's output, `wine.log` in the app), at `info` | Me: the session's shaders check static ranges' buffers |
| `Mullion: textures compression= msaa= msaaSwitch= apple10=` | Always, as each Direct3D 12 device is made: `compression` `off` with `MULLION_TEX_NO_COMPRESSION=1`, else `metal`; `msaa` what multisampled textures and resolve targets get (`off` also under the first); `msaaSwitch` `MULLION_TEX_MSAA_COMPRESSION`'s value; `apple10` the bridge's answer for Apple10 | `d3d12.log` (Wine's output, `wine.log` in the app), at `info` | Me: whether the M4's compression fix was on |
| `Mullion: texture uncompressed rule= samples= size= format=` | The first texture each rule makes uncompressed on a device: `rule` `msaa` or `resolve`, `samples` the count Metal makes it with (4 for the paired 8x before Apple10), `size` width x height, `format` its `DXGI_FORMAT` number | The same | Me: an `msaa` line without a `resolve` line means the game's resolve target wasn't found (the rule's limits, [gpu-families.md](gpu-families.md)) |
| `mullion: textures compression=off` | With `MULLION_TEX_NO_COMPRESSION=1`, once per process, as the bridge loads: Direct3D 11's processes too | The process's stderr (`wine.log` in the app): the bridge writes it | Me: no texture of the session was compressed |
| `Mullion: layer wait=0` | The first swap chain made with `MULLION_LAYER_WAIT=0`, once per process | `d3d12.log` (Wine's output, `wine.log` in the app), at `info` | Me: the session's presents didn't wait for the main thread |
| `mullion: layer mainThreadWaitMs=<N> applied=no` | A change to a layer has waited 1 s, 5 s and 30 s without the main thread applying one: once at each, with either value of `MULLION_LAYER_WAIT`. `<N>`: ms since the main thread last applied a change, or was sent one with none waiting | The process's stderr (`wine.log` in the app), not `MULLION_LOG_PATH`: the bridge's watch thread writes it | Me, on a freeze: the main thread stopped answering |
| `mullion: layer mainThreadWaitMs=<N> applied=yes` | The main thread applies a change after an `applied=no` line | The same, from the main thread | The same |
| `MtlPresenter: <HH:MM:SS.mmm> visibility 0x<before> -> 0x<now> visible= activeSpace= miniaturized= active= why= [[n earlier lines suppressed]]` | Always, each time the game window's visibility changes: hidden, shown, another Space, the Dock ([§ The window lines](#the-window-lines)). At most 30 lines in 10 s with the line below | The process's stderr (`wine.log` in the app): the bridge writes it from the main thread, as the change happens | The app's session report, which reads d9mt's line of that name for its hidden seconds and its `--mark`; me, on a freeze after Cmd-Tab or a Space switch |
| `mullion: window at= active= visibility=0x<now> why= [[n earlier lines suppressed]]` | Always, each time the app becomes active or stops being active while the window's visibility stays as it was | The same | Me: the last activation change before a freeze |
| `STATS t= fps= ms=avg/p99/max slow33= slow50= \| cb= rp= cp= draws= \| sh= pso= wait=n/ms cache=hits/misses \| allocMiB= \| mainWaitMaxMs= \| at=` | With `MULLION_STATS=1`, at the Present that ends each second of presenting; none for a second without a Present | `d3d12.log` (`wine.log`), at `warn` | The app's session report, which reads d9mt's line of that name; me |
| `<NAME> k=v …` | Right after `STATS`, one line for each registered section that has something to say | The same | The tickets that register one |
| `CACHE rejected=` | The `STATS` section of a window in which Metal refused stored shaders, each then deleted from the cache and converted again | The same | Me: a damaged cache, or one a macOS update made stale |
| `RESETWAIT waits= ms= longestMs=` | The `STATS` section of a window in which a command allocator's `Reset` waited for its lists (`MULLION_D3D12_RESET_WAITS=1`): the waits, their total ms and the longest | The same | Me: how often the game resets an allocator in flight, and what the waits cost |
| `HITCH frame= dtMs= inDriverMs= (pct%) \| <category>=n/ms … or none \| queue: gpuMs= encodeMs= behind= \| at=` | With `MULLION_STATS=1`, at the Present that ends a frame of `MULLION_HITCH_MS` or longer, before that second's `STATS` line | `d3d12.log` (`wine.log`), at `warn` | The app's session report (its ticket 13 reads `inDriverMs` and `pipelineWait`); me, after a stutter |
| `COMPILE frame= dtMs= shaders=n/ms psos=n/ms renderWait=n/ms cache=hits/misses queued=` | With `MULLION_STATS=1`, at the Present that ends a frame in which a shader was converted, a Metal pipeline compiled or a draw waited, or that took 33.4 ms or more; right after its `HITCH` line | The same, at `warn` with a `HITCH` line, else `info` | The app's session report, which reads d9mt's line of that name |
| `MULLION-PIPELINE at= kind= ms= late= variant= name= [[n earlier lines suppressed]]` | Always, for each pipeline made that took 20 ms or more, or was made after the first Present, at most 30 lines in 10 s. Replaces `D3D12 pipeline made: <kind>; <name>; <ms> ms`, which only `MULLION_D3D12_GPU_ERRORS` wrote, and only for late pipelines | `d3d12.log` (`wine.log`), at `info` | Me, after a stutter: which pipeline, and which variant |
| `MULLION-PIPELINE-TOTALS late= ms= longestMs=` | With `MULLION_D3D12_GPU_ERRORS`, at each queue's release: the pipelines made after the first Present, their total and longest ms. Replaces `D3D12 pipeline totals: …` | The same | Me |
| `MULLION-SHADER at= step= stage= sha1= kind= dumped= \| <text> [[n earlier lines suppressed]]` | Always, for each shader or pipeline that fails, at the step it fails at (below), at most 30 lines in 10 s together with `draw-skipped`. Replaces `Failed to initialize shader <sha1>`, `Failed to compile <name>`, `Failed to load <name>`, `Failed to load library shader <name>`, `Failed to create [geometry \| mesh shader \| tessellation \| attachment \| compute] PSO of shader(s) <name>` and `pipeline <name>: the variant that emulates its samplers failed`, and says the refusals that only `FAIL(E_INVALIDARG)`'s warning said before | `d3d12.log` (`wine.log`), at `err`; `step=cache-blob` at `warn` | Me: which shader failed, where, and which file reproduces it |
| `MULLION-SHADER at= step=draw-skipped stage=- sha1= kind= skipped= [[n earlier lines suppressed]]` | Always, once per pipeline, at the first draw or dispatch dropped for want of a Metal pipeline: its compile failed on a compile thread after `CreatePipelineState` returned `S_OK`, a variant failed at draw time, or the draw's topology is one the pipeline can't draw | The same, at `warn` | Me: a missing object has a reason |
| `MULLION-SHADER-TOTALS failed= byStep= skippedDraws= dumped=` | Always, when a Direct3D 12 device is destroyed: the process's so far | The same, at `info` | Me, after a session: `failed=0 byStep=none skippedDraws=0` says every shader converted |
| `MULLION-GPU-ERROR at= queue= cmdbuf= frame= error=Name(code) [domain=] first= gpu= encoders= [#i state 'label' …] [+k more] \| <Metal's text> [[n earlier lines suppressed]]` | Always, for each command buffer the GPU failed, at most 30 lines in 10 s. Replaces Direct3D 12's `Device error: queue <pointer>, command buffer <n>: …` and its `fails too` lines in the trace log | `d3d12.log`, at `err`, whatever `MULLION_LOG_LEVEL` is short of `none`; the trace log too while one is open | The app's session report; me, after a GPU fault |
| `MULLION-DEVICE-REMOVED at= reason=Name(0x…) cause= frame= flight=` | Always, once per device, by the call that removes it, after the flight dump. Replaces `A 64-bit atomic gave up waiting for its lock; removing the device` | `d3d12.log`, at `err`, whatever `MULLION_LOG_LEVEL` is short of `none` | The app's session report; me, when the game quits with `DXGI_ERROR_DEVICE_*` |
| `MULLION-GPU-HUNG at= queue= cmdbuf= waitedMs= status=name(n) encoders=r/c/b present= flight=` | Always, once for a command buffer the queue has waited on for 10 s (2 s, or `MULLION_TRACE_GPU_HANG_MS`, with `MULLION_TRACE_GPU`), after the flight dump. Mullion goes on waiting and the device stays. Replaces `command buffer <n> of queue <pointer> has not completed for …` | `d3d12.log`, at `warn` | The same, on a freeze |
| `MULLION-FEATURE-LEVEL asked= reaches= tiledResourcesTier= macOS=` | Always, each time `D3D12CreateDevice` refuses the feature level asked for. Replaces `D3D12CreateDevice: feature level <n> is not supported` | `d3d12.log`, at `warn` | The same, when the game won't start |
| `MULLION-RESET-WAIT at= type= lists= frame= allocator=` | With `MULLION_D3D12_RESET_WAITS=1`, the first time each command allocator's `Reset` waits for its lists, before it waits: `type` the allocator's list type (`direct`, `bundle`, `compute`, `copy`), `lists` its lists not completed, `frame` the process's Presents so far, `allocator` the allocators of the process that have waited, this one included. A `Reset` that never returns leaves it last | `d3d12.log` (`wine.log`), at `warn` | Me: the game resets allocators in flight, and from which frame |
| `mullion: flight record of <reason> in <executable>.flight` | Each flight dump. The file's name only: the folder is `MULLION_DIAG_PATH`, or `~/Library/Logs/Mullion` | The process's stderr (`wine.log`): the bridge writes it | Me: which dump to open |
| `DIAG MEM frame= allocatedMiB= footprintMiB= footprintPeakMiB= budgetMiB= usageMiB= workingSetMiB= \| system: swapMiB= compressedMiB= freeMiB= pageouts= swapouts= \| heaps=n/MiB \| at=` | With `MULLION_MEMSTATS` on, at the first Present after each sample of the bridge's memory sampler; none while nothing presents | `d3d12.log` (`wine.log`), at `warn` | The app's session report, which reads d9mt's line of that name (`frame`, `allocatedMiB`); me, when a session degrades |
| `MULLION-MEMORY-PRESSURE at= swapMiB= footprintMiB= ramMiB=` | Always, once per process, at the first Present after the first sample in which the Mac swapped out while the process's footprint was over half its RAM, with that sample's numbers and time | The same, at `warn` | Me, on "it got slow after an hour" from an 8 or 16 GB Mac |
| `MULLION-RANGE call= what= given= limit= action=dropped` | With `MULLION_D3D12_CHECK_RANGES=1`, the first time each check drops a call or a view, once per process ([§ Ranges](#ranges)) | `d3d12.log` (`wine.log`), at `warn` | Me: a call the game made with numbers past its resource |
| `RANGE <Call>=n …` | With `MULLION_STATS=1` and `MULLION_D3D12_CHECK_RANGES=1`, a section after `STATS` for a window in which a check dropped a call: how many, by call | `d3d12.log` (`wine.log`), at `warn` | Me: how often, which the `MULLION-RANGE` line doesn't say |

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
| `mainWaitMaxMs` | The longest wait of a layer change for the main thread in the window, in whole ms, a change still waiting counted with its wait so far, as the `mainThreadWaitMs` lines count it: from the change's send, or from the main thread's last applied change when another was ahead of it. 0 when no change waited (a present changes its layer only when its size, format or colour space changes). The bridge keeps the maximum in `src/winemetal/unix/layer.c` and the line takes it once a second with the bridge's call 217 from 0 (`WMTMainThreadWaitMax`), which only reads it and starts it again from 0. `n/a` when the bridge didn't answer. With either value of `MULLION_LAYER_WAIT`; with `1` a present that waits doesn't write its line until the main thread answers (private ticket 26) |
| `at` | The local time of the line, `HH:MM:SS.mmm` |

The names up to `slow50`, and `sh`, `pso`, `wait` and `allocMiB`, are the report's and are never renamed. A new group
goes after a ` | `, before ` | at=`. A module's own numbers don't widen the line: it registers a section
(`stats::SectionRegistrar` in [`mullion_stats.hpp`](../../src/mullion/mullion_stats.hpp)), and each section with
something to say prints right after `STATS` as `<NAME> k=v …`, cut below 900 bytes. A section's name must not be one the
report reads for something else (`PACE`, `HITCH`, `COMPILE`, `RESTORE`, `XFER`, `THREADCPU`, `RAWMOUSE`, `STATECACHE`,
`DIAG`). The `STATS` line itself stays under 900 bytes for any numbers; `host_stats` tries the largest.

Hidden-window seconds aren't in it: the report leaves them out by d9mt's visibility line, which the bridge writes
([§ The window lines](#the-window-lines)). What it doesn't have yet: `PACE` and `SCREEN`.

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
| categories | `name=n/ms`, in this order, each with the calls and their ms: `pipelineWait`, a draw or dispatch waiting for its Metal pipeline (compiled on a compile thread, or made now); `shaderConvert`, `CreateGraphicsPipelineState`, `CreateComputePipelineState` and `CreatePipelineState`, and a shader converted for a variant at draw time; `present`, the Present call up to the end of its frame; `fenceWait`, `SetEventOnCompletion` and `SetEventOnMultipleFenceCompletion` without an event; `allocReset`, a command allocator's `Reset` waiting for its lists (`MULLION_D3D12_RESET_WAITS=1`). One with nothing in it is left out; `none` when all are. A call inside another counts in the outer one only, so the categories add up to `inDriverMs` |
| `gpuMs` | GPU time of the command buffers the queues saw complete during the frame, as far as Metal had given it by then: a Metal 3 buffer whose completion was read from its event before Metal completed it counts nothing |
| `encodeMs` | Time the queues spent on command buffers ended during the frame, from taking a buffer's slot to committing it. A buffer encoded on the parallel encode threads counts from the slot on |
| `behind` | Command buffers committed and not yet complete at the line, of all queues |
| `at` | The local time of the line, `HH:MM:SS.mmm` |

`COMPILE`, of all threads, not only the presenting one: `shaders` the shaders converted (or tried) and the ms of
converting each, making its Metal function and storing it in the cache; `psos` the Metal pipelines compiled and their
ms. Each is counted with its time in the frame it ends in, where `STATS`'s `sh` and `pso` count it as it begins.
`renderWait` the draws and dispatches that waited for their pipeline and their ms (as `STATS`'s `wait`); `cache` the
shader cache's hits and misses (as `STATS`'s); `queued` the Metal pipelines left to the compile threads or to their
first draw and not made yet.

`shaders`, `psos`, `renderWait`, `dtMs` and `inDriverMs` are the report's names and are never renamed. A ticket adds a
`HITCH` category with an entry in `hitch::Category` and its name
([`mullion_hitch.hpp`](../../src/mullion/mullion_hitch.hpp)), and a scope where the thread waits. Both lines stay
under 900 bytes for any numbers; `host_hitch` tries the largest.

Cost. Off: each scope tests one flag on entry and a null pointer on exit (one load, one compare and one branch, then
one test: llvm-mingw `-O2`, `cmpb $1, stats::on; je`); each shader converted, Metal pipeline compiled, command buffer
committed or completed, and deferred pipeline made tests one flag. On: a scope reads the clock twice when it is the
thread's outermost one; each Present reads the eighteen counters (relaxed loads; `STATS` read its ten once a second
before) and takes the presenting thread's frame; each shader converted and pipeline compiled reads the clock twice and
adds two atomics, each command buffer committed reads it twice and adds one; each command buffer completed adds one
atomic, and when Metal says it completed, two bridge calls for its GPU times (Metal 3) or one (the Metal 4 feedback). A
frame with a line reads the local time and makes one log call.

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

### The shader failure lines

`MULLION-SHADER`, always on: a shader or a pipeline that failed, named by the SHA-1 of each of its shaders, at the step
it failed at. d9mt names every failing shader by its SHA-1 at each step in the same way, so one line finds the shader in
a dump folder. [`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) has a sample of each kind,
which `host_shader_lines` formats from fixed values; the field names and their order are the contract, as for the
header. The formatter is [`mullion_shader_lines.hpp`](../../src/mullion/mullion_shader_lines.hpp).

| `step` | Where | Text after ` \| ` |
|---|---|---|
| `container` | Direct3D's checks before the converter reads it: the container's layout and hash, and that the shader is of the stage it is given for. A root signature taken from the shader checks its container too | `the container's layout or hash doesn't hold`, or `a <stage> shader given for the <stage> stage` |
| `features` | A feature the shader requires that the device doesn't have (its `SFI0` part) | `it needs a feature the device doesn't have` |
| `parse` | The converter reading it (`SM50Initialize`) | The converter's |
| `convert` | The converter making the Metal function for this pipeline (`SM50Compile` and the geometry and tessellation pipelines' own) | The converter's |
| `library` | Metal refusing the library the converter just made | Metal's, or `no function <name>` |
| `pipeline` | Metal refusing the pipeline: when it is created, or on a compile thread after `CreatePipelineState` returned `S_OK` | Metal's |
| `variant` | Metal refusing a variant made at draw time: a topology with adjacency, the targets a pass has, or the variant that emulates samplers | Metal's, or `the variant that emulates its samplers failed` |
| `cache-blob` | Metal refusing a shader the shader cache stored, which is then deleted and converted again: once per stored shader, at `warn`. `STATS`' `CACHE rejected=` counts the same refusals | Metal's |
| `pipeline-library` | Metal refusing a shader an `ID3D12PipelineLibrary` stored | Metal's |

| Field | What it is |
|---|---|
| `at` | The local time, `HH:MM:SS.mmm` |
| `stage` | `vs`, `ps`, `gs`, `hs`, `ds`, `cs`, `ms` or `as` for one shader, from its container; `-` for several, or for a container without a program |
| `sha1` | The SHA-1 of each shader's bytes, joined with commas, as `MULLION-PIPELINE`'s `name`. A pipeline whose Metal compile failed on a compile thread or at draw time no longer has the game's bytecode: without a dump folder it is named by its pipeline's name, which is the same SHA-1s, and with one by the copies it kept |
| `kind` | `graphics`, `compute` or `mesh` |
| `dumped` | `yes` when each shader is in `MULLION_SHADER_DUMP_PATH`'s folder (written now or before), `limit` once the process wrote 32, `no` without a folder, for `cache-blob`, or when a file couldn't be written |
| text | The converter's or Metal's own, by the rules every fork line keeps: printable ASCII, a path as `(a path)`, the user's or the computer's name as `(a name)`; at most 300 bytes, and the line under 900 |

`draw-skipped`: the first draw (or dispatch) a pipeline drops for want of a Metal pipeline, once per pipeline;
`skipped` is the process's draws and dispatches dropped so far, this one included. Before, these draws were skipped
without a word. `MULLION-SHADER-TOTALS`, when a device is destroyed: `failed` the lines of every step but `cache-blob`
(its shader is converted again), `byStep` each step's lines, `cache-blob`'s included, `skippedDraws` the dropped draws
and dispatches, `dumped` the files written. The totals count what the 30-line limit leaves out.

`FAIL(E_INVALIDARG)`'s own warning (`InitializeShader: E_INVALIDARG (d3d12_pipeline_graphics.cpp:<line>)`) still
follows a `container`, `features` or `parse` line. What the functions return is as before.

Turning a dumped shader back into the failure, on a Mac with a native build (`build/src/airconv/darwin/airconv`):

    airconv -o /tmp/out.air <folder>/<sha1>.dxbc

A `parse` failure fails there again with the line's text (`host_shader_lines` does it on a garbled shader). A
`convert` failure may need the pipeline's arguments, which the dump doesn't hold (`-root-signature`, the stages
around it). A `container` failure doesn't reproduce: airconv doesn't check a container's hash, and converts one whose
hash doesn't hold. `library`, `pipeline` and `variant` are Metal's compile: they need the Mac, and Metal's own tools.

Not seen on a Mac yet: Metal's own refusals (`library`, `pipeline`, `variant`, `cache-blob`, `pipeline-library`),
the totals at a real session's end (whether SWTOR destroys its device as it quits), and the lines of Mullion's tests
that expect a pipeline to fail.

### The removed device, the hung command buffer and the refused feature level

Three lines, each with an answer the game's own message doesn't give.
[`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) has a sample of each, which
`host_device_lines` formats from fixed values; the field names and their order are the contract, as for the header.
The formatter is [`mullion_device_lines.hpp`](../../src/mullion/mullion_device_lines.hpp).

`MULLION-DEVICE-REMOVED`: why the game's next call gets `DXGI_ERROR_DEVICE_REMOVED` or `…_HUNG`. Written once per
device, by the call that removes it; a later call on a removed device writes nothing.

- `at`: the local time, `HH:MM:SS.mmm`, as `MULLION-GPU-ERROR`'s.
- `reason`: what `GetDeviceRemovedReason` returns, by name and HRESULT: `DXGI_ERROR_DEVICE_HUNG`,
  `DXGI_ERROR_DEVICE_REMOVED` (the game's own `RemoveDevice`) or `DXGI_ERROR_INVALID_CALL`.
- `cause`, from the code that removes it:

  | `cause` | When |
  |---|---|
  | `gpu-error` | Metal failed a command buffer; the `MULLION-GPU-ERROR` line just before says how |
  | `atomic-lock` | A 64-bit atomic gave up waiting for its lock |
  | `app-RemoveDevice` | The game called `ID3D12Device5::RemoveDevice` |
  | `no-command-buffer` | Metal gave no command buffer: for a submission on a Metal 4 queue, or for a part of one on either |
  | `buffer-allocation` | Metal gave no buffer for what a submission needs: the sparse buffer table, or an indirect pass's omitted commands |
  | `open-list-executed` | The game executed a command list it hadn't closed |

- `frame`: the process's Presents so far, as `MULLION-GPU-ERROR`'s.
- `flight`: the flight file's name, `<executable>.flight`, without its folder; `none` when the dump wrote no file.

`MULLION-GPU-HUNG`: a command buffer the queue has waited on for `waitedMs` without Metal completing it. d9mt ends the
process at this point (`D9MT-GPU-HUNG`, exit `0xD9D9`); Mullion doesn't. The queue goes on waiting and the device
isn't removed, so the game may still recover, freeze, or later get a `MULLION-GPU-ERROR` for the same command buffer.

- `queue`, `cmdbuf`: as `MULLION-GPU-ERROR`'s, so the two lines match.
- `status`: Metal's status of the command buffer, by name and number (`committed` 2, `scheduled` 3).
- `encoders`: its passes as the queue encoded them, render (draws and clears) / compute / blit. Resolves, the
  upscaler, acceleration structures and the driver's own small passes aren't counted.
- `present`: `yes` when the command buffer presents.
- `flight`: as above.

`MULLION-FEATURE-LEVEL`: `D3D12CreateDevice` refused the level the game asked for (`asked`), and the device reaches
`reaches`. Level 12_0 needs tiled resources tier 2 (`tiledResourcesTier`), which the device has where Metal has
placement sparse resources: macOS 26.4. On an older `macOS` this line is why the game won't start.

None of the three holds a pointer or a host path. The flight file's name keeps `MULLION-GPU-ERROR`'s text rules, cut to
40 bytes, a space as `_`.

### The memory lines

Memory in the main log, from the samples of the bridge's memory sampler, written at the first Present after each,
from the thread that presents. [`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) has twelve
`DIAG MEM` lines, one each 5 s of its session; `host_memory` fails when the formatter stops writing them, and the app's
report takes the session's memory peak and its frame count from them when the log has no `STATS`.

| Field | What it is |
|---|---|
| `frame` | The process's Presents so far, as `MULLION-GPU-ERROR`'s `frame` |
| `allocatedMiB` | Metal's `currentAllocatedSize` at the line |
| `footprintMiB`, `footprintPeakMiB` | The process's memory footprint at the sample, and its peak (`task_info(TASK_VM_INFO)`: `phys_footprint`, `ledger_phys_footprint_peak`). The whole Wine process: the game, Wine and Mullion |
| `budgetMiB`, `usageMiB` | What `QueryVideoMemoryInfo` tells the game for local memory at the line: its budget (Metal's recommended working set, or less with `dxgi.maxDeviceMemory`) and usage (Metal's allocation) |
| `workingSetMiB` | Metal's `recommendedMaxWorkingSetSize` |
| `swapMiB` | The system's swap in use (`vm.swapusage`) |
| `compressedMiB`, `freeMiB` | The system's compressed and free memory (`host_statistics64`) |
| `pageouts`, `swapouts` | The system's pageouts and swapouts since the sample before; 0 at the first |
| `heaps` | The game's `ID3D12Heap`s alive at the line, and their MiB. Committed resources aren't in it |
| `at` | The local time of the line, `HH:MM:SS.mmm` |

`frame` and `allocatedMiB` are the report's names and are never renamed. Without per-category accounting (d9mt's
`texRT`, `vbibGpu`, ...): Mullion keeps none.

`MULLION-MEMORY-PRESSURE`: the first sample in which the system swapped out since the sample before while the
process's footprint was over half the Mac's RAM (`hw.memsize`). `at` is that sample's local time, `swapMiB` and
`footprintMiB` its numbers as above, `ramMiB` the RAM. Once a process, with `MULLION_MEMSTATS` on or off. The bridge
applies the rule ([`memory_pressure.h`](../../src/winemetal/memory_pressure.h)) to every sample and keeps the first that
meets it, so later samples written while nothing presents don't lose it.

The sampler is one thread for the process, `mullion-memory`, started at the first Present. It also writes the trace's
`[memory] sampler:` sentence every 5 s with `MULLION_TRACE_MEMORY`, as before, so two readers share one thread. Each wake
reads `task_info` once, `host_statistics64` once and `vm.swapusage` once, and writes the sample into a struct of
`d3d12core.dll`'s (`WMTMemorySampler`, the bridge's call 216 from 0), which that DLL never frees. With both switches
off it wakes every 30 s. Each Present pays a static's guard, two loads and a compare to find no new sample; one that
finds one reads it, makes one log call when it has a line, and with `MULLION_MEMSTATS` on asks Metal for its allocation
and working set and `QueryVideoMemoryInfo` for the budget. With it on, each `ID3D12Heap` made or released adds two
relaxed atomics.

Lines come at Presents: a process that stops presenting writes none until it presents again, and gets the pressure line
then.

### The window lines

When the game window is shown, hidden or moved to another Space, and when the app gains or loses activation (Cmd-Tab).
The app's session report leaves out the seconds around a change of visibility and takes its `--mark` from the last
return to the window, as it does on d9mt. The freezes after Cmd-Tab or a Space switch stop the main thread, so these
lines show the last change it saw. The formatter and the watch's steps are in
[`window_lines.h`](../../src/winemetal/unix/window_lines.h); `host_window_lines` tests them, and the
report's patterns, against [`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log), which has a
Cmd-Tab to an app on another Space and back between `STATS t=30` and `t=34`.

`MtlPresenter: 20:00:34.655 visibility 0x3 -> 0x1 visible=1 activeSpace=0 miniaturized=0 active=0 why=space`: d9mt's
line, from its prefix to the second value, so the report reads it unchanged (it looks for `MtlPresenter: <time>
visibility 0x<a> -> 0x<b>`). That part isn't `key=value`, for that reason; the fields after it are.

| Field | What it is |
|---|---|
| time | The local time, `HH:MM:SS.mmm` |
| `0x<before> -> 0x<now>` | d9mt's bits: `0x1` some of the window can be seen (`NSWindowOcclusionStateVisible`), `0x2` it is on the active Space, `0x4` it is in the Dock. Shown is `0x3`. A view not in a window yet counts as `0x3`, as in d9mt |
| `visible`, `activeSpace`, `miniaturized` | The three bits of `now`, 1 or 0 |
| `active` | 1 when the app is the active one (`NSApp.isActive`) |
| `why` | What AppKit said: `occlusion`, `miniaturize`, `deminiaturize`, `space`, `activate`, `resign`, or `view` for the first look at a swap chain's window |

`mullion: window at=20:00:34.412 active=0 visibility=0x3 why=resign`: the activation changed and the visibility didn't. A
line of its own, because the report counts every `MtlPresenter` line as a switch.

Where they come from. The bridge watches the window of the last Metal view a swap chain made (`CreateMetalViewFromHWND`,
and winemetal's own view on a Wine whose winemac.drv exports nothing) until that view is released. It observes AppKit's
notifications for occlusion, the Dock, the Space and activation without a queue, so the main thread writes the line as
the notification arrives, before going on. Each notification reads the window's state; a state that didn't change writes
nothing, and one that changed writes one line. Unlike d9mt, which writes its line at the next Present, these lines don't
wait for a frame: they land between the `STATS` lines of the second they happened in, which is where the report puts
them. The first look assumes `0x3` and active, as d9mt does, so a game that starts shown and active writes nothing.

At most 30 lines in 10 s for the two together (the limit `MULLION-GPU-ERROR` and d9mt have). A line after the limit
says how many lines were left out since the last visibility line. An activation line says so too, but leaves the count
to the next visibility line, the one the report reads. Its `before` is the window's real last state. The report leaves
out the seconds since its last visibility line when a line says lines were left out, or when its `before` differs from
that line's `now`.

Always on, with no switch: it changes no behaviour, and a session writes one to three lines for each Cmd-Tab, Space
switch or trip to the Dock. A session without Debug Logging needs them most: the freezes come without warning. Cost:
six observers on the main thread for the process. Each notification reads four properties of the window and the clock,
and writes one line with one `write` when something changed. Nothing on the presenting thread. The bridge keeps the
watched view retained until it is released, so a notification never reads a view that is gone.

Needs the Mac: AppKit's notifications can't run on GitHub's runner. That a Cmd-Tab and a Space switch each write their
lines, and in which order, has not been seen on a Mac yet.

### Ranges

With `MULLION_D3D12_CHECK_RANGES=1` a call whose numbers run past the resource they point into is dropped: Metal never
sees it. Direct3D calls such a call undefined and its debug layer rejects it; on Windows it can cost the game its
device, on a Mac an out-of-range GPU read or write can restart WindowServer and log the user out. The arithmetic is
[`mullion_ranges.hpp`](../../src/mullion/mullion_ranges.hpp); `host_ranges` tests it. A sum never wraps: an offset
and a size whose sum passes 2^64 run past, as they should.

| Call | Dropped when |
|---|---|
| `CopyBufferRegion` | Either resource isn't a buffer, or `SrcOffset` or `DstOffset` plus `NumBytes` runs past its buffer's width, as Direct3D gave it (`GetDesc().Width`; the Metal allocation may be longer) (`what=` `srcResource`, `srcOffset`, `srcSize` or their `dst` forms) |
| `CopyTextureRegion` | A placed footprint runs past its buffer's width (`srcFootprint`, `dstFootprint`): its offset, then every row of every slice at `RowPitch` but the last row, which needs only its texels, each of the larger size of the footprint's format and the texture's at the other end, which Metal moves (a depth-stencil texture's plane by what a copy of it moves, a stencil byte or a 4-byte depth texel: the format's size is both planes'). The source box runs past the source's level or footprint (`srcX`, `srcY`, `srcZ`). The region it writes from `DstX`, `DstY`, `DstZ` runs past the destination's (`dstX`, `dstY`, `dstZ`). A block-compressed level counts in whole 4-texel blocks, small mips too, and a copy between a compressed format and another carries one block into one texel or back. A copy the call drops anyway (no texture or buffer, an empty box, a format Metal lacks) isn't counted |
| `IASetIndexBuffer` | The view is bound as no view when its `BufferLocation` lies in no live buffer (`viewAddress`, `limit` 0) or its `SizeInBytes` runs past that buffer's end from there (`viewSize`). Draws then read zeros for its indices, as with no index buffer (D3D11.3 8.19.2). A view of no size, or at address 0, isn't checked |
| `IASetVertexBuffers` | The same for each view it sets: that slot is bound as no view, which the vertex fetch reads as zeros |
| `CreateConstantBufferView` | The same for the view's `BufferLocation` and `SizeInBytes`: the slot gets a null view, which shaders read as zeros, as for `BufferLocation` 0. Views are made on any thread, so each thread keeps four ranges of its own, as a command list does |
| `WriteBufferImmediate` | Each write whose 4 bytes at `Dest` lie in no live buffer, or run past its end (`destAddress`, `destSize`), is dropped, a marker's too; the call's other writes go on. Through the command list's ranges, as views are |

`MULLION-RANGE call=CopyBufferRegion what=srcSize given=4096 limit=256 action=dropped`: `call` the Direct3D call,
`what` the side (`src`, `dst`, `view`) and the number, `given` the value the game gave, `limit` the most the resource takes
there: the buffer's width for an offset, what it has past the offset for a size or a footprint, the level's or
footprint's extent for an edge, `1` (`D3D12_RESOURCE_DIMENSION_BUFFER`) where `given` is the dimension of a resource
that isn't a buffer. One line per check per process, for the first call it drops: `CopyBufferRegion` has two checks,
its source and its destination, `CopyTextureRegion` four, its two footprints, its box and its region, and every other
call one. `action` is always `dropped`, for a view too: the view is dropped and nothing bound in its place. `AtomicCopyBufferUINT` and `AtomicCopyBufferUINT64` copy through `CopyBufferRegion` and are checked
and counted as it.

A view's buffer is the one whose address range holds `BufferLocation` in the device's address map, which is how the
index buffer was always found: its end is the Metal allocation's, which can be longer than the width Direct3D gave it
(at least 256 bytes; a write-watched or imported buffer is rounded up), as the address doesn't say which resource it
came from. The memory past the width is that buffer's own. Each command list keeps the address ranges of the last
four buffers its views were found in, as numbers, while no buffer is made or released in the process
(`ranges::buffer_generation`, one counter that never restarts, so a device made where another was never inherits its
ranges): a view inside one of them costs a few compares and one atomic load; another takes the device's
address lock once. A view is checked when it is set: a buffer released after that, with the view still bound,
isn't seen. `ExecuteIndirect`'s views are part 4's.

`RANGE CopyBufferRegion=3 CopyTextureRegion=1`: the calls dropped in the `STATS` window, each call with a count that
isn't 0; no section in a window without one.

Cost. Off: each checked call tests one flag. On: a buffer copy compares two ranges; a texture copy asks for the two
formats' descriptions and the subresources' extents, which the copy asks for again after it. A drop adds a relaxed
atomic and, the first time at its check, writes one line. No lock.

Metal 4's draws and argument tables took a buffer's length from an offset as `length - offset`, which wraps to nearly
2^64 for an offset past the end (`winemetal_unix.c`, `metal4_remaining`): it is now 0 there, whatever the switch
([`buffer_range.h`](../../src/winemetal/unix/buffer_range.h), which `host_ranges` tests). A fix
of undefined behaviour without a switch ([ADR 0001](adr/0001-switches.md)).

A command list keeps the buffer it last found an address in, for index buffers, `WriteBufferImmediate` and the
copies of acceleration structures' data, and `IASetIndexBuffer` skipped a view equal to the one bound last, keeping
its Metal buffer. A game may release a buffer while a list records, and its allocation ends at once when no queue has
work; another buffer may then be made at its address. Both kept the old one: the next lookup read the ended
allocation, and an address of the new buffer could find the old one's Metal buffer. The entry now holds only while
`ranges::buffer_generation` is the one it was found at, with the found buffer's addresses kept as numbers, so a
lookup reads nothing of a buffer the generation doesn't vouch for (private ticket 30, `FoundBuffer` in
[`mullion_ranges.hpp`](../../src/mullion/mullion_ranges.hpp), which `host_ranges` tests). `IASetIndexBuffer` looks
every view up through it, the one bound already too. Whatever the switch: a fix of undefined behaviour without one
([ADR 0001](adr/0001-switches.md)). Cost: a lookup the entry holds adds one atomic load, a plain load on x86_64;
after a buffer is made or released anywhere in the process, each list's next lookup takes the device's address lock
once, as a lookup always did before it was kept. A view bound again costs about a nanosecond more than the skip did,
two with the switch on (g++ -O2 on a Ryzen server, not a Mac).

`tests/d3d12/d3d12_view_ranges.cpp` draws through an index view past its buffer, one at a released buffer's address
and a vertex view past its buffer, between draws through views inside theirs; its control,
`tests/breaks/d3d12_view_ranges.patch`, binds them as given, up to 24 bytes past a buffer or at the released address.

`tests/d3d12/d3d12_cbv_ranges.cpp` reads a constant buffer view of 512 bytes over a 256-byte buffer and one inside it,
and writes immediates to a released buffer's address before one inside a buffer; its control,
`tests/breaks/d3d12_cbv_ranges.patch`, takes both as given.

`tests/d3d12/d3d12_copy_ranges.cpp` runs every kind of drop above and one copy in range after each, and the planes of
two depth-stencil formats through footprints of exactly their bytes, which must not drop, with the switch on;
its control, `tests/breaks/d3d12_copy_ranges.patch`, turns the checks off, which hands Metal copies up to 272 bytes
past a buffer and 4 texels past a texture. Both run on the rented M1 under Metal's validation only (private ticket 15).

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
  below), breaks five checks on purpose and requires each to fail, runs each host test on every break of it in
  `tests/breaks/` and requires it to fail (below, the host breaks), and fails on a compiler warning that
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
  `meson.options`, `build-win64.txt`), one of its breaks or the workflow, or when the step can't tell; its three breaks
  run only when it does, about 200 s each. `host_dxbc_hash` skips itself there:
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

  The build checks also compute the converter's codegen id again from the checkout (`tools/codegen_id.py`,
  [caches.md](caches.md)) and require it in `d3d11.dll` and `d3d12core.dll`, which key the shader cache on it, and in
  `VERSION`. The fifth break is a converter change the build's id doesn't follow: the id of the checkout with one
  comment more in `src/airconv/dxbc_converter.cpp`, against this build. Both DLLs and `VERSION` have to fail.

  The host breaks. Every `tests/breaks/host_<test>[.<n>].patch` is a break of the host test `host_<test>`, and the
  step finds them by name, upstream's and the fork's: it applies each, rebuilds and runs only that test, reverts the
  patch, and requires meson to count the test as failed. A break that doesn't apply, a test that doesn't build, a
  timeout, a skip, a test meson doesn't know and a patch named otherwise each fail the step. A new break needs no
  line in the workflow: the file in `tests/breaks/` is enough, and a break its test doesn't notice fails the pull
  request. The converter's `tests/breaks/d3d12_bounds_guarded.patch` and `.2.patch` run against
  `host_guard_static_ranges` too, named in the step. The breaks below also have to fail at a line the step names.

  `host_layer_apply` runs the bridge's layer setters (`src/winemetal/unix/layer.c`) on a `CAMetalLayer` without a
  device while the test's main thread is held for 2 s without its run loop. Its four breaks put `dispatch_sync` back
  on `MULLION_LAYER_WAIT=0`'s path, compare a change only with the layer, not with the change still on its way to the
  main thread, leave `STATS`' `mainWaitMaxMs` without its reset, and leave out of it a change still waiting.

  `MULLION_D3D12_GUARD_STATIC_RANGES` in the converter: `host_guard_static_ranges` builds a
  compute shader that reads a constant buffer by a dynamic index, loads from a structured SRV, and stores and adds to
  a structured UAV, all through one descriptor table, and has the build's native `airconv` write it as LLVM assembly
  for that table's ranges static and volatile, with and without `-guard-static-ranges`. Static with the flag has to be
  volatile's output byte for byte, static without it has to differ, and the flag must not change volatile's. Its
  breaks, `tests/breaks/d3d12_bounds_guarded.patch`, `.2.patch` and `host_guard_static_ranges.patch`, leave a static
  range's SRVs, UAVs and constant buffers unchecked under the flag.

  The shader cache's key against the same switch: `host_compile_key` compiles
  `CompileFunction`'s own loop over the compile arguments, which `tests/host/compile_key.py` copies out of
  `src/d3d12/d3d12_pipeline_graphics.cpp` at build time, and requires two argument chains that differ only in
  `SM50_SHADER_FLAG_GUARD_STATIC_RANGES` to give two keys, and prints both. Its break,
  `tests/breaks/host_compile_key.patch`, leaves the flags out of the key.

  Lossless texture compression: `host_texture_compression` builds the bridge's
  `texture_compression.h` natively, requires a descriptor whose `WMTTextureInfo.uncompressed` is 1, or any with
  `MULLION_TEX_NO_COMPRESSION=1`, to have `allowGPUOptimizedContents` NO and one with neither to keep Metal's YES, and,
  where the runner has a Metal device, has Metal make a 4x and a single-sampled target each way and requires the one
  asked for back without compression (it prints whether the device compresses the other). `host_compression` runs
  `MULLION_TEX_MSAA_COMPRESSION`'s rule and lines. Their breaks: `tests/breaks/host_texture_compression.patch` leaves
  the field out of the bridge, `host_compression.patch` lets `auto` ignore the family, `host_compression.2.patch`
  leaves a resolve target's type group out.

  `MULLION_D3D12_CHECK_RANGES`'s arithmetic: `host_ranges` builds
  [`mullion_ranges.hpp`](../../src/mullion/mullion_ranges.hpp) natively and checks ranges of bytes (a sum that wraps
  past 2^64 among them), views and the address ranges they keep, the buffer a command list last found an address in,
  boxes, block-compressed extents, footprints, the bridge's Metal 4 lengths, the `MULLION-RANGE` line and the `RANGE`
  section. Its breaks: `tests/breaks/host_ranges.patch` computes a range's end as `offset + size`, which wraps,
  `host_ranges.2.patch` counts one slice of a footprint whatever its depth, `host_ranges.3.patch` computes a view's
  end as `address + size`, `host_ranges.4.patch` gives the bridge's Metal 4 length from an offset as
  `length - offset`, which wraps past the end, `host_ranges.5.patch` keeps found address ranges after a buffer is made
  or released, and `host_ranges.8.patch` keeps a command list's found buffer after one is.

  The window lines: `host_window_lines` builds [`window_lines.h`](../../src/winemetal/unix/window_lines.h) natively
  and checks the two lines against the report's patterns, the limit, and the fixture's lines. Its breaks:
  `tests/breaks/host_window_lines.patch` writes an activation alone as d9mt's visibility line, which the report would
  count as a switch, `host_window_lines.2.patch` updates the watch's last state only for the lines it writes, so the
  line after the limit starts from the wrong state, and `host_window_lines.3.patch` lets an activation line clear the
  count of lines left out, so the visibility line after it doesn't carry it.
- **Publish** (`ubuntu-24.04`, after the other jobs): packs the artifact into `mullion-swtor-<commit>.tar.xz` and
  checks that it unpacks to the same files, hash for hash. Only for a push to `swtor-on-mac` does it upload the archive
  (§ Releases). It is the only job with `contents: write`.

**Runner time.** One run at a time for each branch (`swtor-<ref>`): a run in progress is never cancelled, and GitHub
keeps one more of that branch waiting, which a newer push to the same branch replaces. Another pull request's push
never drops a run, and nothing drops a push run of `swtor-on-mac` but a later push to it. With three pull requests
open that is up to four runs at once, about 8 minutes each with warm caches, measured before the step ran every host
break; `host_tessellator`'s three breaks add about 10 minutes when they run. A push run's Publish waits in
`swtor-publish`, one for the whole repository, so two uploads never overlap. No schedule, no manual trigger. Toolchain caches are saved only on a miss. On a miss LLVM takes about 25
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
