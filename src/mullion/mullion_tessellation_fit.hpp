#pragma once

// jdrly/mullion: which of a render pass's draws the compute tessellator draws (tessellation_scan,
// mullion_command.metal): those before the first whose points or whole triangles end past the pass's arena; that one
// and every one after it take their mesh draws. The scan's sums are 32-bit words: wrapping, a pass of more than 2^32
// points would let a draw's ends land inside the arena again, at a draw's boundary or inside one, and upstream's check
// of each draw's own ends alone would draw it, its indices reaching past the arena (private ticket 58). So the scan
// adds with tessellation_sum, which stops at the word's largest value instead of wrapping: the sums only grow, and the
// first draw whose ends are past the arena is the first that doesn't fit.
//
// Metal and C++ alike (no Windows, no Metal headers): tests/host/tessellation_fit.cpp builds it natively.

#ifndef __METAL__
#include <cstdint>
#endif

namespace mullion {

// a + b, or the largest 32-bit value where that is past it: the scan's sums, which never wrap
inline uint32_t
tessellation_sum(uint32_t a, uint32_t b) {
  uint32_t sum = a + b;
  return sum < a ? 0xffffffffu : sum;
}

// the threads of tessellation_scan's one threadgroup: as many as its pipeline takes (`most`, its
// maxTotalThreadsPerThreadgroup: 704 on an M1), whole SIMD groups of `width` (its threadExecutionWidth), at most the 32
// groups its sums have room for, and at most 1024. upstream dispatched 1024 whatever the pipeline took, which Metal
// forbids (private ticket 58)
inline uint32_t
tessellation_scan_threads(uint32_t most, uint32_t width) {
  if (!width)
    return most < 1024 ? most : 1024;
  uint32_t threads = most / width * width;
  if (threads > 32 * width)
    threads = 32 * width;
  return threads < 1024 ? threads : 1024;
}

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
