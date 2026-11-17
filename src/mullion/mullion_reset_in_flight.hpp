#pragma once

// a command allocator's Reset while lists recorded from it are still queued or on the GPU. Direct3D forbids it; games
// make it anyway. Upstream's Reset hands the lists' recording to the queues, which keep it until their work has
// completed (MTLD3D12CommandAllocatorImpl::Retire, d3d12_command_allocator.cpp), and wrote an ERR line for every such
// Reset, which a game that resets a frame early would write every frame (docs/swtor/README.md).
//
// The first such Reset of each allocator writes, at warn:
//
//   MULLION-RESET-IN-FLIGHT at=02:14:07.512 type=direct lists=2 frame=4210 allocator=1
//
// and with MULLION_STATS=1 those of a second go into a section after STATS (Section):
//
//   RESETINFLIGHT resets=3 lists=5
//
// Pure C++ (no Windows, no Metal): tests/host/reset_in_flight.cpp builds it natively.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace mullion::reset_in_flight {

// the local time of a line
struct Time {
  unsigned hour = 0, minute = 0, second = 0, millisecond = 0;
};

struct First {
  Time at;
  const char *type = "direct"; // the allocator's D3D12_COMMAND_LIST_TYPE: direct, bundle, compute, copy
  uint32_t lists = 0;          // its lists not completed at the Reset
  uint64_t frame = 0;          // the process's Presents so far
  uint64_t allocator = 0;      // the allocators of the process that have been reset in flight, this one included
};

// the line of an allocator's first Reset in flight
inline std::string
FirstLine(const First &first) {
  char line[160];
  std::snprintf(
      line, sizeof(line), "MULLION-RESET-IN-FLIGHT at=%02u:%02u:%02u.%03u type=%s lists=%u frame=%llu allocator=%llu",
      first.at.hour % 100, first.at.minute % 100, first.at.second % 100, first.at.millisecond % 1000, first.type,
      first.lists, (unsigned long long)first.frame, (unsigned long long)first.allocator
  );
  return line;
}

// the Resets in flight of the DLL's allocators, with MULLION_STATS on; any thread adds
struct Counters {
  std::atomic<uint64_t> resets{0}, lists{0};

  void
  Add(uint32_t pending) {
    resets.fetch_add(1, std::memory_order_relaxed);
    lists.fetch_add(pending, std::memory_order_relaxed);
  }
};

// this DLL's
inline Counters counters;

// RESETINFLIGHT's text for the second that ends: the Resets in flight since `last_resets` and the lists they handed
// to the queues since `last_lists`; "" for a second without one
inline std::string
Section(Counters &from, uint64_t &last_resets, uint64_t &last_lists) {
  auto resets = from.resets.load(std::memory_order_relaxed), lists = from.lists.load(std::memory_order_relaxed);
  auto window = resets - std::exchange(last_resets, resets), window_lists = lists - std::exchange(last_lists, lists);
  if (!window)
    return {};
  char text[64];
  std::snprintf(
      text, sizeof(text), "resets=%llu lists=%llu", (unsigned long long)window, (unsigned long long)window_lists
  );
  return text;
}

} // namespace mullion::reset_in_flight
