# The SWTOR fork's switches and log lines

Every `MULLION_*` switch this fork adds or changes, and every log line it writes. A pull request that adds or changes
a switch or a log line updates this file in the same pull request. Upstream's own variables are in
[DEBUGGING.md](../DEBUGGING.md), [tracing.md](../tracing.md) and [`mullion.conf`](../../mullion.conf).

How a switch is declared: [ADR 0001](adr/0001-switches.md).

## Switches

| Variable | Default | What it does | Off restores |
|---|---|---|---|

## Log lines

| Line | When | Where it lands | Who reads it |
|---|---|---|---|

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
  below), breaks four checks on purpose and requires each to fail, and fails on a compiler warning that
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
