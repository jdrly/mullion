// contract: what a Direct3D 12 present does with its frame (src/mullion/mullion_present_step.hpp): a drawable is
// presented, at once or after its minimum duration; no drawable sends the frame to the window through GDI, unless
// MULLION_PRESENT_GATE kept the frame from one, which then presents nothing, with or without a minimum duration:
// Metal is never asked to present a drawable it wasn't given
#include "../../src/mullion/mullion_present_step.hpp"
#include <cstdio>
#include <string>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

int
main() {
  check(PresentStepOf(true, false, 0) == PresentStep::Drawable, "a drawable is presented at once");
  check(PresentStepOf(true, false, 1.0 / 60) == PresentStep::DrawableAfter,
        "a drawable is presented after its minimum duration");
  check(PresentStepOf(false, false, 0) == PresentStep::Window, "no drawable: the frame goes to GDI");
  check(PresentStepOf(false, false, 1.0 / 60) == PresentStep::Window, "no drawable, with a minimum duration: GDI");
  check(PresentStepOf(false, true, 0) == PresentStep::Nothing, "a gated frame presents nothing, after 0");
  check(PresentStepOf(false, true, 1.0 / 60) == PresentStep::Nothing, "a gated frame presents nothing, after > 0");
  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
