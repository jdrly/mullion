# A GPU object lives until the GPU is done with the work that uses it

Status: accepted, 2026-11-13; amended 2026-11-17 (below). Line numbers are at `ed40e54e`.

## Context

Direct3D 12 lets a game release an object while work it submitted still uses it. The game is wrong to do so, and some
games do it anyway; Windows' drivers mostly survive it. On Metal the same release can free memory the GPU still reads.
Metal 4 command buffers don't retain what they use, and shaders reach a descriptor heap by GPU address, so nothing
else keeps that memory alive. The result is the GPU reading the wrong data or faulting, which on a Mac can end the
user's session.

d9mt, my Direct3D 9 driver for SWTOR on Mac, hit exactly this: a released texture view whose ID the GPU still used
drew pink and black textures and froze the GPU. Its ADR 0001 made the rule below. Applied to the present path, it
took GPU faults from 8 in 150 starts to 0 in 150.

Mullion already follows the rule for part of its objects:

- `EndAfterWorkInFlight` (`src/d3d12/d3d12_device.hpp:965-978`) runs what ends a released object only after every
  queue has completed the work it was given before the release, including work still waiting to be encoded
  (`Outlast`, `src/d3d12/d3d12_command_queue.cpp:645-660`). Buffers (`d3d12_buffer.cpp:113`), textures
  (`d3d12_texture.cpp:407`) and heaps (`d3d12_heap.cpp:35`) use it. `tests/d3d12/d3d12_release_in_flight.cpp` tests
  it.
- A command allocator stays alive while its lists are on the GPU (`d3d12_command_queue.cpp:1512-1519`, `1055-1057`). Its
  recording, when it is reset in flight: see the amendment below.
- The bridge keeps its own set of what each Metal 4 command buffer encoded (`src/winemetal/unix/winemetal_unix.c:5520-5524`).

These aren't on that path yet. Each ends what it holds in its destructor, at once:

- descriptor heaps and sampler heaps: the shader-visible Metal buffer (`d3d12_descriptor_heap.cpp:200-207`, `670-676`);
- root signatures: the static samplers' buffer (`d3d12_root_signature.cpp:456-459`);
- query heaps: the results buffer (`d3d12_query_heap.cpp:30-33`);
- pipelines: the Metal pipeline, whose handle sits raw in recorded commands until the queue thread encodes them
  (`d3d12_pipeline_graphics.cpp:1076-1079`).

That they are released at once is read in the code. That it faults a Mac is suspected, not seen: no game is known to
do it, and nobody has run the test that would show it (release a descriptor heap right after `ExecuteCommandLists` of
a long draw).

## Decision

- Nothing that recorded or submitted work refers to is ended before the GPU has completed that work. A release goes
  through `EndAfterWorkInFlight`, or through a path that waits at least as long.
- Every new owner of a Metal object follows the rule from its first commit.
- A finding that a change can release a Metal object the GPU may still use is a `Blocker`
  ([CONTRIBUTING](../../../.github/CONTRIBUTING.md#review)).
- The objects listed above move onto the path in their own pull requests, after the GPU gate has a baseline to run
  them against. Each is a change on a high path that needs the Mac run.

## Consequences

- An object lives until its work completes instead of until the game releases it. Peak memory rises by whatever the
  in-flight frames hold. Nobody has measured how much.
- Until the listed objects move, a game that releases one of them early can still fault the GPU. The rule doesn't
  pretend otherwise.
- Mullion has no check that catches a violation without a GPU fault, like d9mt's `D9MT_LIFETIME_CHECK`. A violation
  shows up only when the GPU reads freed memory, which a hosted runner never shows.

## Alternatives considered

- **Rely on the game releasing correctly.** That is what the listed objects do today. Games don't always release
  correctly, which is why vkd3d-proton and Mullion's own `EndAfterWorkInFlight` exist.
- **Rely on Metal's validation layer.** d9mt found that it doesn't catch this class of bug.
- **Move every listed object now, in this pull request.** A lifetime change needs the GPU gate's runs as evidence,
  which don't exist yet.

## Amendment, 2026-11-17: a command allocator reset in flight hands its recording to the queues

The fork's #17 (private ticket 19) applied this rule to a command allocator `Reset` while lists recorded from it are
still queued or on the GPU, which Direct3D forbids and games do anyway: `Reset` waited until those lists had completed
(`MULLION_D3D12_RESET_WAITS`, on by default since #44). Ticket 19 named the alternative, vkd3d-proton's: retire the
recording after the pending lists instead of blocking.

Upstream batch 3 (private ticket 51) brings that alternative: upstream's `ab46de5`, `3d0372c` and `be22cea`. A `Reset`
with lists in flight takes what they recorded and what their passes use out of the allocator (`Retire`,
`src/d3d12/d3d12_command_allocator.cpp`) and hands it to the device's queues through `OutlastCommandQueues`, the path
`EndAfterWorkInFlight` uses, which keeps it until the work they had been given has completed. The allocator starts
again with nothing of theirs. That satisfies the decision above without a wait.

**Decision.** The fork keeps one mechanism, upstream's hand-off, and removes its wait. The wait can never return when
the list is behind a queue's `Wait` on a fence the resetting thread signals only after the `Reset`: Direct3D lets an
application order its work that way, and upstream's `d3d12_allocator_reset_in_flight` does, in its even rounds. The
hand-off doesn't stall the game's thread either.

**Removed:** the switch `MULLION_D3D12_RESET_WAITS`, the `MULLION-RESET-WAIT` line, the `STATS` section `RESETWAIT`,
the hitch category `allocReset`, the device's `reset_wakes`, `tests/host/reset_wait.cpp` and its three breaks, and the
fork's `d3d12_allocator_reset_in_flight` (it asserted the wait itself), replaced by upstream's test and break of that
name. Upstream's `ERR` line for every such `Reset` is replaced by a line the first time each allocator is reset in
flight, `MULLION-RESET-IN-FLIGHT`, and a `STATS` section `RESETINFLIGHT` ([README](../README.md) § Log lines), so
that a game that resets a frame early doesn't write a line every frame.
