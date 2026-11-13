# jdrly/mullion agent guidance

This is my fork of Mullion for the DirectX 12 client of SWTOR on Mac (`jdrly/swtor-on-mac`), the app that runs Star
Wars: The Old Republic on Apple Silicon through Wine. AI agents write its changes, and they stay here.

- `swtor-on-mac` is the default branch and the base of every change. Work branches off it and comes back through a
  pull request.
- `main` mirrors upstream's `main` and receives nothing else. Only GitHub's fork sync moves it
  (`gh repo sync jdrly/mullion --branch main`).
- **Nothing goes to `dttdrv/mullion`, `3Shain/dxmt` or athei's repositories.** No issue, no pull request, no comment,
  no discussion, not even a finding offered as research. A finding upstream should hear about goes to me, and I
  decide. No checkout has a remote that can push there: `gh repo clone` adds `upstream`, so remove it.
- Every `gh` command names the fork: `--repo jdrly/mullion`, and `--base swtor-on-mac` for `gh pr create`. Without
  them `gh` in a fork picks the upstream repository.
- Mark every AI-written commit as such in its body.

## Which rules apply

Upstream's [`CONTRIBUTING.md`](CONTRIBUTING.md) governs contributions to upstream, not to this fork; ignore where it
says to report or where a change starts. Its [`docs/CODE.md`](docs/CODE.md), [`docs/TESTING.md`](docs/TESTING.md)
and [`docs/DEBUGGING.md`](docs/DEBUGGING.md) still describe how the code and its tests are written. I leave all of
upstream's files as they are, so that upstream's changes merge without conflicts. The fork's own files are new. One
exception: upstream's workflows `ci.yml` and `game-reports.yml` are deleted here, so they can never run in the fork
([docs/swtor/README.md](docs/swtor/README.md) § CI).

The fork's rules:

1. [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md): branches, titles, pull requests, review severities and the
   Definition of Done.
2. [`docs/swtor/README.md`](docs/swtor/README.md): every switch the fork adds and every log line it writes.
3. [`docs/swtor/adr/`](docs/swtor/adr/): the decisions behind them.

If two of these conflict, report the conflict instead of choosing one.

## High paths

A change here gets a hard review and usually can't be shown on GitHub's runner, which has no real Apple GPU:

- `src/airconv/`: the shader converter. A change can alter generated code that players' caches already hold.
- `src/winemetal/`: the bridge between Wine and Metal. Its PE and unix sides ship from one build.
- Lifetimes, and anything that hands the GPU an address or a length: `src/d3d12/d3d12_command_*`,
  `d3d12_descriptor_heap.*`, `d3d12_texture.*`, `d3d12_buffer.*`, `d3d12_heap.*`.
- The shader cache: `src/mullion/mullion_shader_cache.*` and the cache key in `src/d3d12/d3d12_pipeline_*.cpp`.
- `.github/` and the root `meson.build`.

## Never

- Post anything to `dttdrv/mullion`, `3Shain/dxmt` or athei's repositories.
- Push to `swtor-on-mac` or `main` directly.
- Push a `v*` tag. That numbering is upstream's.
- Start SWTOR, its launcher or the app, run Wine on my install, or fault the GPU on my Mac on purpose.
- Commit a player's log, a dumped game shader or anything that names a player or an account.
