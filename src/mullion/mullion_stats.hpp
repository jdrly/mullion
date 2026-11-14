#pragma once

// MULLION_STATS=1: one STATS line a second of presenting, at warn, from the thread that presents
// (d3d12_swapchain.cpp), in the form SWTOR on Mac's session report reads from d9mt (scripts/session-report.sh):
//
//   STATS t=63 fps=59.9 ms=16.7/18.0/21.4 slow33=0 slow50=0 | cb=13.0 rp=31.4 cp=2.0 draws=1520
//     | sh=0 pso=0 wait=0/0.0ms cache=0/0 | allocMiB=5210 | mainWaitMaxMs=0 | at=20:01:04.120
//
//   t            seconds since the first Present
//   fps          Presents of the window per second of it
//   ms           Present-to-Present intervals of the window: average / 99th percentile / longest
//   slow33/50    intervals of the window longer than 33.4 / 50 ms
//   cb rp cp     per Present: command buffers committed, render passes (draw passes and clears), compute passes
//   draws        per Present: draw calls (DrawInstanced, DrawIndexedInstanced) of the command lists executed
//   sh pso       shaders converted, pipelines made, in the window
//   wait         draws and dispatches that waited for a pipeline still being made: count / total ms
//   cache        shader cache lookups: found / not found (and converted)
//   allocMiB     Metal's currentAllocatedSize at the line
//   mainWaitMaxMs  the longest wait of a layer change for the main thread in the window, a change still waiting
//                included (the bridge's WMTMainThreadWaitMax); n/a when the bridge didn't answer
//   at           the local time at the line
//
// The names up to slow50, sh, pso, wait and allocMiB are the report's and are never renamed. A new group goes after a
// " | ", before " | at=". A module's own numbers don't widen the line: it registers a section (SectionRegistrar), and
// each section with something to say prints as its own line, "<NAME> k=v ...", right after STATS.
//
// Counters any thread writes are relaxed atomics, read once a second. Pure C++ (no Windows, no Metal):
// tests/host/stats.cpp builds it natively.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mullion::stats {

// MULLION_STATS, read once as the DLL loads (d3d12_swapchain.cpp). Every place that counts checks it first
extern const bool on;

// every line is shorter, as the session header's (docs/swtor/README.md § Log lines)
constexpr size_t kLineLimit = 900;
// d9mt's thresholds (driver/d3d9/d3d9_stats.h): an interval longer than these is slow
constexpr double kSlow33Ms = 33.4, kSlow50Ms = 50.0;

// the process's counts since it started; any thread adds
struct Counters {
  std::atomic<uint64_t> command_buffers{0}, render_passes{0}, compute_passes{0}, draws{0};
  std::atomic<uint64_t> shaders{0}, pipelines{0}, waits{0}, wait_ns{0}, cache_hits{0}, cache_misses{0};
  std::atomic<uint64_t> cache_rejected{0};
};

// this DLL's
inline Counters counters;

// mainWaitMaxMs when the bridge didn't answer (WMTMainThreadWaitUnknown)
constexpr uint64_t kNoAnswer = UINT64_MAX;

// the counts as plain numbers
struct Totals {
  uint64_t command_buffers = 0, render_passes = 0, compute_passes = 0, draws = 0;
  uint64_t shaders = 0, pipelines = 0, waits = 0, wait_ns = 0, cache_hits = 0, cache_misses = 0;
  uint64_t cache_rejected = 0;
};

inline Totals
Read(const Counters &from = counters) {
  auto get = [](const std::atomic<uint64_t> &counter) { return counter.load(std::memory_order_relaxed); };
  return {get(from.command_buffers), get(from.render_passes), get(from.compute_passes), get(from.draws),
          get(from.shaders),         get(from.pipelines),     get(from.waits),          get(from.wait_ns),
          get(from.cache_hits),      get(from.cache_misses),  get(from.cache_rejected)};
}

// the local time of a line
struct Clock {
  unsigned hour = 0, minute = 0, second = 0, millisecond = 0;
};

// a section's text for the window that ends ("k=v ..."), or "" for nothing: the window's length and its Presents
using SectionFn = std::string (*)(double seconds, uint64_t presents);

class Sections {
public:
  void
  add(const char *name, SectionFn fn) {
    std::lock_guard lock(mutex_);
    sections_.emplace_back(name, fn);
  }

  // each section with something to say, "<NAME> <text>", in the order they registered, shorter than kLineLimit
  std::vector<std::string>
  lines(double seconds, uint64_t presents) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> lines;
    for (auto &[name, fn] : sections_)
      if (auto text = fn(seconds, presents); !text.empty())
        lines.push_back((name + " " + text).substr(0, kLineLimit - 1));
    return lines;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::string, SectionFn>> sections_;
};

