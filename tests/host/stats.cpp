// contract: the STATS line (src/mullion/mullion_stats.hpp) has the fields, order and arithmetic of d9mt's that SWTOR on
// Mac's session report reads (docs/swtor/README.md § Log lines): a fixed list of Present intervals gives the line
// written here by hand, the 99th percentile and the slow counts as computed by hand; a second with no Present prints
// nothing; sections print after STATS, the empty ones not at all; no line reaches 900 bytes; and the sixty STATS lines
// of tests/swtor/fixtures/session.log are what the window writes for their session
#include "../../src/mullion/mullion_stats.hpp"
#include <cstdio>
#include <fstream>
#include <limits>

using namespace mullion;

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

// Presents `intervals` (ns) apart after `now`; how many of them closed a window, and the last one's time
static unsigned
Present(stats::Window &window, uint64_t &now, std::initializer_list<uint64_t> intervals) {
  unsigned due = 0;
  for (auto interval : intervals)
    due += window.Add(now += interval);
  return due;
}

static std::string
SectionEmpty(double, uint64_t) {
  return "";
}

static std::string
SectionPresents(double seconds, uint64_t presents) {
  char text[64];
  std::snprintf(text, sizeof(text), "presents=%llu seconds=%.1f", (unsigned long long)presents, seconds);
  return text;
}

static std::string
SectionLong(double, uint64_t) {
  return "long=" + std::string(2000, 'x');
}

