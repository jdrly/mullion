# The GPU gate

The fork's tests on a real Apple GPU, under Metal's validation, with failing controls (private ticket 15). GitHub's
hosted runners have no Metal 4 and no Apple GPU worth the name, so this runs on a rented Mac mini and, for the newest
family, on the maintainer's M5 Max.

**The rule.** A release for players needs this gate green on the commit it ships, on the M1 (this page) and on the M5
Max (`--family native,9`, not run yet). Green means: every control fails its test, and the run differs from the
family's known list ([`tests/swtor/known-m1.txt`](../../tests/swtor/known-m1.txt)) in nothing, or every difference is
explained in the pull request that ships. Upstream's batches go through it as [upstream.md § 7](upstream.md#7-the-gate)
says. A switch turns on by default only when its tests passed here with their control, unless the off state is the
defect itself (ADR 0001's reasoning, scoped in [ADR 0004](adr/0004-the-drawable-is-taken-off-the-commit-path.md)).

## 1. How it runs

On the rented Mac (Apple M1, 8 GB, macOS 27.0), by hand over SSH, with the scripts of the private `jdrly/gpu-ci`
(`mullion/gate.zsh`, `mullion/chain.zsh`). Every step is its own ticket in the machine's queue (`ciq`), which it shares
with another project; a chain runs the slices one ticket at a time, so the other side's tickets go in between.

```sh
gate.zsh setup <commit>      # the source, meson and ninja in a venv, llvm-mingw and Wine at swtor.yml's pins
gate.zsh build               # -Denable_tests=true -Db_ndebug=false, -j 2, then tests/compilers.py
gate.zsh run <slice> native --jobs 1 --validate <tests…>          # tests/run.py --exact, --record per slice
gate.zsh run <slice> native-metal3 --jobs 1 --validate <tests…>
gate.zsh prefault            # tests/faults.py's copy, built once at -j 2
gate.zsh faults <slice> --validate <patch>                         # tests/faults.py
gate.zsh control <slice> <patch> <tests…>                          # faults.py's steps by hand (§ 4)
```

- **From SSH, not from the GUI session.** Started inside the logged-in session, every test that creates a DXGI
  adapter hung 600 s in `GetAdapterLuid` (`src/dxgi/dxgi_adapter.cpp`): with Wine's Mac display driver live it asks
  Wine for the primary display's adapter (`D3DKMTOpenAdapterFromGdiDisplayName`), and that call blocks in the test
  Wine (v8.16-3shain) on macOS 27.0. From SSH there is no display and it falls back to Metal's registry ID at once.
  A test-harness and test-Wine problem: the game runs in the GUI session on the app's CrossOver engine and gets
  through DXGI. So the window suite doesn't run here (§ 5).
- **`--jobs 1`.** Under validation the kernel's footprint of `d3d12_raster`, `d3d12_raster_samples`,
  `d3d12_format_blend` and `d3d12_statistics_heap` is 4-6 GB (GPU memory included); at `--jobs 2` a slice grew swap by
  391 MiB and the queue stopped it. Those four get no verdict on 8 GB under validation: run.py's own memory guard
  ends them at pressure level 2, and the later slices leave them out.
- **Tests by name.** There is no `d3d12` suite (meson's are `mullion`, `window`, `host`, `timing`): the slices name the
  `d3d12_` tests with `--exact`.
- **Build differences from swtor.yml.** meson 1.11.2 (the Mac's Python is 3.9; swtor.yml pins 1.12.1, which wants
  3.10), the machine's LLVM 15.0.7 with deployment target 26.0 (swtor.yml builds it for 15.0), Xcode 27.0 (27.2), tests
  and assertions on, no strip. llvm-mingw and the Wine are swtor.yml's, SHA-256 checked. A Homebrew GCC MinGW on the
  machine's `PATH` must not build the DLLs: `gate.zsh` puts llvm-mingw's prefixed tools first and checks it.

## 2. The first run: efee691 on the M1, 2026-11-14/15

`jdrly/mullion` efee691 (`swtor-on-mac` after upstream batch 2), Apple M1 (Apple7), macOS 27.0 (26A428), Xcode 27.0,
Wine v8.16-3shain x86_64 under Rosetta. 552 `d3d12_` tests (the window suite's 19 and `d3d12_statistics_heap`'s two
apart), each under `--validate`, as the GPU's own family on Metal 4 (`native`) and on Metal 3 (`native-metal3`), and a
third pass with the three switches of § 4 set on in every test's environment, as their defaults would be.

| pass | tests | passed | failed | skipped | time |
|---|---|---|---|---|---|
| `native` | 552 | 348 | 200 | 4 | 80 min (r1 at `--jobs 2`) |
| `native-metal3` | 546 | 362 | 178 | 6 | 56 min |
| `native`, switches on | 546 | 349 | 193 | 4 | 53 min |

By slice: h1 (25 high-path tests: the fork's range, bounds, allocator, texture tests, `d3d12_release_in_flight`,
`d3d12_fetch`) 97 s / 33 s / 33 s; r1-r4 (126-133 tests each) 239-2364 s. A skip is never counted as a pass.

**Why 200 fail.** The baseline is wide on purpose: it is the M1 under validation, which nobody had run. By cause
([`tests/swtor/known-m1-validated.txt`](../../tests/swtor/known-m1-validated.txt) has the line of each, with its
cause):

| cause | native | native-metal3 |
|---|---|---|
| Apple's shader validation crashed compiling a pipeline, the draw skipped (below) | 68 | 67 |
| unexplained: wrong results no rule accounts for | 36 | 44 |
| Metal API or shader validation reports (INF/NaN interpolants, encoder assertions) | 30 | 26 |
| hang under validation on Metal 4 that passes on Metal 3 (#165, [TESTING.md 6.1](../TESTING.md#6-running-tests-and-reading-results)) | 14 | 0 |
| Metal validation: invalid load in a shader, no fork switch | 14 | 14 |
| Metal asserts `Device does not support MTLMatrixLayoutRowMajor` (Apple7; fixed by #45) | 8 | 10 |
| no verdict: memory pressure (8 GB) | 8 | 3 |
| hang, also on Metal 3 or not run there | 8 | 2 |
| the range checks' invalid loads (fixed by #39, 1bd0bb9) | 6 | 6 |
| opens a window: none from SSH | 5 | 5 |
| by design (`d3d12_corpus`) or the test's own skip under validation (`d3d12_shader_groups`) | 4 | 4 |
| upstream's M5 Pro causes, other counts (`d3d12_raster_bias`) | 2 | 2 |
| `d3d12_texture_compression` prints no verdict (its checks held) | 1 | 1 |

- **The compiler crash.** In 135 failures Apple's `MTLCompilerService` died while building a pipeline
  (`XPC_ERROR_CONNECTION_INTERRUPTED`, `MULLION-SHADER step=pipeline`); the draw that needed it was skipped, and the
  test read wrong results. Its crash reports (`~/Library/Logs/DiagnosticReports/MTLCompilerService-*.ips`) stop in
  shader validation's own instrumentation: `MTLCompilerObject::addDebugInstrumentationToModule` →
  `TextureCheckPass::getCheckedTextureFunction` → SIGSEGV in `llvm::ConstantInt::get` (22 of the 23 reports of
  the two days; the 23rd stopped in the backend's `MachineFunctionPass`). Without validation the same pipelines build:
  `d3d12_msaa8`, `d3d12_view_types` and `d3d12_mipchain`, 88, 172 and 27 wrong results under validation, pass. A
  validation failure, not one a player meets, and the reason for § 2a.
- **Invalid loads.** The six range-check tests (`d3d12_cbv_ranges`, `d3d12_root_view_ranges{,_indirect}`) read memory
  validation calls invalid on their checked path: a constant-buffer row past its bound read the shader's own zero row
  (`@mullion.zero_row`, upstream `adb8298`), where validation sees no resource. Fixed by #39 (1bd0bb9).
  `d3d12_cbuffer_layout` and `d3d12_compute` showed the same report with no fork switch involved: the same zero row on
  upstream's path, read by a table CBV's row past its view and by a null view. Fixed by #47 (private ticket 43): both
  clean on both paths, and a control that gives the shader its own row back brings the gate's addresses back.
  **The zero row's reports were validation's own** (private ticket 44, below), so #39 and #47 moved a row nothing
  misread. Both were reverted (Jan, 2026-11-16): the shader reads its own row again, and these tests get the report
  again under validation, as validation's.
  - **The evidence.** #47's control (the shader's own row back) brings the reports back in `d3d12_compute`,
    `d3d12_cbuffer_layout` and `d3d12_root_view_ranges` (#39's checked root CBV).
  - **The control.** With D3D12's pipelines also made without indirect command buffer support, the same tests report
    none and pass.
  - **Not rerun at e26baa3.** #39's other case, `d3d12_cbv_ranges`' null view of size 0, read the same row. With
    the switch on, #39 still gave that view 64 KiB then; the revert's own run covers it.
  `d3d12_gather_dxil`, `d3d12_layers`, `d3d12_pipeline_statistics`, `d3d12_sampler_table_lod_dxbc` and
  `d3d12_viewport_reach` report the same addresses with that fix as without it. Each of their loads reads a constant
  array of the shader itself: DXBC's immediate constant buffer (`@icb`, and in `d3d12_sampler_table_lod_dxbc` the rows
  fxc builds to pick a vector's component by index) or an array DXC keeps in the constant address space (`axes`,
  `corners`).
  **These reports, and the zero row's, are Metal's validation, not reads past memory** (private ticket 44, at
  e26baa3). Shader validation calls every load of a pipeline's own program-scope constants invalid, and zeroes it,
  when the pipeline supports indirect command buffers. Mullion's Direct3D 12 compute pipelines and its vertex and
  fragment render pipelines support them, for `ExecuteIndirect` (mesh pipelines only from Apple9). Upstream's
  [TESTING.md](../TESTING.md) § 6 says the same of macOS 27.2.
  - **Apple's own compiler.** On the M1 a Metal kernel built by Apple's compiler reads a `constant float3 axes[6]`
    per thread, outside Wine and Mullion. In a pipeline made with `supportIndirectCommandBuffers` and run under
    validation, it gets the same report (`Invalid load in 'device' or 'constant' address space at GPU address
    0x11000…`) and reads zeros, dispatched directly or through an indirect command buffer. Without that support, or
    without validation, it reads the right values, through an indirect command buffer too. A kernel with no
    constant array never gets the report.
  - **Ruled out.** Apple's kernel changed to Mullion's forms still reads its values without the report in a pipeline
    without that support. Each change ran alone and in the combinations tried: the element's `getelementptr`
    without `inbounds`, a sign-extended index, `@icb`'s `align 4`, airconv's header (`macosx15.0.0`, AIR 2.7,
    Metal 3.2), and the library written by airconv's metallib writer.
  - **Inside Mullion.** With D3D12's pipelines made without that support (a diagnostic patch, kept in the workspace:
    `ExecuteIndirect` needs the support), the eight tests above report no invalid load on Metal 4 or Metal 3, and
    six of them pass. Both `d3d12_pipeline_statistics` tests stop at Metal's `executeCommandsInBuffer` assertion
    instead. #47's control (`host_check_root_ranges.25`'s change: the shader's own zero row back) brings the gate's
    addresses back in `d3d12_compute` and `d3d12_cbuffer_layout`. With the same patch it reports none, and both
    pass.
  - **Values.** Without validation all of these tests pass (§ 2a).

  They stay in the known list as validation's, not as stability findings. #39 and #47 moved the zero row out of the
  shader's constants to avoid such a report; the values they read never changed, and both were reverted.
- **`d3d12_indirect_many`** (private ticket 44, at e26baa3) is another report, and still open:
  - **What validation reports.** On Metal 4 under validation, both front ends report 13 invalid loads by `vs_main`,
    during the execution of an indirect command buffer (`Error detected in unknown`). Each is at offset 65536 of a
    16 MiB buffer validation calls `resident:No`. The test then hangs as upstream's #165 describes
    ([TESTING.md](../TESTING.md) § 6).
  - **The buffer.** 16 MiB is the size of the device's first buffer for what the GPU reads of command lists
    (`kFirstArguments`), which `NewArgumentsBuffer` adds to the device's residency set. The Metal 4 queue carries
    that set.
  - **The other runs.** On Metal 3 under validation it passes with no report. On Metal 4 without validation it
    passes.
  - **Not settled.** Whether validation misses the queue's residency set, or the GPU really reads memory the set
    doesn't cover, waits for its own ticket.
- **`MTLMatrixLayoutRowMajor`.** `d3d12_obligations`, `d3d12_recording_failure` and others build an acceleration
  structure descriptor with a matrix layout the M1 doesn't support; Metal's validation ends the process. Mullion sets
  it without a family check. Fixed by #45: the bridge had set it on every triangle geometry, one without a
  transform too, and a size query has none. On the M1 the ray tests' builds on Metal 4 then stop at Metal 4's own
  refusal, `Metal 4 does not support raytracing with software emulation`, a separate defect. On Metal 3 all ten tests
  pass without validation, and under it all but one step of `d3d12_rayquery` (an invalid load, validation's view).

## 2a. The same tests without validation

Because validation's own instrumentation broke 135 answers, the 552 `d3d12_` tests ran once more as `native`, without
`--validate` (`--jobs 1`, 35 min, 2026-11-15): **503 passed, 47 failed, 2 skipped**. That is the answer a player's
M1 gets, and `tests/swtor/known-m1.txt` (the list run.py reads without `--validate`) records it;
`tests/swtor/known-m1-validated.txt` holds § 2's.

| cause | tests |
|---|---|
| upstream's M5 Pro causes: `d3d12_depth_stencil` (144) and `d3d12_raster_bias` (1740) give the M5 Pro's very answers, hash included; `d3d12_block_values` (132 against 108), `d3d12_raster` (1014 against 990) and `d3d12_raster_samples` (56112 against 26544) other counts | 10 |
| ray tracing on Apple7 (`d3d12_raytracing`, `d3d12_ray_generation`, `d3d12_ray_shader_kinds`, `d3d12_rayquery`) | 4 |
| a window from SSH (`d3d12_device_removal`, `d3d12_startup`, `d3d12_trace_log`) | 5 |
| **Apple's M1 compiler backend aborts** on a pipeline of `d3d12_buffer_loads_dxil` (below; fixed by #46) | 1 |
| by design (`d3d12_corpus`) | 2 |
| `d3d12_texture_compression`'s missing verdict | 1 |
| unexplained: `d3d12_clear_values` (R11G11B10_FLOAT clears), `d3d12_frames`, `d3d12_pipeline_times`, `d3d12_profile`, `d3d12_ps_writes`, `d3d12_sample_residency`, `d3d12_adapters`, `d3d12_swapchain_creation`, `d3d12_swapchain_refs`, `d3d12_queued_present`, `d3d12_sampler_slots`, `d3d12_resource_interfaces`, `d3d12_atomic64_fallback_dxil`, `d3d12_mesh_groups_dxil` | 26 |

- **The backend abort.** Without validation `MTLCompilerService` aborted (`llvm::report_fatal_error` in a
  `MachineFunctionPass` of the AGX backend) on one compute pipeline of `d3d12_buffer_loads_dxil` (`cs_6_x` 64-bit
  loads, `MULLION-SHADER … sha1=f6b2fae2518697b30198a235a69407cddfa5b472 step=pipeline`), eight times in a row; the
  pipeline failed and its dispatches were skipped (8232 wrong results). Under validation a mesh pipeline crashed the
  same backend once (SIGSEGV, `AGCModule<MeshProgramKey>`). A shader Mullion makes can kill Apple's compiler on an M1:
  a game's draw with it would be skipped. Not seen on the M5 Max; the shader isn't dumped yet (`dumped=no`).
  Fixed by #46: the pipelines were all 16 of the test's 64-bit shaders (`cs_6_3` and `cs_6_6`), whose
  `globallycoherent` loads zero-extended an atomic 32-bit load, which the backend of Apple7 and Apple8 GPUs can't
  legalize. With the fix the test passes on the M1 and its line is out of `known-m1.txt`. The mesh tests gave this
  baseline's answers again; the mesh crash didn't recur.
- `d3d12_statistics_heap` has no verdict: its slice pushed swap 280 MiB and the queue stopped it. Without validation
  `d3d12_format_blend` passes, and `d3d12_raster` and `d3d12_raster_samples` give the counts above.
- The six range-check tests pass without validation (their invalid loads were validation's view of the shader's own
  zero row, § 2).

## 2b. What validation can't see: an argument-table slot nobody bound

Private ticket 54 (#53), on the rented M1, macOS 27.0 (26A428), 2026-11-17. A finding by ADR 0006's rule, with its
controls; no code changes for it.

- **Validation refuses every kernel that declares a buffer slot nobody bound, whatever the slot holds.** The
  dispatch stops at `-[MTL4DebugComputeCommandEncoder dispatchThreads:threadsPerThreadgroup:]` with one of three
  messages:
  - a new argument table: "Buffer binding at index 3 for slot was never set";
  - a slot `setAddress:0` cleared: "cannot be null";
  - a slot that still holds a visible function table's resource ID: "should have been set with setAddress:atIndex:".
- **A kernel that doesn't declare the slot runs clean**, even with a released function table's ID in it.
- **The control is the run without validation.** The same kernel reads the slot's 64 bits: 0 in a new table, the
  table's ID (`0x1`) after `setResource:atBufferIndex:`, 0 again after `setAddress:0` (a probe outside the
  repository, the PR has the readings).

So validation can't tell a pooled table that still names a released object from one that was cleared:
`host_metal4_parts`, the test of `MULLION_METAL4_REUSE_PARTS`, observes the slot only without validation. Under
validation it checks the bridge's count of the slots `dealloc` clears, a structural check (the ticket's amendment of
2026-11-17).

| break | without validation | under validation |
|---|---|---|
| `tests/breaks/host_metal4_parts.patch`: the function table's slot not counted | fails: slot 3 reads `0x1` in the pooled table | fails: the count is 1 |
| `.4`: `dealloc` doesn't clear the buffer slots | fails: slot 3 reads `0x1` | passes: the count is right, and nothing reads the slot |
| `.2`, `.3`: off takes from, or gives back to, the pool | fails | fails |

## 3. Upstream's baseline

`tests/known.txt` (one M5 Pro, no validation, 23 lines) stays upstream's. The fork's lists are per chip:
`tests/swtor/known-m1.txt` without validation and `tests/swtor/known-m1-validated.txt` with it, read with
`--known`. The same tests that fail on the M5 Pro for Apple's rasterizer, depth
bias, BC4/5 decoder and 24-bit depth fail on the M1 too, with other counts.

## 4. Controls

Each control is a fault its test must notice. Run last, one ticket each, with `gpuEvent-*.ips` collected after each:
none faulted the GPU.

| control | test | answer with the fault | switch |
|---|---|---|---|
| `tests/breaks/d3d12_allocator_reset_in_flight.patch` | `d3d12_allocator_reset_in_flight_dxil` | `failed: 8 wrong results [7c4c750f]` | `MULLION_D3D12_RESET_WAITS` |
| `tests/breaks/d3d12_bounds_guarded.patch` | `d3d12_bounds_guarded_dxil` | `failed: 3 wrong results [7aadde1a]` | `MULLION_D3D12_GUARD_STATIC_RANGES` |
| `tests/breaks/d3d12_bounds_guarded.2.patch` | the same | `failed: 6 wrong results [224f29b8]` | the same |
| `tests/breaks/d3d12_bounds_guarded.3.patch` | the same | `failed: 9 wrong results [f356d968]` | the same |
| `tests/breaks/d3d12_bounds.2.patch` | `d3d12_bounds_dxbc` | `failed: 90 wrong results [8a90497f]` | upstream's bounds |
| a break of the gate's own (below) | `d3d12_texture_compression` | `failed: a target of its group and size: uncompressed 0, want 1` | `MULLION_TEX_MSAA_COMPRESSION=auto` |
| `tests/breaks/d3d12_cbv_ranges.patch` | `d3d12_cbv_ranges_{dxil,dxbc}` | `wrong: a view of 512 bytes over 256: v[0] is 0x1, want 0` and a validation assertion | `MULLION_D3D12_CHECK_RANGES` |
| `tests/breaks/d3d12_root_view_ranges.patch` | `d3d12_root_view_ranges_{dxil,dxbc}` | `failed: 4 wrong results` and `Invalid load … at offset 320` | the same |

- The allocator row is history: since upstream batch 3 (private ticket 51) `d3d12_allocator_reset_in_flight` and its
  break are upstream's, and `MULLION_D3D12_RESET_WAITS` is gone ([ADR 0002](adr/0002-gpu-objects-live-until-the-gpu-is-done.md),
  amendment of 2026-11-17).
- The texture control is the gate's own, not a file of `tests/breaks`: the fork's breaks for
  `MULLION_TEX_MSAA_COMPRESSION` are host tests, and the one that lets `auto` ignore the family gives the M1's own
  answer on Apple7. It leaves the device's field at 0:
  `src/d3d12/d3d12_texture.cpp`, `InfoOut.uncompressed = Device->TextureUncompressed(Desc, InfoOut);` →
  `InfoOut.uncompressed = 0;`.
- `tests/faults.py` can't take a test whose unfaulted answer `--known` can't hold: it records that answer with
  `--record` and reads it back, and the reader takes only `failed: N wrong results [hash]` and `skipped:` lines. The
  texture test (no verdict line at efee691) and the two range tests (passed, then a validation error) are such
  tests, so their controls ran by hand (`gate.zsh control`): the same steps, outputs side by side.
- Not run: `d3d12_bounds.patch` doesn't apply since the fork's #29/#35; 196 of the 1,356 `tests/breaks` patches don't
  apply at efee691 (four of them upstream's batch 2 names: `d3d12_occlusion_far.3`, `d3d12_occlusion_many.4`,
  `d3d12_occlusion_replay.4`, `d3d12_dsv_unknown_format.3`). A patch that doesn't apply is stale, neither caught nor
  missed. The ticket's controls for `d3d12_release_in_flight` and `d3d12_fetch` don't exist: neither test has a break.
  Both tests passed on both paths.

## 5. What this run doesn't show

- **The window suite** (19 tests) and three tests that open a window (`d3d12_device_removal`, `d3d12_startup`,
  `d3d12_trace_log`): no window from SSH, and the GUI session hangs as in § 1.
- **Other families**: the masked Apple9/8 runs, and anything on M3/M4's Apple9 driver: a driver bug can be one
  generation's (the M4's compression bugs, d9mt #105 and #140, were seen on an M4 only). The M1 can't show them; an M4
  tester can.
- **Apple10** (the M5 Max part of the gate), **macOS 26** (the machine has 27.0; players are on 26 and 27.2), and the
  D3D10, D3D11 and DXGI tests, conformance and benchmarks.
- **Metal 3 without validation**, and the switches-on pass without validation: § 2a ran `native` only.