// this DLL's
inline Sections &
processSections() {
  static Sections sections;
  return sections;
}

// at namespace scope: static stats::SectionRegistrar kCache{"CACHE", CacheSection};
struct SectionRegistrar {
  SectionRegistrar(const char *name, SectionFn fn, Sections &sections = processSections()) { sections.add(name, fn); }
};

// the CACHE section's text (d3d12_pipeline_graphics.cpp): the stored shaders Metal refused since `last`, the count at
// the window before, each deleted and converted again; "" when there were none
inline std::string
CacheSection(uint64_t rejected, uint64_t &last) {
  uint64_t window = rejected - std::exchange(last, rejected);
  return window ? "rejected=" + std::to_string(window) : std::string();
}

// the second of Presents a line is about
class Window {
public:
  static constexpr uint64_t kPeriodNs = 1'000'000'000;

  // a Present at `now` (nanoseconds, a steady clock); true when the window has lasted a second and Take is due
  bool
  Add(uint64_t now) {
    if (!started_) {
      started_ = true;
      start_ = window_start_ = last_ = now;
      return false;
    }
    intervals_.push_back(double(now - last_) / 1e6);
    last_ = now;
    return now - window_start_ >= kPeriodNs;
  }

  // the STATS line and the sections' lines for the window that ends at `now`, and the next window starts. `totals` are
  // the counters now, `allocated` Metal's allocated bytes, `main_wait_ms` the window's longest wait for the main thread
  // or kNoAnswer
  std::vector<std::string>
  Take(uint64_t now, const Totals &totals, uint64_t allocated, uint64_t main_wait_ms, const Clock &at,
       const Sections &sections = processSections()) {
    const double seconds = double(now - window_start_) / 1e9;
    const uint64_t n = intervals_.size();
    double sum = 0, longest = 0, p99 = 0;
    unsigned slow33 = 0, slow50 = 0;
    for (double interval : intervals_) {
      sum += interval;
      longest = std::max(longest, interval);
      slow33 += interval > kSlow33Ms;
      slow50 += interval > kSlow50Ms;
    }
    // d9mt's: the interval at n * 0.99 of the sorted window
    if (n) {
      auto k = std::min<size_t>(n - 1, size_t(double(n) * 0.99));
      std::nth_element(intervals_.begin(), intervals_.begin() + k, intervals_.end());
      p99 = intervals_[k];
    }
    const double frames = n ? double(n) : 1;
    auto per = [&](uint64_t Totals::*counter) { return double(totals.*counter - last_totals_.*counter) / frames; };
    auto in = [&](uint64_t Totals::*counter) { return (unsigned long long)(totals.*counter - last_totals_.*counter); };

    const std::string main_wait = main_wait_ms == kNoAnswer ? "n/a" : std::to_string(main_wait_ms);
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "STATS t=%.0f fps=%.1f ms=%.1f/%.1f/%.1f slow33=%u slow50=%u"
                  " | cb=%.1f rp=%.1f cp=%.1f draws=%.0f"
                  " | sh=%llu pso=%llu wait=%llu/%.1fms cache=%llu/%llu"
                  " | allocMiB=%llu | mainWaitMaxMs=%s | at=%02u:%02u:%02u.%03u",
                  double(now - start_) / 1e9, seconds > 0 ? double(n) / seconds : 0.0, n ? sum / double(n) : 0.0, p99,
                  longest, slow33, slow50, per(&Totals::command_buffers), per(&Totals::render_passes),
                  per(&Totals::compute_passes), per(&Totals::draws), in(&Totals::shaders), in(&Totals::pipelines),
                  in(&Totals::waits), double(totals.wait_ns - last_totals_.wait_ns) / 1e6, in(&Totals::cache_hits),
                  in(&Totals::cache_misses), (unsigned long long)(allocated >> 20), main_wait.c_str(), at.hour % 100,
                  at.minute % 100, at.second % 100, at.millisecond % 1000);

    std::vector<std::string> lines{line};
    for (auto &section : sections.lines(seconds, n))
      lines.push_back(std::move(section));
    last_totals_ = totals;
    window_start_ = now;
    intervals_.clear();
    return lines;
  }

private:
  bool started_ = false;
  uint64_t start_ = 0, window_start_ = 0, last_ = 0;
  std::vector<double> intervals_;
  Totals last_totals_;
};

} // namespace mullion::stats
