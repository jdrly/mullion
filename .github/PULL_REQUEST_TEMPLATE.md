<!--
If the runner can't exercise this change (anything that draws, the bridge, lifetimes, generated shader code), the
first line of this description is "Needs the Mac run", and the exact commands go in that section below. Claim none of
their results. The change merges with its new behaviour off.
-->

## What changes

<!-- Two to five sentences: what this PR does and why, not a list of files. The issue (Closes #) or the private ticket it implements. Say that an AI agent wrote it, if one did. -->

## Switches and log lines

<!-- Every MULLION_* switch this adds or changes (name, default, what off restores) and every log line it adds or changes. They are also in docs/swtor/README.md. A change to src/airconv says whether generated shader code changes. Write "none" if there are none. -->

## Evidence

<!-- What ran, where (the runner, or a Mac: chip, macOS) and what it showed. Each test given as evidence, new or existing: the break that makes it fail (a tests/breaks patch or a reverted line) and its failing output, then the pass. Skipped tests and checks that didn't run are listed apart; none of them counts as passed. -->

## How to verify

<!-- Steps for me: what to run, what to look at, which log line shows it. -->

## Needs the Mac run

<!-- The exact commands, and what each should show. Write "nothing" if the runner shows all of it. -->

## Definition of Done

- [ ] The `SWTOR` workflow is green on the head commit. A PR that changes only `*.md` files and `docs/` gets no run; for it this item doesn't apply.
- [ ] A behaviour change has a `MULLION_*` switch whose off value restores the base behaviour, and a log line; both are in `docs/swtor/README.md`.
- [ ] Each test given as evidence, new or existing, was seen failing with a named break (a `tests/breaks` patch or a reverted line) before it passed. No skipped test is counted as passed.
- [ ] A high-path change the runner can't exercise says "Needs the Mac run" at the top with the exact commands, claims none of them, and merges with its new behaviour off.
- [ ] A change to `src/airconv` says whether generated shader code changes.
- [ ] The independent review says `APPROVE` on the head commit with no open `Blocker` or `High`.

## Review

<!-- An independent agent that didn't write this PR reviews it and its verdict goes here. Squash-merge into swtor-on-mac (subject = PR title) after APPROVE. -->

- **Verdict:** pending

## Risks and what I didn't do

<!-- What could break and who would notice, known limits, deferred work, what the switch doesn't cover. An empty field is an answer too: write "none". -->
