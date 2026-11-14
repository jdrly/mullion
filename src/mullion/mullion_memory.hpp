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
// Present after the first sample that showed pressure (memory_pressure.h, which the bridge applies to each sample),
// with that sample's numbers and time:
//
//   MULLION-MEMORY-PRESSURE at=21:04:05.123 swapMiB=2048 footprintMiB=9216 ramMiB=16384
//
// Pure C++ (no Windows, no Metal): tests/host/memory.cpp builds it natively.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../winemetal/memory_pressure.h"

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

// the first sample that showed pressure, as the bridge keeps it
struct Pressure {
  uint64_t swap_used = 0, footprint = 0, physical_memory = 0;
  Clock at;
};

inline std::string
PressureLine(const Pressure &p) {
  char line[160];
  std::snprintf(line, sizeof(line),
                "MULLION-MEMORY-PRESSURE at=%02u:%02u:%02u.%03u swapMiB=%llu footprintMiB=%llu ramMiB=%llu",
                p.at.hour % 100, p.at.minute % 100, p.at.second % 100, p.at.millisecond % 1000, MiB(p.swap_used),
                MiB(p.footprint), MiB(p.physical_memory));
  return line;
}

// the bridge's time of day, ms since midnight, as a Clock
inline Clock
TimeOfDay(uint32_t ms) {
  return {ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000};
}

} // namespace mullion::memory
