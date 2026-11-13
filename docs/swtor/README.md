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
  checks that the seven files the app installs are there and that `winemetal.so` holds no LLVM assertion text, then
  uploads `mullion-swtor-<commit>` (7 days): `x86_64-windows/{d3d10core,d3d11,d3d12,d3d12core,dxgi,winemetal}.dll`,
  `x86_64-unix/winemetal.so` and `VERSION`. For a pull request it also runs the host tests (`meson test 'host_*'`, all
  of `tests/host`), breaks two checks on purpose and requires both to fail, and fails on a compiler warning that
  `.github/swtor/warnings-x86_64.txt` doesn't list. A push run builds and uploads only: the pull request's run of the
  same tree has already passed the rest.

**Runner time.** One run at a time for the whole repository: a run in progress is never cancelled, and GitHub keeps one
more waiting. No schedule, no manual trigger. Toolchain caches are saved only on a miss. On a miss LLVM takes about 25
minutes to build; push nothing else while that runs. When `.github/swtor/toolchain.zsh` builds LLVM differently, raise
`LLVM_BUILD` in the workflow so the old cache isn't used.

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
