# The fork fixes upstream's defects itself, and nothing goes upstream

Status: accepted, 2026-11-15.

## Context

The fork's own checks find defects in upstream's code: the GPU gate on a real M1 ([gate.md](../gate.md)), Metal's
validation, the host tests and their breaks. Upstream can't find them the same way: one maintainer merges his own
work without review, and no GPU test runs in its CI ([upstream.md](../upstream.md)).

Two rules stood against fixing them here, and against each other:

- Nothing goes to `dttdrv/mullion`, `3Shain/dxmt` or athei's repositories ([AGENTS.md](../../../AGENTS.md)), so a
  defect can't be reported there and waited for.
- The fork left all of upstream's files as they were, so that batches merge without conflicts. Yet its range checks
  and converter fixes already edited upstream's sources, and #39's review reported that conflict.

Leaving a defect in place means players run it until upstream happens to fix it, and the fork gets that fix only with
a batch I choose.

## Decision

My decision of 2026-11-15, written into `AGENTS.md` and
[upstream.md § Fixing upstream's code](../upstream.md#fixing-upstreams-code) by #47:

- **The fork fixes obvious defects in upstream's code itself**, in its own pull requests, editing upstream's files as
  narrowly as it can.
- **Obvious means the code is plainly wrong:** undefined behaviour, code that contradicts its own comment and
  callers, a read Metal's validation calls invalid. The last one counts only once its control has run
  ([ADR 0006](0006-a-validation-finding-needs-a-control.md)).
- **Such a fix has no switch** ([ADR 0001](0001-switches.md)'s case): its off value would bring the defect back. On a
  high path it still says "Needs the Mac run".
- **It is marked in the code** (`jdrly/mullion:`), so a batch's reader sees whose line it is.
- **Nothing of it goes upstream:** no issue, pull request, comment, discussion or finding offered as research. A
  finding upstream should hear about goes to me, and I decide. No checkout has a remote for upstream, and every
  `gh` command names the fork.
- **A batch that conflicts on a fixed line keeps the fork's fix**, unless the batch fixes the same defect itself.
  Then upstream's fix is taken and the fork's is dropped, and the pull request says which, with the fork's test that
  shows the defect gone ([upstream.md § 4](../upstream.md#4-merge)).
- Upstream's README, CONTRIBUTING and `docs/` stay as upstream has them. The fork's own changes still live in new
  files where they can.

Batches are read before they are merged, and that keeps this affordable: a research lane reads every high-path
commit line by line and notes each one that touches a line the fork changed, so a fork fix under an upstream change
is seen, not merged over.

## Consequences

- Every fix is a line the next batch can conflict on, and the fork carries it until upstream fixes the same defect.
  The batch's file checks ([upstream.md § 5](../upstream.md#5-check-the-merge)) report such a file as `differs`, and
  the pull request shows its resolution.
- Upstream doesn't learn of a defect the fork finds, unless I tell it.
- A fix is only as good as its finding, and it changes code every player runs, with no switch to turn it off. #47,
  the pull request that wrote this rule down, fixed a report that turned out to be Metal's validation and not a
  defect; #49 reverted its code and #39's, and kept the rule. ADR 0006 is the check that came out of it.
- A fix in the converter gives a new codegen id, so every player converts every shader again once
  ([ADR 0003](0003-shader-cache-keyed-on-codegen.md)).

## Alternatives considered

- **Report it upstream and take the fix with a batch.** Nothing goes upstream. A fix would also arrive only with the
  next batch I choose, unreviewed and without a GPU test.
- **Leave upstream's files alone and list the defect as a known failure.** Players keep the defect.
- **Fix it behind a switch that defaults off.** That ships the defect on, and ADR 0001 already says such a fix needs
  no switch.
