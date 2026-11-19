// jdrly/mullion: contract: the compute tessellator draws a pass's draws only before the first whose points or whole
// triangles end past the arena (src/mullion/mullion_tessellation_fit.hpp, private ticket 58), so that every draw it
// draws ends inside the arena in exact arithmetic, also when the scan's 32-bit sums wrap past 2^32 points and a later
// draw's ends land inside the arena again.
#include "../../src/mullion/mullion_tessellation_fit.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

struct Draw {
  uint32_t first, patches;
};

// a pass as the scan leaves it: each patch's frames, points and triangles made exclusive 32-bit sums, the totals after
// them (tessellation_scan); `exact` the same sums in 64 bits
struct Pass {
  std::vector<uint32_t> counts;
  std::vector<uint64_t> exact;
  std::vector<Draw> draws;

  // a draw of `patches` patches of `points` points and `triangles` triangles each
  void
  add(uint32_t patches, uint32_t points, uint32_t triangles) {
    draws.push_back({uint32_t(counts.size() / 3), patches});
    for (uint32_t i = 0; i < patches; i++)
      for (uint32_t c : {1u, points, triangles})
        counts.push_back(c), exact.push_back(c);
  }
  void
  scan() {
    uint32_t sum[3] = {};
    uint64_t sum64[3] = {};
    for (size_t i = 0; i < counts.size(); i++) {
      auto own = counts[i];
      counts[i] = sum[i % 3], exact[i] = sum64[i % 3];
      sum[i % 3] += own, sum64[i % 3] += own;
    }
    for (int k = 0; k < 3; k++)
      counts.push_back(sum[k]), exact.push_back(sum64[k]);
  }
};

// every draw before `unfit` ends inside the room exactly, and `unfit`, when there is one, doesn't
static void
admits(const Pass &pass, uint32_t unfit, uint32_t point_room, uint32_t index_room, const std::string &what) {
  for (uint32_t d = 0; d < pass.draws.size(); d++) {
    uint64_t end = 3 * uint64_t(pass.draws[d].first + pass.draws[d].patches);
    bool fits = pass.exact[end + 1] <= point_room && pass.exact[end + 2] <= index_room / 3;
    if (d < unfit)
      check(fits, what + ": draw " + std::to_string(d) + " is drawn, and ends past the arena");
    else if (d == unfit)
      check(!fits, what + ": draw " + std::to_string(d) + " fits, and takes its mesh draw");
  }
}

int
main() {
  const uint32_t point_room = 1u << 20, index_room = 3u << 20;

  // everything fits
  {
    Pass pass;
    pass.add(10, 100, 150), pass.add(20, 100, 150);
    pass.scan();
    auto unfit = tessellation_first_unfit(pass.counts.data(), pass.draws.data(), pass.draws.size(), point_room,
                                          index_room);
    check(unfit == 2, "two draws that fit are both drawn");
    admits(pass, unfit, point_room, index_room, "fits");
  }
  // the second draw doesn't fit, the third would on its own
  {
    Pass pass;
    pass.add(10, 100, 150), pass.add(1 << 14, 100, 150), pass.add(1, 1, 1);
    pass.scan();
    auto unfit = tessellation_first_unfit(pass.counts.data(), pass.draws.data(), pass.draws.size(), point_room,
                                          index_room);
    check(unfit == 1, "a draw past the arena and every one after it take their mesh draws");
    admits(pass, unfit, point_room, index_room, "past the arena");
  }
  // the sums wrap: the second draw's points run past 2^32, so the third's ends land inside the arena again
  {
    Pass pass;
    pass.add(4, 1000, 1500);
    pass.add(1 << 20, 4096, 1); // 2^32 points: the sum wraps to where it began
    pass.add(4, 1000, 1500);
    pass.scan();
    uint64_t end = 3 * uint64_t(pass.draws[2].first + pass.draws[2].patches);
    check(pass.counts[end + 1] <= point_room && pass.exact[end + 1] > point_room,
          "the third draw's wrapped points end inside the arena (the case)");
    auto unfit = tessellation_first_unfit(pass.counts.data(), pass.draws.data(), pass.draws.size(), point_room,
                                          index_room);
    check(unfit == 1, "after a wrap of the sums, no draw after the one past the arena is drawn");
    admits(pass, unfit, point_room, index_room, "wrapped");
  }

  if (failures)
    return 1;
  std::printf("passed\n");
  return 0;
}
