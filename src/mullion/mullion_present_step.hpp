#pragma once

// What a Direct3D 12 present does with its frame once the presenter has encoded it (d3d12_command_queue.cpp): present
// the drawable, at once or after a minimum duration; send the frame to the window through GDI once its command buffer
// completes, for a window without a layer or a drawable that timed out; or nothing, for a frame MULLION_PRESENT_GATE
// kept from a drawable (docs/swtor/README.md § The present gate). Metal is never asked to present a drawable it wasn't
// given. Pure C++: tests/host/present_step.cpp builds it natively.

namespace mullion {

enum class PresentStep {
  Window,        // drawToWindow once the command buffer completes (it finds no frame for a window with a layer)
  Nothing,       // a gated frame: drawn to the fallback target, nothing presented
  Drawable,      // presentDrawable
  DrawableAfter, // presentDrawableAfterMinimumDuration
};

// `drawable`: the presenter gave one; `gated`: the gate kept the frame from one; `after`: the minimum duration, s
constexpr PresentStep
PresentStepOf(bool drawable, bool gated, double after) {
  if (!drawable)
    return gated ? PresentStep::Nothing : PresentStep::Window;
  return after > 0 ? PresentStep::DrawableAfter : PresentStep::Drawable;
}

} // namespace mullion
