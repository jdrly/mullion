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

## Log lines

| Line | When | Where it lands | Who reads it |
|---|---|---|---|
| `Mullion: session <version> (jdrly/mullion swtor-on-mac) d3d12 fl= metal4= shaderMetal= airconv= codegen=` | With `MULLION_SESSION_HEADER=1`, once per process, when the first Direct3D 12 device is created, before its feature level is checked | `d3d12.log` (Wine's output, `wine.log` in the app), at `info` | The app's session report; me, first in every log |
| `Mullion: mac macOS= build= gpu="" family= masked= ram= workingSet= arch= supportsPlacementSparse=` | With the session line | The same | The same |
| `Mullion: switches <NAME=value …>` or `none` | With the session line | The same | The same |
| `Mullion: env <NAME=value …>` or `none` | With the session line | The same | The same |
| `Mullion: cache shaders= entries= sizeMiB=` | With the session line | The same | The same |
| `mullion: layer mainThreadWaitMs=<N> applied=no` | A change to a layer has waited 1 s, 5 s and 30 s without the main thread applying one: once at each, with either value of `MULLION_LAYER_WAIT`. `<N>`: ms since the main thread last applied a change, or was sent one with none waiting | The process's stderr (`wine.log` in the app), not `MULLION_LOG_PATH`: the bridge's watch thread writes it | Me, on a freeze: the main thread stopped answering |
| `mullion: layer mainThreadWaitMs=<N> applied=yes` | The main thread applies a change after an `applied=no` line | The same, from the main thread | The same |

### The session header

What a session ran with, in five lines written together, at `info`, with `MULLION_SESSION_HEADER=1`. Without it (the
default), or with `MULLION_LOG_LEVEL=warn` or above, nothing is written or read for them.
[`tests/swtor/fixtures/session.log`](../../tests/swtor/fixtures/session.log) is a sample;
`host_session` fails when the formatter stops writing it, and the app's session report tests copy it. The field names
and their order are the contract: a field is added at the end of its line, and a renamed or moved field changes the
fixture in the same pull request.

- `session`: `MULLION_VERSION` (`git describe` of the build); the API; `fl` the device's feature level; `metal4` `on`,
  `off` (`MULLION_D3D12_METAL4=0`) or `unsupported` (no Metal 4 family); `shaderMetal` the Metal version shaders are
  converted for (310, 320, 400); `airconv` `AIRCONV_VERSION`; `codegen` the converter's codegen id, `n/a` until the
  shader cache keys on one (private ticket 18).
- `mac`: `macOS` the version; `build` the macOS build (`kern.osversion`); `gpu` Metal's device name; `family` the
  GPU's own newest Apple family, past `MULLION_GPU_FAMILY`; `masked` the family the bridge answers as when
  `MULLION_GPU_FAMILY` is set, else `no`; `ram` and `workingSet` (the device's recommended maximum) in MiB; `arch` the
  DLL's architecture, `-rosetta` when the process is translated; `supportsPlacementSparse` 1 or 0, the bridge's
  answer (0 below macOS 26.4).
- `switches`: every switch declared in `d3d12.dll`, by name, with its value now: a bool as `on` or `off`, a number as
  its digits.
- `env`: every `MULLION_*`, `DXMT_*` and `MTL_*` variable set (names of letters, digits and `_`), by name.
- `cache`: the shader cache's file below the user's cache directory (`off` with `MULLION_SHADER_CACHE=0`); `entries`
  its rows, `n/a` until private ticket 18 counts them (the cache is a SQLite WAL database, and a connection to one
  rewrites its `-shm`, while reading the file alone races a writer in another process); and the file's size in MiB
  (0 without it), from a `stat`.

`build`, `family`, `masked`, `ram`, `arch`'s `-rosetta` and the cache's size come from the bridge
(`WMTGetSessionFacts`), which only reads: it keeps no state and makes no Metal object. A value it can't read is `n/a`.

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
  below), breaks five checks on purpose and requires each to fail, and fails on a compiler warning that
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

  The fifth break guards `host_layer_apply`, which runs the bridge's layer setters (`src/winemetal/unix/layer.c`) on
  a `CAMetalLayer` without a device while the test's main thread is held for 2 s without its run loop. Its two patches
  in `tests/breaks/` put `dispatch_sync` back on `MULLION_LAYER_WAIT=0`'s path, and compare a change only with the
  layer, not with the change still on its way to the main thread. The test has to fail on each.

  The fourth break, for the build checks, is a copy of the staged files with a file the archive doesn't ship and
  without `SOURCE`, its `winemetal.so` unsigned, with an LLVM source path appended and `minos` 26.0, and
  `d3d12core.dll`'s internal library tagged 2.9 with its first function AIR 2.9. Each of the seven has to fail.
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
  (`airconv <AIRCONV_VERSION>`, the shader cache's version), `toolchain:` (Xcode, macOS SDK, `metal --version`, LLVM,
  llvm-mingw and its clang) and `built:` (the run's URL).
- **`SOURCE`.** The repository and the commit the files are built from, as LGPL 2.1 asks.
- **How the app pins it.** The run's summary prints the pin in the form `src/som.sh` takes: `MULLION_URL`,
  `MULLION_API_URL` (the asset's API address), `MULLION_SIZE`, `MULLION_SHA256` and `MULLION_COMMIT`. The app checks
  size and hash on download; refusing an archive whose `VERSION` names another commit comes with its pin of the fork
  (private ticket 11).
- **Who gets it.** Not players, until the GPU gate has passed on that commit (private ticket 15). Until then a fork
  build reaches only development builds of the app. A pull request's run packs the archive but uploads nothing.
