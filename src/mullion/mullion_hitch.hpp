#pragma once

// MULLION_STATS=1: a HITCH line for a frame at or over MULLION_HITCH_MS, and a COMPILE line for a frame with compile
// work or one of 33.4 ms or more, written at the Present that ends the frame, from the thread that presents
// (d3d12_swapchain.cpp), in the shape of d9mt's (driver/d3d9/d3d9_hitch.h, driver/mtl/mtl_compile.h):
//
//   HITCH frame=2228 dtMs=70.0 inDriverMs=40.0 (57%) | pipelineWait=1/38.6ms present=1/1.4ms
//     | queue: gpuMs=9.8 encodeMs=2.3 behind=2 | at=20:00:44.142
//   COMPILE frame=2228 dtMs=70.0 shaders=2/18.2ms psos=1/31.0ms renderWait=1/38.6ms cache=0/2 queued=0
//
// And always, whatever MULLION_STATS is, a line for each pipeline that took 20 ms or more or was made after the first
// Present (d3d12_device.cpp, PipelineMade), at most 30 in 10 s (gpu_error::Limiter):
//
//   MULLION-PIPELINE at=20:00:44.120 kind=graphics ms=23.4 late=yes variant=topology name=<SHA-1>,<SHA-1>
//
// inDriverMs is the time the presenting thread spent in the driver's calls that can block (Scope): this driver's name
// for d9mt's inD9mtMs. Only the outermost scope of a thread counts, so the categories add up to it. A category with
// nothing in it is left out; "none" when all are. A ticket adds a category with an entry before kCategories and its
// name in kNames.
//
// Pure C++ (no Windows, no Metal): tests/host/hitch.cpp builds it natively.

#include "mullion_stats.hpp"
#include <chrono>
#include <cstring>

