#pragma once

// jdrly/mullion: the pipelines made at their first draw because the program's pipeline archive was recorded to hold
// them (upstream's 055ab02: MTLD3D12Device::Defer queues no compile for them, and DeferredPipeline::Wait makes them on
// the thread that records the draw). Each is a lookup in the archive, or, when the archive doesn't have it after all
// (an unreadable shard, an add Metal refused), a full compile there. With MULLION_STATS=1 those of a second go into a
// section after STATS (Section, docs/swtor/README.md):
//
//   ARCHIVEDRAW made=12 missed=1 ms=48.3
//
// `made` the pipelines made at a draw, `missed` those of them the archive didn't have (Metal compiled them), `ms` the
// time the draws waited for all of them. Private ticket 58.
//
// Pure C++ (no Windows, no Metal): tests/host/archive_draw.cpp builds it natively.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace mullion::archive_draw {

struct Counters {
  std::atomic<uint64_t> made{0}, missed{0}, ns{0};
};

// this DLL's
inline Counters counters;

// the lookups that missed on this thread (Missed), so that a make tells its own misses from other threads'
inline thread_local uint64_t misses_here = 0;

// a lookup in the archive that missed, and so a compile (MTLD3D12Device::Archived)
inline void
Missed() {
  misses_here++;
}

// `make` run as a pipeline's make at its first draw: counted in `to`, as missed when a lookup of it missed. Counts
// nothing when `on` is false (MULLION_STATS off)
template <typename Make>
void
Made(bool on, Make &&make, Counters &to = counters) {
  if (!on) {
    make();
    return;
  }
  auto before = misses_here;
  auto start = std::chrono::steady_clock::now();
  make();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  // `made` last, released: a window that sees it sees its miss and its time (Section)
  if (misses_here != before)
    to.missed.fetch_add(1, std::memory_order_relaxed);
  to.ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
  to.made.fetch_add(1, std::memory_order_release);
}

// what was counted at the window before
struct Last {
  uint64_t made = 0, missed = 0, ns = 0;
};

// ARCHIVEDRAW's text for the second that ends: what was made since `last`; "" for a second without one. `made` is read
// first: a miss or a time counted before its `made` is visible stays for the window that sees it, as a second without
// a pipeline leaves `last` as it was
inline std::string
Section(Counters &from, Last &last) {
  auto made = from.made.load(std::memory_order_acquire);
  if (made == last.made)
    return {};
  auto missed = from.missed.load(std::memory_order_relaxed), ns = from.ns.load(std::memory_order_relaxed);
  auto window = made - std::exchange(last.made, made), window_missed = missed - std::exchange(last.missed, missed),
       window_ns = ns - std::exchange(last.ns, ns);
  char text[96];
  std::snprintf(text, sizeof(text), "made=%llu missed=%llu ms=%.1f", (unsigned long long)window,
                (unsigned long long)window_missed, double(window_ns) / 1e6);
  return text;
}

} // namespace mullion::archive_draw
