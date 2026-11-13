// contract: the HITCH and COMPILE lines (src/mullion/mullion_hitch.hpp) have d9mt's shape with the fields
// docs/swtor/README.md § Log lines gives them: a 70 ms frame with 40 ms of pipeline wait on the presenting thread
// prints inDriverMs=40.0 (57%) and the category, one with nothing inside prints none, and a frame under
// MULLION_HITCH_MS prints no HITCH; COMPILE comes for a frame with compile work or of 33.4 ms or more; nested scopes
// count once; the MULLION-PIPELINE line joins its shaders' names and ends with the lines its limit left out; no line
// reaches 900 bytes, the suppressed count kept; and the HITCH and COMPILE lines of tests/swtor/fixtures/session.log are what the frames write
#include "../../src/mullion/mullion_hitch.hpp"
#include <cstdio>
#include <fstream>
#include <limits>

using namespace mullion;

const bool mullion::stats::on = true;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static void
equal(const std::string &got, const std::string &want) {
  check(got == want, "got\n  " + got + "\nwant\n  " + want);
}

constexpr uint64_t kMs = 1'000'000;

// the scopes' clock, which the test moves
static uint64_t clock_ns = 0;
static uint64_t
TestNow() {
  return clock_ns;
}
using Scope = hitch::Scope<TestNow>;

// a frame of the presenting thread with these (category, ms) in it
static hitch::Frame
With(std::initializer_list<std::pair<hitch::Category, double>> spent) {
  hitch::Frame frame;
  for (auto [category, ms] : spent)
    frame.n[category]++, frame.ns[category] += uint64_t(ms * 1e6 + 0.5);
  return frame;
}