// the fixture's session: sixty seconds, loading for the first five (fewer frames, shaders converted and pipelines
// made, draws waiting for them), then 60 fps with a hitch at 20 s and two at 41 s
static std::vector<std::string>
Session() {
  stats::Window window;
  stats::Sections none;
  stats::Totals totals;
  std::vector<std::string> lines;
  uint64_t now = 1'000'000 * kMs;
  window.Add(now);
  for (unsigned second = 1; second <= 60; second++) {
    bool loading = second <= 5;
    std::vector<uint64_t> intervals;
    if (second == 20)
      intervals = {120 * kMs};
    else if (second == 41)
      intervals = {45 * kMs, 70 * kMs};
    uint64_t frames = loading ? 25 : 60, used = 0;
    for (auto interval : intervals)
      used += interval;
    // the rest of the second in even frames, the last one taking what is left
    uint64_t left = frames - intervals.size(), even = (1000 * kMs - used) / left;
    for (uint64_t i = 0; i + 1 < left; i++)
      intervals.push_back(even);
    intervals.push_back(1000 * kMs - used - even * (left - 1));
    for (size_t i = 0; i + 1 < intervals.size(); i++)
      check(!window.Add(now += intervals[i]), "no line before the second is over");
    check(window.Add(now += intervals.back()), "a line when it is");
    totals.command_buffers += frames * 3 + (loading ? 10 : 0);
    totals.render_passes += frames * 12 + second % 3;
    totals.compute_passes += frames * 2;
    totals.draws += frames * 850 + second * 7;
    if (loading) {
      totals.shaders += 140 - second * 20;
      totals.pipelines += 50 - second * 6;
      totals.waits += 6 - second;
      totals.wait_ns += (6 - second) * 14'300'000;
      totals.cache_hits += 90 - second * 10;
      totals.cache_misses += 140 - second * 20;
    } else if (second == 20 || second == 41) {
      totals.shaders += 2, totals.pipelines += 1, totals.waits += 1, totals.wait_ns += 38'600'000;
      totals.cache_misses += 2;
    }
    uint64_t allocated = (1800ull + std::min(second, 30u) * 20 - (second > 50 ? 15 : 0)) << 20;
    unsigned ms = 4000 + second * 1000 + second % 7;
    for (auto &line : window.Take(now, totals, allocated, {20, ms / 60000, ms / 1000 % 60, ms % 1000}, none))
      lines.push_back("warn:  " + line);
  }
  return lines;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }
  stats::Sections none;

  // a second of ten Presents: average 100, 99th percentile and longest 473; over 33.4: 33.5 and up (6), over 50:
  // 50.1 and up (4). Then two hundred: the 99th percentile is the second longest, 80, of the window's own
  stats::Window window;
  uint64_t now = 5000 * kMs;
  check(Present(window, now, {0}) == 0, "the first Present starts the window");
  check(Present(window, now, {10 * kMs, 20 * kMs, 30 * kMs, 33'400'000, 33'500'000, 50 * kMs, 50'100'000, 100 * kMs,
                              200 * kMs}) == 0,
        "nine Presents in 527 ms: no line");
  check(Present(window, now, {473 * kMs}) == 1, "the tenth closes the second");
  stats::Totals totals{.command_buffers = 25, .render_passes = 31, .compute_passes = 7, .draws = 8504,
                       .shaders = 12, .pipelines = 5, .waits = 3, .wait_ns = 45'670'000, .cache_hits = 9,
                       .cache_misses = 3};
  auto lines = window.Take(now, totals, (5210ull << 20) + (500 << 10), {20, 1, 4, 120}, none);
  check(lines.size() == 1, "no section: one line");
  equal(lines[0], "STATS t=1 fps=10.0 ms=100.0/473.0/473.0 slow33=6 slow50=4 | cb=2.5 rp=3.1 cp=0.7 draws=850"
                  " | sh=12 pso=5 wait=3/45.7ms cache=9/3 | allocMiB=5210 | at=20:01:04.120");

  for (unsigned i = 0; i < 199; i++)
    check(!window.Add(now += i == 50 ? 128 * kMs : i == 120 ? 80 * kMs : 4 * kMs), "no line inside the second");
  check(window.Add(now += 4 * kMs), "the two hundredth closes it");
  lines = window.Take(now, totals, 5210ull << 20, {20, 1, 5, 120}, none);
  equal(lines[0], "STATS t=2 fps=200.0 ms=5.0/80.0/128.0 slow33=2 slow50=2 | cb=0.0 rp=0.0 cp=0.0 draws=0"
                  " | sh=0 pso=0 wait=0/0.0ms cache=0/0 | allocMiB=5210 | at=20:01:05.120");

  // three seconds without a Present print nothing; the Present after them closes one window of 3.2 s
  check(Present(window, now, {3200 * kMs}) == 1, "one line for the gap");
  lines = window.Take(now, totals, 0, {20, 1, 8, 320}, none);
  equal(lines[0], "STATS t=5 fps=0.3 ms=3200.0/3200.0/3200.0 slow33=1 slow50=1 | cb=0.0 rp=0.0 cp=0.0 draws=0"
                  " | sh=0 pso=0 wait=0/0.0ms cache=0/0 | allocMiB=0 | at=20:01:08.320");

  // sections: each with something to say on a line of its own after STATS, in the order they registered
  {
    stats::Sections sections;
    stats::SectionRegistrar empty{"EMPTY", SectionEmpty, sections};
    stats::SectionRegistrar presents{"PRESENTS", SectionPresents, sections};
    stats::SectionRegistrar longest{"LONG", SectionLong, sections};
    stats::Window window;
    uint64_t now = 0;
    Present(window, now, {1});
    check(Present(window, now, {250 * kMs, 250 * kMs, 250 * kMs, 250 * kMs}) == 1, "four Presents in a second");
    auto lines = window.Take(now, {}, 0, {}, sections);
    check(lines.size() == 3, "STATS, PRESENTS and LONG, without EMPTY: " + std::to_string(lines.size()));
    check(lines[0].starts_with("STATS t=1 fps=4.0 "), lines[0]);
    equal(lines[1], "PRESENTS presents=4 seconds=1.0");
    check(lines[2].starts_with("LONG long=xxx") && lines[2].size() == stats::kLineLimit - 1,
          "a long section is cut below 900 bytes: " + std::to_string(lines[2].size()));
  }

  // no line reaches 900 bytes, whatever the numbers
  {
    constexpr uint64_t most = std::numeric_limits<uint64_t>::max();
    stats::Totals full{most, most, most, most, most, most, most, most, most, most};
    for (uint64_t interval : {uint64_t(1000), 1000 * kMs, most / 4}) {
      stats::Window window;
      uint64_t now = 0;
      window.Add(now);
      while (!window.Add(now += interval))
        ;
      for (auto &line : window.Take(now, full, most, {99999, 99999, 99999, 99999}, none)) {
        check(line.size() < stats::kLineLimit, "shorter than 900: " + std::to_string(line.size()) + ": " + line);
        check(line.find(" | at=") != std::string::npos, "the line is whole: " + line);
      }
    }
  }

  // the fixture: the sixty STATS lines of its session
  {
    std::vector<std::string> want;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.find(" STATS t=") != std::string::npos)
        want.push_back(line);
    auto got = Session();
    check(got.size() == 60, "sixty seconds");
    if (got != want) {
      std::printf("the session's lines:\n");
      for (auto &line : got)
        std::printf("%s\n", line.c_str());
    }
    check(got == want, std::string("the STATS lines of ") + argv[1]);
  }

  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
