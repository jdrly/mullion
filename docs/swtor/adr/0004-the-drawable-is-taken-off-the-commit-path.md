# A Direct3D 12 present takes its drawable off the queue's commit path

Status: accepted, 2026-11-15. Every timing is the rented M1 on macOS 27.0 (26A428); the pull requests named hold the
runs.

## Context

On 2026-11-14 SWTOR's DX12 client crashed at every switch to another Space. The chain, as far as it was measured:

- **The block.** At a switch away from a window's Space, `-[CAMetalLayer nextDrawable]` blocks about 1.15 s and then
  returns a drawable. A native program without Wine shows it at 9 of 10 switches. Switching back doesn't block,
  covering the window on the same Space doesn't, and on this Mac a window that stays on the other Space doesn't.
- **Where it landed.** Mullion took the drawable on the thread that encodes and commits the queue's command buffers.
  A `Signal` after a `Present` retires only when that command buffer completes, so every fence behind the present
  came about 1.2 s late: 1193–1273 ms at 8 of 8 switches, against 112 ms at most without a switch.
- **What the game does with it.** The client waits on those fences, takes the late one for a hung GPU, reports
  `DXGI_ERROR_DEVICE_HUNG` and ends in a crash. The client's time limit wasn't measured, and the crashes were on an
  M5 Max on macOS 27.2, where no stall was timed. So either the limit is under about 1.2 s, or the stall is longer
  on that Mac. Both are open.

The log said it only as a `HITCH` with a long `encodeMs` and no GPU time. #37 added the line that names it,
`mullion: nextDrawable blocked <ms> ms …`, at 50 ms or more, and `drawableMs` in the `HITCH` line
([README § The nextDrawable line](../README.md#the-nextdrawable-line)). It changes no behaviour, so it has no switch
(ADR 0001), and it is how every run below was read.

The first fix was a visibility gate, as d9mt has: take no drawable while AppKit last said the window is hidden
(private ticket 36, #38, `MULLION_PRESENT_GATE`). In the native program it removed the block at 6 of 6 switches,
where AppKit's notification came 22–44 ms before the call that would have blocked. Under Wine it removed nothing:

| arm, 3 switches each (#38, 65e32a0) | `nextDrawable blocked` | fence waits over 300 ms |
|---|---|---|
| gate on | 1193, 1198, 1194 ms | 1195, 1198, 1195 ms |
| gate off (the control) | 1196, 1195, 1193 ms | 1198, 1198, 1195 ms |

- At all 6 switches measured under Wine (#37's run and #38's), the call that blocks began 9–10 ms **before**
  AppKit's occlusion notification. The gate closed 1–2 ms after the blocked call had returned.
- With the gate closed nothing paced the presents: the repro presented 5,000 to 5,400 frames a second while hidden,
  against 120 while shown.

So no decision made before the call can avoid the block. #38 was closed unmerged.

## Decision

- **A blocked drawable may delay a picture, never a fence.** A Direct3D 12 present's drawable is taken on a thread of
  the presenter's own. The queue's command buffer for the `Present` draws the frame into a texture of the
  presenter's and commits without a drawable; once it completes, the presenter's thread takes the drawable, copies
  the frame into it and presents it (#40, private ticket 38).
  [README § The presenter's thread](../README.md#the-presenters-thread) has the mechanics.
- **`MULLION_PRESENT_THREAD` is on by default.** CONTRIBUTING's Definition of Done ("merges with its new behaviour
  off") and the GPU gate's rule for defaults ([gate.md](../gate.md)) didn't allow that as they stood. Both now name
  this exception, and this is its scope:
  - the off value is the measured defect itself, the ~1.2 s stall of every fence, which the client turns into a
    crash at every Space switch. ADR 0001 gives that reason for a fix without a switch; here it decides a default;
  - the A/B ran on a real Mac before the merge, and its control failed: with `0` the stall came back (below);
  - the switch stays, for that A/B and as a way out.

  A behaviour change whose off value is merely the old behaviour still merges off.
- **The game's `Present` paces itself on the presenter's thread.** It waits on the game's own thread while two frames
  are drawn and neither shown nor dropped, for 100 ms at most. The commit thread's `nextDrawable` had been the only
  thing pacing a swap chain without a frame-latency waitable object: the first run without this wait ran at about
  4,800 frames a second, nearly all dropped.
- **The thread is never waited for.** It is detached and shares its state with the presenter. A presenter's release
  only asks it to stop: that release can come on the queue's completion thread, ahead of the fences it retires,
  while the thread waits for a drawable.
- **Frames are shown in order, newest kept.** What the thread can't show in time is dropped and counted in
  `Mullion: present dropped=…`, at most once a second.
- **Direct3D 11 presents as before.**

The run that decided it (#40, 78b5540, 3 switches away per arm):

| arm | longest fence wait | `nextDrawable blocked` | dropped |
|---|---|---|---|
| thread on, Metal 3 | 2 ms | 1202, 1204, 1192 ms, on `mullion-present` | 11 per switch |
| thread on, Metal 4 | 8 ms | 1195, 1191, 1195 ms | 11 per switch |
| thread off (the control) | 1199, 1205, 1203 ms | 1198, 1204, 1200 ms | none |
| thread on, no switch | 7 ms | none | 1 at start |

The frame rate's median was 120 with the thread on and off.

## Consequences

- Every present costs one more full-frame copy on the GPU, a command buffer of the presenter's own, and a texture
  per frame in flight (3 in a steady stream, 16 at most).
- A frame is shown later by the handoff: the queue buffer's completion, the thread's wake-up and its copy. Only the
  frame rate was measured, not the latency to the display.
- During a block the game's `Present` waits about 100 ms at a time, so the game runs at about 10 frames a second
  for that second, and a `Signal` it submits after a `Present` on the same thread is submitted up to 100 ms later.
  A fence already submitted never waits for the drawable.
- At a Space switch about 11 frames are dropped and the newest is shown about 1.2 s late.
- The block is still there: `mullion: nextDrawable blocked` lines still come, now from `mullion-present`. They no
  longer mean a late fence.
- The detached thread doesn't keep `d3d12core.dll` loaded. A `FreeLibrary` after every object is released could
  unload code the thread still runs, as with the existing detached shader-compilation threads. Open, `Low`.
- Not measured when #40 merged: the game itself, an M5 Max, macOS 26 and 27.2, and whether a newer Mac blocks every
  frame of a hidden window, as d9mt saw, where the M1 blocks only at the switch.

## Alternatives considered

- **A visibility gate** (#38). Measured and rejected, above. It also needs pacing of its own.
- **The same split with the drawable still on the commit thread.** The second command buffer's `nextDrawable` would
  block the thread before the `Signal` the game makes after its `Present`.
- **A presenter thread that renders from the game's back buffer.** While the drawable blocks, the game renders into
  that buffer again, from another queue with no fence between the two. Avoiding the race means holding the buffer,
  which blocks the game's fences again, or copying it, which is this decision.
- **A shorter drawable timeout.** `allowsNextDrawableTimeout` has 1 s or none.
- **Returning `DXGI_STATUS_OCCLUDED`.** It would change what the game does, and the block comes before anything says
  the window is hidden.