namespace mullion::hitch {

// MULLION_HITCH_MS's default: d9mt's D9MT_HITCH_MS
constexpr unsigned kDefaultMs = 50;
// a frame this long has a COMPILE line with no compile work in it (d9mt's)
constexpr double kCompileSlowMs = 33.4;

enum Category : unsigned {
  PipelineWait,  // a draw or dispatch waiting for its Metal pipeline (DeferredPipeline::Wait)
  ShaderConvert, // CreateGraphicsPipelineState, CreateComputePipelineState, CreatePipelineState, and a shader converted
                 // for a variant at draw time
  Present,       // the Present call, up to the point its frame ends
  FenceWait,     // SetEventOnCompletion and SetEventOnMultipleFenceCompletion without an event: the call waits
  AllocReset,    // a command allocator's Reset waiting for its lists (MULLION_D3D12_RESET_WAITS)
  kCategories
};
inline constexpr const char *kNames[kCategories] = {
    "pipelineWait", "shaderConvert", "present", "fenceWait", "allocReset"
};

// a thread's scopes since its last frame ended: the outermost ones only
struct Frame {
  uint64_t n[kCategories] = {}, ns[kCategories] = {};
  // a scope of the thread's is counting
  bool inside = false;
};

// this thread's
inline thread_local Frame thread_frame;

// the presenting thread's frame, which starts again empty
inline Frame
Take() {
  Frame taken = thread_frame;
  for (unsigned i = 0; i < kCategories; i++)
    thread_frame.n[i] = thread_frame.ns[i] = 0;
  return taken;
}

// nanoseconds of the steady clock, which reads QueryPerformanceCounter under Windows, as diag::Now does
inline uint64_t
SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// the time from here to End (or the scope's end) in `category` of this thread's frame, unless the thread is in a scope
// already, which counts it. With MULLION_STATS off: one load and one branch here, one test of a null pointer at the end
template <uint64_t (*Now)() = SteadyNs> class Scope {
public:
  explicit Scope(Category category) {
    if (!stats::on) [[likely]]
      return;
    Frame &frame = thread_frame;
    if (frame.inside)
      return;
    frame.inside = true;
    frame_ = &frame;
    category_ = category;
    start_ = Now();
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  ~Scope() { End(); }

  void
  End() {
    if (!frame_) [[likely]]
      return;
    frame_->n[category_]++;
    frame_->ns[category_] += Now() - start_;
    frame_->inside = false;
    frame_ = nullptr;
  }

private:
  // set only while it counts
  Frame *frame_ = nullptr;
  Category category_;
  uint64_t start_;
};

// at its end, with MULLION_STATS on, adds the time from here to `ns` and one to `count`, so that a frame mostly sees
// both or neither. A Present that reads between the two sees the time alone, which Frames writes too
template <uint64_t (*Now)() = SteadyNs> class Timed {
public:
  Timed(std::atomic<uint64_t> &count, std::atomic<uint64_t> &ns)
      : count_(stats::on ? &count : nullptr), ns_(&ns), start_(count_ ? Now() : 0) {}
  Timed(const Timed &) = delete;
  Timed &operator=(const Timed &) = delete;
  ~Timed() {
    if (!count_) [[likely]]
      return;
    ns_->fetch_add(Now() - start_, std::memory_order_relaxed);
    count_->fetch_add(1, std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> *count_, *ns_;
  uint64_t start_;
};

// what COMPILE and HITCH's queue group read beside ticket 07's counters; any thread adds, each place testing
// stats::on first
struct Counters {
  // shaders converted (or tried) and Metal pipelines compiled, each counted with its time when it ends (Timed): a
  // shader's with its Metal function made and stored in the cache. STATS's sh and pso count them as they begin
  std::atomic<uint64_t> shaders{0}, shader_ns{0}, pipelines{0}, pipeline_ns{0};
  // time spent by the GPU on the command buffers completed (as Metal gave it by their completion), and encoding command
  // buffers (from a slot taken to its commit)
  std::atomic<uint64_t> gpu_ns{0}, encode_ns{0};
  // command buffers completed (or cancelled), against ticket 07's committed
  std::atomic<uint64_t> completed{0};
  // Metal pipelines left to the compile threads or to their first draw, not made yet
  std::atomic<int64_t> compiling{0};
};

// this DLL's
inline Counters counters;

struct Totals {
  uint64_t shaders = 0, shader_ns = 0, pipelines = 0, pipeline_ns = 0, gpu_ns = 0, encode_ns = 0, completed = 0;
  int64_t compiling = 0;
};

inline Totals
Read(const Counters &from = counters) {
  auto get = [](const auto &counter) { return counter.load(std::memory_order_relaxed); };
  return {get(from.shaders), get(from.shader_ns), get(from.pipelines), get(from.pipeline_ns),
          get(from.gpu_ns),  get(from.encode_ns), get(from.completed), get(from.compiling)};
}

// the frames of the counted swap chain: Present to Present
class Frames {
public:
  // a Present at `now` (ns, the steady clock) ends a frame: true when it has a line. `frame` is the presenting thread's
  // (Take), `stats` and `totals` the counters now
  bool
  End(uint64_t now, const Frame &frame, const stats::Totals &stats, const Totals &totals, double hitch_ms) {
    const bool first = !frame_++;
    dt_ms_ = first ? 0 : double(now - last_) / 1e6;
    last_ = now;
    frame_data_ = frame;
    auto in = [&](uint64_t stats::Totals::*counter) { return stats.*counter - last_stats_.*counter; };
    auto of = [&](uint64_t Totals::*counter) { return totals.*counter - last_totals_.*counter; };
    waits_ = in(&stats::Totals::waits), wait_ns_ = in(&stats::Totals::wait_ns), hits_ = in(&stats::Totals::cache_hits);
    misses_ = in(&stats::Totals::cache_misses);
    shaders_ = of(&Totals::shaders), shader_ns_ = of(&Totals::shader_ns), psos_ = of(&Totals::pipelines);
    pipeline_ns_ = of(&Totals::pipeline_ns), gpu_ns_ = of(&Totals::gpu_ns), encode_ns_ = of(&Totals::encode_ns);
    behind_ = stats.command_buffers > totals.completed ? stats.command_buffers - totals.completed : 0;
    compiling_ = totals.compiling > 0 ? uint64_t(totals.compiling) : 0;
    last_stats_ = stats;
    last_totals_ = totals;
    hitch_ = !first && dt_ms_ >= hitch_ms;
    // a time without its count is a compile that ended as the counters were read: written, not lost
    compile_ = !first && (shaders_ || psos_ || waits_ || shader_ns_ || pipeline_ns_ || dt_ms_ >= kCompileSlowMs);
    return hitch_ || compile_;
  }

  bool
  Hitch() const {
    return hitch_;
  }
  bool
  Compile() const {
    return compile_;
  }

  // the HITCH line of the frame End was told of, `at` the local time now
  std::string
  HitchLine(const stats::Clock &at) const {
    uint64_t inside = 0;
    for (auto ns : frame_data_.ns)
      inside += ns;
    char text[256];
    std::snprintf(text, sizeof(text), "HITCH frame=%llu dtMs=%.1f inDriverMs=%.1f (%.0f%%) |",
                  (unsigned long long)frame_, dt_ms_, double(inside) / 1e6,
                  dt_ms_ > 0 ? 100.0 * double(inside) / 1e6 / dt_ms_ : 0.0);
    std::string line = text;
    bool any = false;
    for (unsigned i = 0; i < kCategories; i++)
      if (frame_data_.n[i]) {
        std::snprintf(text, sizeof(text), " %s=%llu/%.1fms", kNames[i], (unsigned long long)frame_data_.n[i],
                      double(frame_data_.ns[i]) / 1e6);
        line += text;
        any = true;
      }
    if (!any)
      line += " none";
    std::snprintf(text, sizeof(text), " | queue: gpuMs=%.1f encodeMs=%.1f behind=%llu | at=%02u:%02u:%02u.%03u",
                  double(gpu_ns_) / 1e6, double(encode_ns_) / 1e6, (unsigned long long)behind_, at.hour % 100,
                  at.minute % 100, at.second % 100, at.millisecond % 1000);
    return (line + text).substr(0, stats::kLineLimit - 1);
  }

  // the COMPILE line of the frame End was told of
  std::string
  CompileLine() const {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "COMPILE frame=%llu dtMs=%.1f shaders=%llu/%.1fms psos=%llu/%.1fms renderWait=%llu/%.1fms"
                  " cache=%llu/%llu queued=%llu",
                  (unsigned long long)frame_, dt_ms_, (unsigned long long)shaders_, double(shader_ns_) / 1e6,
                  (unsigned long long)psos_, double(pipeline_ns_) / 1e6, (unsigned long long)waits_,
                  double(wait_ns_) / 1e6, (unsigned long long)hits_, (unsigned long long)misses_,
                  (unsigned long long)compiling_);
    return text;
  }

private:
  uint64_t frame_ = 0, last_ = 0;
  double dt_ms_ = 0;
  Frame frame_data_;
  uint64_t shaders_ = 0, psos_ = 0, waits_ = 0, wait_ns_ = 0, hits_ = 0, misses_ = 0;
  uint64_t shader_ns_ = 0, pipeline_ns_ = 0, gpu_ns_ = 0, encode_ns_ = 0, behind_ = 0, compiling_ = 0;
  stats::Totals last_stats_;
  Totals last_totals_;
  bool hitch_ = false, compile_ = false;
};

// MULLION-PIPELINE: a pipeline that took 20 ms or more, or was made after the first Present
constexpr double kSlowPipelineMs = 20;

// the pipeline's line. `name` its shaders' SHA-1s, each followed by a space (MTLD3D12PipelineState::Name), which the line
// joins with commas; `dropped` the lines the limit (gpu_error::Limiter, 30 in 10 s) left out before it, as
// MULLION-GPU-ERROR says them
inline std::string
PipelineLine(const stats::Clock &at, const char *kind, double ms, bool late, const char *variant,
             const std::string &name, uint64_t dropped) {
  char text[200];
  std::snprintf(text, sizeof(text), "MULLION-PIPELINE at=%02u:%02u:%02u.%03u kind=%s ms=%.1f late=%s variant=%s",
                at.hour % 100, at.minute % 100, at.second % 100, at.millisecond % 1000, kind, ms, late ? "yes" : "no",
                variant);
  std::string joined = name;
  while (!joined.empty() && joined.back() == ' ')
    joined.pop_back();
  for (auto &c : joined)
    if (c == ' ')
      c = ',';
  std::string tail = dropped ? " [" + std::to_string(dropped) + " earlier lines suppressed]" : "";
  return std::string(text) + " name=" + joined.substr(0, stats::kLineLimit - 1 - std::strlen(text) - 6 - tail.size()) +
         tail;
}

// the totals at the end of a session (each queue's release, with MULLION_D3D12_GPU_ERRORS): the late pipelines
inline std::string
PipelineTotalsLine(uint64_t late, double ms, double longest_ms) {
  char text[128];
  std::snprintf(text, sizeof(text), "MULLION-PIPELINE-TOTALS late=%llu ms=%.1f longestMs=%.1f", (unsigned long long)late,
                ms, longest_ms);
  return text;
}

} // namespace mullion::hitch
