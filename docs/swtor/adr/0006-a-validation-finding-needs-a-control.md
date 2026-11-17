# A report from Metal's validation is a defect only after its control

Status: accepted, 2026-11-16. Measured on the rented M1 (Apple7, macOS 27.0); [gate.md § 2](../gate.md) has the
record.

## Context

The GPU gate runs the fork's tests under Metal's validation, and
[ADR 0005](0005-the-fork-fixes-upstreams-defects-itself.md) called "a read Metal's validation calls invalid" an
obvious defect, fixed with no switch. Two pull requests were built on such reports:

- #39 (private ticket 37): under `MULLION_D3D12_CHECK_RANGES`, a constant buffer's row past its bound read the
  shader's own row of zeros (`@mullion.zero_row`), and validation called the load invalid.
- #47 (private ticket 43): the same report on upstream's default path, in `d3d12_compute` and `d3d12_cbuffer_layout`.

Both moved the row out of the shader's constants into a resident buffer, and both changed generated shader code.
#47's control gave the shader its own row back, and the report came back at the same addresses. Every test read the
right values before and after.

Private ticket 44 (#48) then looked at the reports that were left, and found the cause of all of them:

- **Metal's shader validation reports, and zeroes, every load of a pipeline's own program-scope constants when the
  pipeline supports indirect command buffers.** Mullion's Direct3D 12 compute pipelines and its vertex and fragment
  render pipelines are made with that support, for `ExecuteIndirect`.
- **Apple's own compiler shows it.** A Metal kernel that reads a constant array, outside Wine and Mullion, gets the
  same `Invalid load in 'device' or 'constant' address space` and reads zeros in a pipeline made with
  `supportIndirectCommandBuffers`. Without that support, or without validation, it reads the right values and gets
  no report.
- **Inside Mullion** the same: with Direct3D 12's pipelines made without that support, the tests report no invalid
  load on either Metal version.
- **The values were never wrong.** Without validation every one of these tests passes.

Upstream's [TESTING.md § 6.1](../../TESTING.md#6-running-tests-and-reading-results) had said so of macOS 27.2 since
2026-11-05, in the tree both pull requests merged onto. Neither cites it.

So #39 and #47 moved a row nothing misread. #49 reverted the code of both (my decision of 2026-11-16). #47's control
had shown that the row was what validation reported, not that reading it was wrong.

## Decision

- **A report from Metal's validation is a finding. It becomes a defect when a control separates it from what
  validation reports of valid code**, and the pull request names what is wrong. Until then nothing is fixed on its
  account, and ADR 0005's "a read Metal's validation calls invalid" doesn't apply.
- **A control changes one thing and leaves the read under suspicion as it is.** For an invalid load there are two,
  and a result means something else in each:

  | control | result | what it says |
  |---|---|---|
  | Apple's shader: one built by Apple's compiler does the same read in the same kind of pipeline, outside Mullion | the same report | validation's: it reports a valid read |
  | Apple's shader | no report, while Mullion's shader gets one | something Mullion does brings the report. Change Apple's shader toward Mullion's form one step at a time, as #48 did, until a step brings it: that step is the cause, and a defect once the pull request says why it is wrong |
  | No support: the same test with Direct3D 12's pipelines made without indirect command buffer support | the report goes and the values are right | validation's: the artefact above |
  | No support | the report stays | not that artefact, and nothing more. Unresolved until another control separates the causes |

  The second control is a diagnostic patch that sets `info.support_indirect_command_buffers` to false in
  `d3d12_pipeline_compute.cpp` and `d3d12_pipeline_graphics.cpp`. It never merges, because `ExecuteIndirect` needs
  the support.
- **A result that doesn't separate the causes leaves the report unresolved**, neither a defect nor an artefact.
- **The run without validation comes first.** A test that reads the right values without validation and fails only
  on validation's report is a candidate artefact, not a candidate defect.
- **A pull request that fixes a validation report names its control and shows its output**, next to the test and
  break CONTRIBUTING already asks for. A fix that makes the report go away is not that control.
- **An artefact is recorded, not worked around:** its cause in [gate.md](../gate.md), its tests in
  `tests/swtor/known-m1-validated.txt` as validation's. No code changes for it.
- **A report without a control yet has no verdict either.** It stays open under its own ticket.

The same class, where the control is another run:

- Apple's shader validation crashes compiling a pipeline, in its own instrumentation. 135 failures of the first gate
  run were that; without validation the same pipelines build ([gate.md § 2a](../gate.md)).
- Tests that use `ExecuteIndirect` can hang under validation on Metal 4: upstream's issue 165
  ([TESTING.md § 6.1](../../TESTING.md#6-running-tests-and-reading-results)). Metal 3 under validation is the
  control.

`d3d12_indirect_many` is the rule applied to an open report: on Metal 4, validation names a 16 MiB buffer
`resident:No` before that hang. A report that names a buffer and an offset is not the constants' artefact, and it
has no control yet, so it has no verdict (private ticket 45).

## Consequences

- A validation finding costs one more Mac run before anything is fixed.
- The control for invalid loads lives outside the repository, with the gate's scripts: the diagnostic patch and
  Apple's kernel. Whoever needs it writes the patch again from the two lines above.
- That control gives no verdict on a test that uses `ExecuteIndirect`: without the support it stops at Metal's
  `executeCommandsInBuffer` assertion. It shows no report, not a pass.
- A real invalid load among a pipeline's own constants hides behind the artefact as long as the pipeline supports
  indirect command buffers. `MTL_SHADER_VALIDATION_GLOBAL_MEMORY=0` is not a control: it turns off the checks of
  memory loads altogether.
- #39, #47 and #49 each changed generated code, so a player whose builds carried them converted every shader again
  each time ([ADR 0003](0003-shader-cache-keyed-on-codegen.md)). That is the cost this rule avoids.
- The known list for a validated run stays long. A line there is read by its cause, and "validation's" is one.

## Alternatives considered

- **Treat every validation report as a defect** (ADR 0005 as first written). It produced #39 and #47.
- **Stop running the gate under validation.** It finds real defects: #45's unsupported matrix layout was Metal's
  validation ending the process.
- **Make pipelines without indirect command buffer support, to keep the runs clean.** `ExecuteIndirect` needs it.
