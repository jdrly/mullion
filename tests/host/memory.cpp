// contract: the DIAG MEM line (src/mullion/mullion_memory.hpp) has the fields and order docs/swtor/README.md gives,
// with frame= and allocatedMiB= as SWTOR on Mac's session report reads them from d9mt's line; MULLION-MEMORY-PRESSURE
// comes once a process, at the first sample that swapped out while the footprint was over half the RAM, whether the
// DIAG MEM line is on or not; MULLION_MEMSTATS's grammar; no line reaches 900 bytes; and the DIAG MEM lines of
// tests/swtor/fixtures/session.log are what the sampler's samples of its session write
#include "../../src/mullion/mullion_memory.hpp"
#include <fstream>
#include <limits>
#include <vector>

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

constexpr uint64_t kMiB = 1 << 20;

// the fixture's session (tests/host/stats.cpp writes its STATS lines): with MULLION_MEMSTATS=1 a sample every 5 s,
// written at the Present after that second's STATS line. Metal's allocation as STATS has it, the footprint 1610 MiB
// more, on a 128 GiB M5 Max that never swaps
static std::vector<std::string>
Session() {
  memory::Lines lines(true);
  std::vector<std::string> written;
  uint64_t peak = 0;
  for (unsigned second = 5; second <= 60; second += 5) {
    unsigned settled = std::min(second, 30u);
    memory::Numbers n;
    n.frame = 1 + 25 * std::min(second, 5u) + 60 * (second > 5 ? second - 5 : 0);
    n.allocated = n.usage = (1800ull + settled * 20 - (second > 50 ? 15 : 0)) * kMiB;
    n.footprint = n.allocated + 1610 * kMiB;
    n.footprint_peak = peak = std::max(peak, n.footprint);
    n.budget = n.working_set = 110592 * kMiB;
    n.compressed = 812 * kMiB;
    n.free_memory = (92000ull - settled * 20) * kMiB;
    n.heaps = settled / 5 * 4;
    n.heap_bytes = n.heaps * 64 * kMiB;
    n.physical_memory = 131072 * kMiB;
    unsigned ms = 4000 + second * 1000 + second % 7 + 1;
    written.push_back("warn:  " + lines.Take(n, {20, ms / 60000, ms / 1000 % 60, ms % 1000}));
  }
  return written;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }

  // the line, by hand
  memory::Numbers n{.frame = 1326, .allocated = 2400 * kMiB + 512 * 1024, .footprint = 4010 * kMiB,
                    .footprint_peak = 4100 * kMiB, .budget = 110592 * kMiB, .usage = 2400 * kMiB,
                    .working_set = 110592 * kMiB, .swap_used = 0, .compressed = 812 * kMiB, .free_memory = 91520 * kMiB,
                    .pageouts = 3, .swapouts = 0, .heaps = 24, .heap_bytes = 1536 * kMiB,
                    .physical_memory = 131072 * kMiB};
  equal(memory::Line(n, {20, 0, 29, 6}),
        "DIAG MEM frame=1326 allocatedMiB=2400 footprintMiB=4010 footprintPeakMiB=4100 budgetMiB=110592 usageMiB=2400"
        " workingSetMiB=110592 | system: swapMiB=0 compressedMiB=812 freeMiB=91520 pageouts=3 swapouts=0"
        " | heaps=24/1536 | at=20:00:29.006");

  // the pressure rule, on a 16 GiB Mac: a swapout while the footprint is over 8 GiB
  {
    memory::Numbers mac{.footprint = 9216 * kMiB, .swap_used = 2048 * kMiB, .swapouts = 0,
                        .physical_memory = 16384 * kMiB};
    check(!memory::Pressure(mac), "no swapout: no pressure");
    mac.swapouts = 1;
    check(memory::Pressure(mac), "a swapout over half the RAM: pressure");
    mac.footprint = 8192 * kMiB;
    check(!memory::Pressure(mac), "exactly half: no pressure");
    mac.footprint = 8192 * kMiB + 1;
    check(memory::Pressure(mac), "a byte over half: pressure");
    mac.physical_memory = 0;
    check(!memory::Pressure(mac), "RAM unknown: no pressure");
  }

  // the line on: DIAG MEM each sample, the pressure line once, at the first sample with it
  {
    memory::Lines lines(true);
    memory::Numbers mac{.frame = 90000, .footprint = 9216 * kMiB, .swap_used = 2048 * kMiB, .swapouts = 0,
                        .physical_memory = 16384 * kMiB};
    std::string text = lines.Take(mac, {21, 4, 0, 123});
    check(text.starts_with("DIAG MEM frame=90000 ") && text.find('\n') == std::string::npos,
          "no pressure yet: DIAG MEM alone: " + text);
    mac.swapouts = 12;
    text = lines.Take(mac, {21, 4, 5, 123});
    check(text.starts_with("DIAG MEM frame=90000 "), "DIAG MEM first: " + text);
    check(text.ends_with("\nMULLION-MEMORY-PRESSURE at=21:04:05.123 swapMiB=2048 footprintMiB=9216 ramMiB=16384"),
          "then the pressure line: " + text);
    text = lines.Take(mac, {21, 4, 10, 123});
    check(text.find("PRESSURE") == std::string::npos, "once a process: " + text);
  }

  // the line off: the pressure line alone, once
  {
    memory::Lines lines(false);
    memory::Numbers mac{.footprint = 9216 * kMiB, .swap_used = 2048 * kMiB, .swapouts = 0,
                        .physical_memory = 16384 * kMiB};
    check(lines.Take(mac, {21, 4, 0, 123}).empty(), "off and no pressure: nothing");
    mac.swapouts = 1;
    equal(lines.Take(mac, {21, 4, 30, 123}),
          "MULLION-MEMORY-PRESSURE at=21:04:30.123 swapMiB=2048 footprintMiB=9216 ramMiB=16384");
    check(lines.Take(mac, {21, 5, 0, 123}).empty(), "off, after the pressure line: nothing");
  }

  // MULLION_MEMSTATS: 0 off, 1 every 5 s, N every N s up to an hour; anything else off
  {
    struct Case {
      const char *raw;
      unsigned want;
    } cases[] = {{"", 0}, {"0", 0}, {"1", 5}, {"2", 2}, {"10", 10}, {"3600", 3600}, {"3601", 0},
                 {"-1", 0}, {" 5", 0}, {"5s", 0}, {"on", 0}, {"99999999999999999999", 0}};
    for (auto &c : cases)
      check(memory::Period(c.raw, 0) == c.want,
            std::string("MULLION_MEMSTATS=\"") + c.raw + "\": " + std::to_string(memory::Period(c.raw, 0)));
  }

  // no line reaches 900 bytes, whatever the numbers
  {
    constexpr uint64_t most = std::numeric_limits<uint64_t>::max();
    memory::Numbers full{most, most, most, most, most, most, most, most, most, most, most, most, most, most, most};
    memory::Clock at{99999, 99999, 99999, 99999};
    check(memory::Line(full, at).size() < 900 && memory::Line(full, at).ends_with(" | at=99:99:99.999"),
          "DIAG MEM whole and shorter than 900: " + memory::Line(full, at));
    full.physical_memory = 2;
    auto pressure = memory::PressureLine(full, at);
    check(pressure.size() < 900 && pressure.ends_with(" ramMiB=0"), "the pressure line whole: " + pressure);
  }

  // the fixture: its DIAG MEM lines, the report's keys first
  {
    std::vector<std::string> want;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.find(" DIAG MEM frame=") != std::string::npos)
        want.push_back(line);
    auto got = Session();
    check(got.size() == 12, "twelve samples");
    if (got != want) {
      std::printf("the session's lines:\n");
      for (auto &line : got)
        std::printf("%s\n", line.c_str());
    }
    check(got == want, std::string("the DIAG MEM lines of ") + argv[1]);
  }

  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
