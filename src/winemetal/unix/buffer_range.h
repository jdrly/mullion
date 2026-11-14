#pragma once

// jdrly/mullion: the bytes a buffer of `length` bytes has from `offset`, 0 from its end on, never a difference that
// wraps to nearly 2^64 for an offset past the end, which Metal 4 would take as a range's length (private ticket 20,
// part 2). Plain C, so that tests/host/ranges.cpp builds it natively.

#include <stdint.h>

static inline uint64_t
buffer_remaining(uint64_t length, uint64_t offset) {
  return offset < length ? length - offset : 0;
}
