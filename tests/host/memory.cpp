// contract: the DIAG MEM line (src/mullion/mullion_memory.hpp) has the fields and order docs/swtor/README.md gives,
// with frame= and allocatedMiB= as SWTOR on Mac's session report reads them from d9mt's line; MULLION-MEMORY-PRESSURE's
// rule, which the bridge applies to each sample (src/winemetal/memory_pressure.h), and its line; paging since the sample
// before, from the system's totals; MULLION_MEMSTATS's grammar; no line reaches 900 bytes; and the DIAG MEM lines of
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
    unsigned ms = 4000 + second * 1000 + second % 7 + 1;
    written.push_back("warn:  " + memory::Line(n, {20, ms / 60000, ms / 1000 % 60, ms % 1000}));
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
                    .pageouts = 3, .swapouts = 0, .heaps = 24, .heap_bytes = 1536 * kMiB};
  equal(memory::Line(n, {20, 0, 29, 6}),
        "DIAG MEM frame=1326 allocatedMiB=2400 footprintMiB=4010 footprintPeakMiB=4100 budgetMiB=110592 usageMiB=2400"
        " workingSetMiB=110592 | system: swapMiB=0 compressedMiB=812 freeMiB=91520 pageouts=3 swapouts=0"
        " | heaps=24/1536 | at=20:00:29.006");

  // the pressure rule, on a 16 GiB Mac: a swapout while the footprint is over 8 GiB
  {
    constexpr uint64_t ram = 16384 * kMiB;
    check(!memory_pressure(0, 9216 * kMiB, ram), "no swapout: no pressure");
    check(memory_pressure(1, 9216 * kMiB, ram), "a swapout over half the RAM: pressure");
    check(!memory_pressure(1, 8192 * kMiB, ram), "exactly half: no pressure");
    check(memory_pressure(1, 8192 * kMiB + 1, ram), "a byte over half: pressure");
    check(!memory_pressure(1, 9216 * kMiB, 0), "RAM unknown: no pressure");
  }

  // paging since the sample before: nothing at the first; a total that was 0 there still counts after
  {
    memory_paging last{};
    uint64_t pageouts = 99, swapouts = 99;
    memory_paging_take(&last, 0, 0, &pageouts, &swapouts);
    check(pageouts == 0 && swapouts == 0, "the first sample: nothing");
    memory_paging_take(&last, 7, 12, &pageouts, &swapouts);
    check(pageouts == 7 && swapouts == 12, "totals 0 then 7 and 12: " + std::to_string(pageouts) + " and " +
                                               std::to_string(swapouts));
    memory_paging_take(&last, 7, 12, &pageouts, &swapouts);
    check(pageouts == 0 && swapouts == 0, "unchanged: nothing");
    memory_paging last_started{};
    memory_paging_take(&last_started, 500, 40, &pageouts, &swapouts);
    check(pageouts == 0 && swapouts == 0, "a first sample with totals: nothing");
    memory_paging_take(&last_started, 510, 43, &pageouts, &swapouts);
    check(pageouts == 10 && swapouts == 3, "then the difference");
  }

  // the pressure line, from the sample the bridge kept and its time of day
  {
    check(memory::TimeOfDay(((21 * 60 + 4) * 60 + 5) * 1000 + 123).hour == 21, "21 h");
    equal(memory::PressureLine({2048 * kMiB, 9216 * kMiB, 16384 * kMiB,
                                memory::TimeOfDay(((21 * 60 + 4) * 60 + 5) * 1000 + 123)}),
          "MULLION-MEMORY-PRESSURE at=21:04:05.123 swapMiB=2048 footprintMiB=9216 ramMiB=16384");
    equal(memory::PressureLine({0, 0, 0, memory::TimeOfDay(24 * 3'600'000 - 1)}),
          "MULLION-MEMORY-PRESSURE at=23:59:59.999 swapMiB=0 footprintMiB=0 ramMiB=0");
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
    memory::Numbers full{most, most, most, most, most, most, most, most, most, most, most, most, most, most};
    memory::Clock at{99999, 99999, 99999, 99999};
    check(memory::Line(full, at).size() < 900 && memory::Line(full, at).ends_with(" | at=99:99:99.999"),
          "DIAG MEM whole and shorter than 900: " + memory::Line(full, at));
    auto pressure = memory::PressureLine({most, most, 2, at});
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