// `count` frames of 16.7 ms with nothing in them, which write nothing
static void
Quiet(hitch::Frames &frames, uint64_t &now, unsigned count, const stats::Totals &stats, const hitch::Totals &totals) {
  for (unsigned i = 0; i < count; i++)
    check(!frames.End(now += 16'700'000, {}, stats, totals, hitch::kDefaultMs), "a quiet frame writes nothing");
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }
  const stats::Clock at{20, 1, 4, 120};
  stats::Totals stats;
  hitch::Totals totals;

  // the first Present has no frame before it
  hitch::Frames frames;
  uint64_t now = 5000 * kMs;
  check(!frames.End(now, With({{hitch::Present, 0.5}}), stats, totals, hitch::kDefaultMs), "the first Present");

  // 70 ms, of which 40 waiting for a pipeline: the driver's
  check(frames.End(now += 70 * kMs, With({{hitch::PipelineWait, 40}}), stats, totals, hitch::kDefaultMs),
        "a 70 ms frame has a line");
  check(frames.Hitch(), "a 70 ms frame is a hitch");
  equal(frames.HitchLine(at), "HITCH frame=2 dtMs=70.0 inDriverMs=40.0 (57%) | pipelineWait=1/40.0ms"
                              " | queue: gpuMs=0.0 encodeMs=0.0 behind=0 | at=20:01:04.120");
  check(frames.Compile(), "a 70 ms frame has a COMPILE line too");
  equal(frames.CompileLine(), "COMPILE frame=2 dtMs=70.0 shaders=0/0.0ms psos=0/0.0ms renderWait=0/0.0ms cache=0/0"
                              " queued=0");

  // 70 ms with nothing inside: the game's
  check(frames.End(now += 70 * kMs, {}, stats, totals, hitch::kDefaultMs) && frames.Hitch(), "a second hitch");
  equal(frames.HitchLine(at), "HITCH frame=3 dtMs=70.0 inDriverMs=0.0 (0%) | none"
                              " | queue: gpuMs=0.0 encodeMs=0.0 behind=0 | at=20:01:04.120");

  // at the threshold is a hitch, under it not; MULLION_HITCH_MS moves it
  check(frames.End(now += 50 * kMs, {}, stats, totals, hitch::kDefaultMs) && frames.Hitch(), "50.0 ms is a hitch");
  check(frames.End(now += 49'900'000, {}, stats, totals, hitch::kDefaultMs) && !frames.Hitch(),
        "49.9 ms is no hitch, and has a COMPILE line");
  check(frames.End(now += 49'900'000, {}, stats, totals, 40) && frames.Hitch(), "49.9 ms is one at 40");

  // COMPILE: from 33.4 ms, or with compile work; warn only for a hitch (the caller's level), every category in order
  check(!frames.End(now += 33'300'000, {}, stats, totals, hitch::kDefaultMs), "33.3 ms with nothing writes nothing");
  check(frames.End(now += 33'400'000, {}, stats, totals, hitch::kDefaultMs) && frames.Compile() && !frames.Hitch(),
        "33.4 ms has a COMPILE line");
  totals.shaders += 3, totals.pipelines += 2, stats.waits += 1, stats.wait_ns += 12'340'000;
  stats.cache_hits += 5, stats.cache_misses += 3, stats.command_buffers += 7;
  totals.shader_ns += 21'000'000, totals.pipeline_ns += 44'440'000, totals.gpu_ns += 9'960'000;
  totals.encode_ns += 1'250'000, totals.completed += 4, totals.compiling = 6;
  check(frames.End(now += 16'700'000, With({{hitch::FenceWait, 2}, {hitch::Present, 1}, {hitch::ShaderConvert, 3}}),
                   stats, totals, hitch::kDefaultMs) &&
            frames.Compile() && !frames.Hitch(),
        "a short frame with compile work has a COMPILE line");
  equal(frames.CompileLine(), "COMPILE frame=9 dtMs=16.7 shaders=3/21.0ms psos=2/44.4ms renderWait=1/12.3ms"
                              " cache=5/3 queued=6");
  check(frames.End(now += 60 * kMs, With({{hitch::FenceWait, 2}, {hitch::Present, 1}, {hitch::ShaderConvert, 3}}),
                   stats, totals, hitch::kDefaultMs),
        "a 60 ms frame");
  equal(frames.HitchLine(at), "HITCH frame=10 dtMs=60.0 inDriverMs=6.0 (10%) | shaderConvert=1/3.0ms present=1/1.0ms"
                              " fenceWait=1/2.0ms | queue: gpuMs=0.0 encodeMs=0.0 behind=3 | at=20:01:04.120");
  equal(frames.CompileLine(), "COMPILE frame=10 dtMs=60.0 shaders=0/0.0ms psos=0/0.0ms renderWait=0/0.0ms cache=0/0"
                              " queued=6");

  // a pipeline compiled across a frame's end counts, with its time, in the frame it ends in
  {
    hitch::Frames across;
    hitch::Counters counters;
    clock_ns = 0;
    across.End(clock_ns, {}, {}, hitch::Read(counters), hitch::kDefaultMs);
    {
      hitch::Timed<TestNow> compiling(counters.pipelines, counters.pipeline_ns);
      check(!across.End(clock_ns += 16 * kMs, {}, {}, hitch::Read(counters), hitch::kDefaultMs),
            "a frame a compile only began in has nothing to say");
      clock_ns += 8 * kMs;
    }
    check(across.End(clock_ns += 8 * kMs, {}, {}, hitch::Read(counters), hitch::kDefaultMs) && across.Compile(),
          "the frame it ended in has a COMPILE line");
    equal(across.CompileLine(), "COMPILE frame=3 dtMs=16.0 shaders=0/0.0ms psos=1/24.0ms renderWait=0/0.0ms cache=0/0"
                                " queued=0");
  }

  // the largest numbers stay under 900 bytes
  {
    hitch::Frames big;
    hitch::Frame full;
    for (unsigned i = 0; i < hitch::kCategories; i++)
      full.n[i] = full.ns[i] = std::numeric_limits<uint64_t>::max() / 8;
    stats::Totals most;
    most.command_buffers = most.waits = most.wait_ns = most.cache_hits = most.cache_misses =
        std::numeric_limits<uint64_t>::max();
    hitch::Totals all{~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, 0, std::numeric_limits<int64_t>::max()};
    big.End(0, {}, {}, {}, hitch::kDefaultMs);
    big.End(~0ull, full, most, all, hitch::kDefaultMs);
    check(big.HitchLine({99, 99, 99, 999}).size() < stats::kLineLimit, "HITCH under 900 bytes");
    check(big.CompileLine().size() < stats::kLineLimit, "COMPILE under 900 bytes");
    auto longest = hitch::PipelineLine(at, "graphics", 1e300, true, "sampler-emulation", std::string(2000, 'x'), ~0ull);
    check(longest.size() < stats::kLineLimit, "MULLION-PIPELINE under 900 bytes");
    check(longest.ends_with(" earlier lines suppressed]"), "and its suppressed count kept");
  }

  // scopes: only a thread's outermost one counts, and End ends it once
  {
    clock_ns = 1000;
    {
      Scope wait(hitch::PipelineWait);
      clock_ns += 5 * kMs;
      {
        Scope convert(hitch::ShaderConvert);
        clock_ns += 3 * kMs;
      }
      clock_ns += 2 * kMs;
    }
    Scope present(hitch::Present);
    clock_ns += 4 * kMs;
    present.End();
    clock_ns += 9 * kMs;
    present.End();
    auto frame = hitch::Take();
    check(frame.n[hitch::PipelineWait] == 1 && frame.ns[hitch::PipelineWait] == 10 * kMs, "the outer scope's 10 ms");
    check(!frame.n[hitch::ShaderConvert] && !frame.ns[hitch::ShaderConvert], "the nested scope counts nothing");
    check(frame.n[hitch::Present] == 1 && frame.ns[hitch::Present] == 4 * kMs, "End ends it once");
    auto again = hitch::Take();
    check(!again.n[hitch::PipelineWait] && !again.ns[hitch::Present] && !again.inside, "Take starts the frame again");
  }

  // the pipeline line, with the count the limit left out before it (its limit is MULLION-GPU-ERROR's, which
  // tests/host/gpu_error.cpp tests)
  equal(hitch::PipelineLine(at, "graphics", 23.44, true, "topology",
                            "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b 2c26b46b68ffc68ff99b453c1d30413413422d70 ", 0),
        "MULLION-PIPELINE at=20:01:04.120 kind=graphics ms=23.4 late=yes variant=topology"
        " name=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b,2c26b46b68ffc68ff99b453c1d30413413422d70");
  equal(hitch::PipelineLine(at, "compute", 20, false, "base", "fcde2b2edba56bf408601fb721fe9b5c338d10ee ", 4),
        "MULLION-PIPELINE at=20:01:04.120 kind=compute ms=20.0 late=no variant=base"
        " name=fcde2b2edba56bf408601fb721fe9b5c338d10ee [4 earlier lines suppressed]");
  equal(hitch::PipelineTotalsLine(12, 412.345, 88.06), "MULLION-PIPELINE-TOTALS late=12 ms=412.3 longestMs=88.1");
  // the fixture: its HITCH and COMPILE lines, of the session tests/host/stats.cpp writes the STATS lines of: frame
  // 967 is second 20's 120 ms, 2227 and 2228 second 41's 45 and 70 ms (tests/host/stats.cpp, Session)
  {
    std::vector<std::string> want, got;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.find(" HITCH frame=") != std::string::npos || line.find(" COMPILE frame=") != std::string::npos)
        want.push_back(line);
    hitch::Frames session;
    stats::Totals stats_now;
    hitch::Totals totals_now;
    uint64_t t = 1'000'000 * kMs;
    auto write = [&](const stats::Clock &when) {
      if (session.Hitch())
        got.push_back("warn:  " + session.HitchLine(when));
      if (session.Compile())
        got.push_back((session.Hitch() ? "warn:  " : "info:  ") + session.CompileLine());
    };
    session.End(t, {}, stats_now, totals_now, hitch::kDefaultMs);
    Quiet(session, t, 965, stats_now, totals_now);
    // second 20's hitch: a pipeline compiled and a draw on another thread waited for it; the presenting thread only
    // presented. The game's
    totals_now.shaders += 2, totals_now.pipelines += 1, stats_now.waits += 1, stats_now.wait_ns += 38'600'000;
    stats_now.cache_misses += 2, stats_now.command_buffers += 5;
    totals_now.shader_ns += 18'200'000, totals_now.pipeline_ns += 31'000'000, totals_now.gpu_ns += 10'400'000;
    totals_now.encode_ns += 2'100'000, totals_now.completed += 3;
    session.End(t += 120 * kMs, With({{hitch::Present, 0.4}}), stats_now, totals_now, hitch::kDefaultMs);
    write({20, 0, 23, 125});
    Quiet(session, t, 1259, stats_now, totals_now);
    // second 41: a 45 ms frame with nothing in it, then 70 ms with 38.6 of them waiting for a pipeline: the driver's
    session.End(t += 45 * kMs, {}, stats_now, totals_now, hitch::kDefaultMs);
    write({20, 0, 44, 72});
    totals_now.shaders += 2, totals_now.pipelines += 1, stats_now.waits += 1, stats_now.wait_ns += 38'600'000;
    stats_now.cache_misses += 2, stats_now.command_buffers += 5;
    totals_now.shader_ns += 18'200'000, totals_now.pipeline_ns += 31'000'000, totals_now.gpu_ns += 9'800'000;
    totals_now.encode_ns += 2'300'000, totals_now.completed += 5;
    session.End(t += 70 * kMs, With({{hitch::PipelineWait, 38.6}, {hitch::Present, 1.4}}), stats_now, totals_now,
                hitch::kDefaultMs);
    write({20, 0, 44, 142});
    check(got.size() == 5, "five lines");
    if (got != want) {
      std::printf("the session's lines:\n");
      for (auto &line : got)
        std::printf("%s\n", line.c_str());
    }
    check(got == want, std::string("the HITCH and COMPILE lines of ") + argv[1]);
  }

  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
