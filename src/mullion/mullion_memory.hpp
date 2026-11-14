#pragma once

// The memory line and the memory-pressure line (docs/swtor/README.md § The memory lines), in the main log, at warn,
// from the thread that presents (d3d12_swapchain.cpp), from the samples of the bridge's memory sampler
// (WMTMemorySampler):
//
//   DIAG MEM frame=1326 allocatedMiB=2400 footprintMiB=4010 footprintPeakMiB=4100 budgetMiB=110592 usageMiB=2400
//     workingSetMiB=110592 | system: swapMiB=0 compressedMiB=812 freeMiB=91520 pageouts=0 swapouts=0
//     | heaps=24/1536 | at=20:00:29.006
//
//   frame               the process's Presents so far
//   allocatedMiB        Metal's currentAllocatedSize
//   footprintMiB        the process's footprint (the whole Wine process, not only Mullion), and its peak
//   budgetMiB usageMiB  what QueryVideoMemoryInfo tells the game: its budget and usage
//   workingSetMiB       Metal's recommendedMaxWorkingSetSize
//   system:             swap used, compressed and free memory; pageouts and swapouts since the sample before
//   heaps               the game's ID3D12Heaps alive: how many / their MiB
//   at                  the local time of the line
//
// with MULLION_MEMSTATS on, once a sample. frame= and allocatedMiB= are what SWTOR on Mac's session report reads from
// d9mt's line of that name (scripts/session-report.sh) and are never renamed. Always, once a process, at the first
// sample that shows pressure:
//
//   MULLION-MEMORY-PRESSURE at=21:04:05.123 swapMiB=2048 footprintMiB=9216 ramMiB=16384
//
// Pure C++ (no Windows, no Metal): tests/host/memory.cpp builds it natively.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mullion::memory {

// MULLION_MEMSTATS, read once as the DLL loads (d3d12_swapchain.cpp): the DIAG MEM line's period in seconds, 0 off
extern const unsigned seconds;
// the sampler's period without it: the pressure check's alone
constexpr unsigned kQuietSeconds = 30;

// MULLION_MEMSTATS's grammar: 0 off, 1 every 5 s, N from 2 to 3600 every N s; anything else is off
inline unsigned
Period(const std::string &raw, const unsigned &fallback) {
  if (raw.empty())
    return fallback;
  char *end = nullptr;
  unsigned long n = std::strtoul(raw.c_str(), &end, 10);
  if (*end != '\0' || raw[0] < '0' || raw[0] > '9' || n > 3600)
    return 0;
  return n == 1 ? 5 : unsigned(n);
}

// the game's ID3D12Heaps alive, counted only with MULLION_MEMSTATS on (d3d12_heap.cpp)
inline std::atomic<uint64_t> heaps{0}, heap_bytes{0};

// what a line says, in bytes, from one sample and the device at the line
struct Numbers {
  uint64_t frame = 0;
  uint64_t allocated = 0, footprint = 0, footprint_peak = 0, budget = 0, usage = 0, working_set = 0;
  uint64_t swap_used = 0, compressed = 0, free_memory = 0, pageouts = 0, swapouts = 0;
  uint64_t heaps = 0, heap_bytes = 0;
  uint64_t physical_memory = 0;
};

struct Clock {
  unsigned hour = 0, minute = 0, second = 0, millisecond = 0;
};

inline unsigned long long
MiB(uint64_t bytes) {
  return (unsigned long long)(bytes >> 20);
}

inline std::string
Line(const Numbers &n, const Clock &at) {
  char line[640];
  std::snprintf(line, sizeof(line),
                "DIAG MEM frame=%llu allocatedMiB=%llu footprintMiB=%llu footprintPeakMiB=%llu budgetMiB=%llu"
                " usageMiB=%llu workingSetMiB=%llu | system: swapMiB=%llu compressedMiB=%llu freeMiB=%llu"
                " pageouts=%llu swapouts=%llu | heaps=%llu/%llu | at=%02u:%02u:%02u.%03u",
                (unsigned long long)n.frame, MiB(n.allocated), MiB(n.footprint), MiB(n.footprint_peak), MiB(n.budget),
                MiB(n.usage), MiB(n.working_set), MiB(n.swap_used), MiB(n.compressed), MiB(n.free_memory),
                (unsigned long long)n.pageouts, (unsigned long long)n.swapouts, (unsigned long long)n.heaps,
                MiB(n.heap_bytes), at.hour % 100, at.minute % 100, at.second % 100, at.millisecond % 1000);
  return line;
}

// pressure: the system swapped out since the sample before, while the process's footprint is over half the Mac's RAM
inline bool
Pressure(const Numbers &n) {
  return n.swapouts > 0 && n.physical_memory && n.footprint > n.physical_memory / 2;
}

inline std::string
PressureLine(const Numbers &n, const Clock &at) {
  char line[160];
  std::snprintf(line, sizeof(line),
                "MULLION-MEMORY-PRESSURE at=%02u:%02u:%02u.%03u swapMiB=%llu footprintMiB=%llu ramMiB=%llu",
                at.hour % 100, at.minute % 100, at.second % 100, at.millisecond % 1000, MiB(n.swap_used),
                MiB(n.footprint), MiB(n.physical_memory));
  return line;
}

// one process's lines: the DIAG MEM line of each sample with it on, and the pressure line once
class Lines {
public:
  explicit Lines(bool line) : line_(line) {}

  // the lines of a new sample, "" for none; more than one joined with "\n"
  std::string
  Take(const Numbers &n, const Clock &at) {
    std::string text = line_ ? Line(n, at) : std::string();
    if (!pressed_ && Pressure(n)) {
      pressed_ = true;
      text += (text.empty() ? "" : "\n") + PressureLine(n, at);
    }
    return text;
  }

private:
  bool line_;
  bool pressed_ = false;
};

} // namespace mullion::memory
