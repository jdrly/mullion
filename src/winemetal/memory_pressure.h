#pragma once

// MULLION-MEMORY-PRESSURE's rule (docs/swtor/README.md § The memory lines), for the bridge's memory sampler, which
// applies it to each sample, and tests/host/memory.cpp. C and C++, no Windows, no Metal.
//
// pressure: the system swapped out since the sample before, while the process's footprint is over half the Mac's RAM.
// No rule without the RAM (0)

#include <stdbool.h>
#include <stdint.h>

static inline bool
memory_pressure(uint64_t swapouts, uint64_t footprint, uint64_t physical_memory) {
  return swapouts > 0 && physical_memory && footprint > physical_memory / 2;
}

// paging since the reader's sample before, from the system's totals; 0 at its first
struct memory_paging {
  bool started;
  uint64_t pageouts, swapouts;
};

static inline void
memory_paging_take(struct memory_paging *last, uint64_t pageouts_total, uint64_t swapouts_total, uint64_t *pageouts,
                   uint64_t *swapouts) {
  *pageouts = last->started ? pageouts_total - last->pageouts : 0;
  *swapouts = last->started ? swapouts_total - last->swapouts : 0;
  last->started = true;
  last->pageouts = pageouts_total;
  last->swapouts = swapouts_total;
}
