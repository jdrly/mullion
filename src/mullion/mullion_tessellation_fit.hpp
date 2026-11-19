#pragma once

// jdrly/mullion: which of a render pass's draws the compute tessellator draws (tessellation_scan,
// mullion_command.metal): those before the first whose points or whole triangles end past the pass's arena; that one
// and every one after it take their mesh draws. The scan's sums are 32-bit, so a pass of more than 2^32 points wraps
// them: a later draw's ends can then land inside the arena again while its points don't, and upstream's check of each
// draw's own ends alone would draw it, its indices reaching past the arena (private ticket 58). Before the wrap the
// ends only grow, and the arena holds at most 2^32 - 1 points, so the first draw that doesn't fit comes before any
// wrap, and every draw it admits ends inside the arena.
//
// Metal and C++ alike (no Windows, no Metal headers): tests/host/tessellation_fit.cpp builds it natively.

#ifndef __METAL__
#include <cstdint>
#endif

namespace mullion {

// the first of `draw_count` draws (each with `first` and `patches`) whose points (counts[3 * patch + 1]) or triangles
// (counts[3 * patch + 2]), the scan's exclusive sums at its patch after its last, end past `point_room` points or
// `index_room` indices; `draw_count` when every one fits
template <typename Counts, typename Draws>
uint32_t
tessellation_first_unfit(Counts counts, Draws draws, uint32_t draw_count, uint32_t point_room, uint32_t index_room) {
  for (uint32_t d = 0; d < draw_count; d++) {
    uint32_t end = 3 * (draws[d].first + draws[d].patches);
    // triangles against the room's whole ones: three times a pass's triangles may not fit in 32 bits
    if (counts[end + 1] > point_room || counts[end + 2] > index_room / 3)
      return d;
  }
  return draw_count;
}

} // namespace mullion
