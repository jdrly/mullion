# Contributing to jdrly/mullion (the SWTOR on Mac fork)

This fork changes Mullion for one program: the DirectX 12 client of Star Wars: The Old Republic, run by SWTOR on Mac
on Apple Silicon. These are its rules. Upstream's [CONTRIBUTING.md](../CONTRIBUTING.md) governs contributions to
upstream, which receive nothing from here (see [AGENTS.md](../AGENTS.md)); its [CODE.md](../docs/CODE.md) and
[TESTING.md](../docs/TESTING.md) still say how the code and tests are written.

The fork's code is LGPL-2.1-or-later like Mullion's. By contributing you agree that your change is licensed the same
way.

## Sources of truth

1. [AGENTS.md](../AGENTS.md): what the fork is, its high paths, and what never goes upstream.
2. This file.
3. [docs/swtor/README.md](../docs/swtor/README.md): every `MULLION_*` switch the fork adds and every log line it
   writes.
4. [docs/swtor/adr/](../docs/swtor/adr/): the decisions.
5. The code.

If two sources conflict, report the conflict instead of choosing one.

## Issues

- Every change that isn't trivial starts from an issue, a **Task** or a **Bug report**, or from a private ticket of
  mine that the pull request names.
- Titles use `Context: Actionable result`, for example
  `Device removal: Say which command buffer failed and why`. The issue, the pull request and the squash commit carry
  the same title.
- *Affected areas* lists only paths you have verified in the repository, at a named commit.
- *Verification (UAT)* is what someone does on a Mac, step by step, and the result each step expects.

## Branches

- Never push to `swtor-on-mac` or `main`. Every change reaches `swtor-on-mac` through a pull request. `main` only
  mirrors upstream.
- Name the branch `<type>/<issue>-<short-description>` in `lower-case-kebab-case`, for example
  `fix/12-descriptor-heap-lifetime`. Types: `feat`, `fix`, `diag` (logs), `chore`, `docs`, `port` (from d9mt or
  `jdrly/dxmt`). An upstream batch is `chore/sync-upstream-<short sha>`.
- One branch is one issue.

## Commits

- English, imperative. The subject follows the title form, `Context: what the commit does`.
- The body says why and with what evidence: what failed before, what ran, what didn't. It says that an AI agent wrote
  the change when one did.

## Pull requests

- The title is identical to the issue's.
- Fill in the [template](PULL_REQUEST_TEMPLATE.md).
- Open it as a **draft**, and keep it one until it meets the Definition of Done.
- One change per pull request. A fix doesn't carry unrelated cleanup, and independent changes are separate pull
  requests.
- Every claim says where it comes from: read in the code, run on the runner, or run on a Mac. A timing names the chip,
  the macOS version and the conditions. Nothing claims a measurement nobody took.
- It is squash-merged with its title as the commit subject. An upstream batch is the exception: it merges with a
  merge commit, so upstream's commits stay ancestors and the next batch merges cleanly. How a batch is chosen, read,
  merged and checked: [docs/swtor/upstream.md](../docs/swtor/upstream.md).

## Review

Every pull request gets an independent agent review by an agent that didn't write it. The review classifies each
finding:

| Severity | Meaning |
|---|---|
| `Blocker` | The pull request must not merge until it is resolved. |
| `High` | Wrong behaviour, or a broken project rule. |
| `Medium` | Duplication, more code than the job needs, or a maintenance risk. |
| `Low` | Local cleanup that can go with related work. |

A finding is a `Blocker` by default when the change can:

- make the GPU read or write outside a resource;
- release a Metal object the GPU may still use;
- stall the queue thread;
- change behaviour with no log line and no switch;
- write anything into a log that names the player, or an absolute host path.

The review ends with `APPROVE` or `CHANGES REQUESTED`. A pull request merges with `APPROVE` on its head commit and no
open `Blocker` or `High`. Code added after an approval is reviewed again. `Medium` and `Low` findings may merge with a
follow-up.

## Definition of Done

- [ ] **Build.** The `SWTOR` workflow (`.github/workflows/swtor.yml`) is green on the head commit. A pull request that
      changes only `*.md` files and `docs/` gets no run; for it this item doesn't apply.
- [ ] **Switched and logged.** A behaviour change has a `MULLION_*` switch whose off value restores the base
      behaviour, and a log line when it acts (once, or on each change of state, never per draw). Both are in
      [docs/swtor/README.md](../docs/swtor/README.md), updated in the same pull request. A fix that only removes
      undefined behaviour, or makes code do what its own comment and callers say, needs no switch
      ([ADR 0001](../docs/swtor/adr/0001-switches.md)).
- [ ] **A test is evidence only after its control failed.** For every test it gives as evidence, new or existing, the
      pull request names the break, a patch in `tests/breaks/` or a reverted line, and shows the test failing with
      it. A test only ever seen passing proves
      nothing. A skipped test never counts as passed.
- [ ] **Needs the Mac run.** A change on a high path that the runner can't exercise says "Needs the Mac run" at the
      top of the pull request, with the exact commands to run. It claims none of their results, and it merges with
      its new behaviour off. A fix that needs no switch (above) has no off: it merges on, and still says "Needs the
      Mac run".
- [ ] **Generated code.** A change to `src/airconv` says whether the shader code it generates changes. Any change
      to the converter's inputs gives a new codegen id, so players convert every shader again once, and the next
      build's notes say so ([caches.md](../docs/swtor/caches.md)).
- [ ] **Reviewed.** The independent review says `APPROVE` on the head commit with no open `Blocker` or `High`, and its
      verdict is in the pull request.

## Never

- Open, comment on or offer anything in `dttdrv/mullion`, `3Shain/dxmt` or athei's repositories. Nothing from this
  fork goes there.
- Push to `swtor-on-mac`, or to `main` anything but GitHub's fork sync.
- Merge a change that renders differently without its issue asking for it.
- Commit logs, captures, dumped game shaders or traces that name a player or an account.
