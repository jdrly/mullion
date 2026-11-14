#pragma once

// MULLION_D3D12_RESET_WAITS=1: a command allocator's Reset while lists recorded from it are still queued or on the
// GPU waits for them to complete, or for the device to be removed, before it gives their recording back
// (d3d12_command_allocator.cpp). Direct3D forbids that Reset; games make it anyway, and upstream resets at once, so
// the queue may encode records another list overwrites and the GPU read what is freed (docs/swtor/README.md).
//
// Each allocator counts its lists a queue has taken and not completed (MTLD3D12CommandAllocator::pending). A queue
// lowers the count as each completes (Completed), and when it reaches 0 raises the device's Wakes; so does the
// device when it is removed. A Reset waits on the Wakes, not on its own count, so that a removal reaches it too.
//
// The first wait of each allocator writes, at warn:
//
//   MULLION-RESET-WAIT at=02:14:07.512 type=direct lists=2 frame=4210 allocator=1
//
// and with MULLION_STATS=1 the waits of a second go into a section after STATS (Section):
//
//   RESETWAIT waits=3 ms=4.1 longestMs=2.0
//
// Pure C++ (no Windows, no Metal): tests/host/reset_wait.cpp builds it natively.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace mullion::reset_wait {

// what a waiting Reset wakes on: a count of the device's, raised when one of its allocators' last pending list
// completes and when the device is removed
struct Wakes {
  std::atomic<uint32_t> count{0};

  void
  Raise() {
    count.fetch_add(1, std::memory_order_release);
    count.notify_all();
  }
};

// a queue is done with one list of the allocator whose count is `pending`: completed, or cancelled before it ran
inline void
Completed(std::atomic<uint32_t> &pending, Wakes &wakes) {
  if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
    wakes.Raise();
}

// returns once `pending` is 0 or `removed()` is true. The count of the Wakes is read before each test, so a list that
// completes or a removal that comes after a test has raised it past what the wait waits on
template <class Removed>
void
Wait(const std::atomic<uint32_t> &pending, Wakes &wakes, Removed removed) {
  for (;;) {
    auto seen = wakes.count.load(std::memory_order_acquire);
    if (!pending.load(std::memory_order_acquire) || removed())
      return;
    wakes.count.wait(seen, std::memory_order_acquire);
  }
}

// the local time of a line
struct Time {
  unsigned hour = 0, minute = 0, second = 0, millisecond = 0;
};

struct First {
  Time at;
  const char *type = "direct"; // the allocator's D3D12_COMMAND_LIST_TYPE: direct, bundle, compute, copy
  uint32_t lists = 0;          // its lists not completed at the Reset
  uint64_t frame = 0;          // the process's Presents so far
  uint64_t allocator = 0;      // the allocators of the process that have waited, this one included
};

// the line of an allocator's first wait, written before it waits: a Reset that never returns leaves it last
inline std::string
FirstLine(const First &first) {
  char line[160];
  std::snprintf(
      line, sizeof(line), "MULLION-RESET-WAIT at=%02u:%02u:%02u.%03u type=%s lists=%u frame=%llu allocator=%llu",
      first.at.hour % 100, first.at.minute % 100, first.at.second % 100, first.at.millisecond % 1000, first.type,
      first.lists, (unsigned long long)first.frame, (unsigned long long)first.allocator
  );
  return line;
}

// the waits of the DLL's allocators, with MULLION_STATS on; any thread adds
struct Counters {
  std::atomic<uint64_t> waits{0}, ns{0}, longest_ns{0};

  void
  Add(uint64_t waited_ns) {
    waits.fetch_add(1, std::memory_order_relaxed);
    ns.fetch_add(waited_ns, std::memory_order_relaxed);
    for (auto longest = longest_ns.load(std::memory_order_relaxed);
         waited_ns > longest && !longest_ns.compare_exchange_weak(longest, waited_ns, std::memory_order_relaxed);) {
    }
  }
};

// this DLL's
inline Counters counters;

// RESETWAIT's text for the second that ends: the waits since `last_waits` and `last_ns`, the counts at the second
// before, and the longest of them; "" for a second without one
inline std::string
Section(Counters &from, uint64_t &last_waits, uint64_t &last_ns) {
  auto waits = from.waits.load(std::memory_order_relaxed), ns = from.ns.load(std::memory_order_relaxed);
  auto longest = from.longest_ns.exchange(0, std::memory_order_relaxed);
  auto window = waits - std::exchange(last_waits, waits), window_ns = ns - std::exchange(last_ns, ns);
  if (!window)
    return {};
  char text[96];
  std::snprintf(
      text, sizeof(text), "waits=%llu ms=%.1f longestMs=%.1f", (unsigned long long)window, window_ns / 1e6,
      longest / 1e6
  );
  return text;
}

} // namespace mullion::reset_wait
