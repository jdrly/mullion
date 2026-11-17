#pragma once

// jdrly/mullion: how many flight dumps a process writes into its <executable>.flight (private ticket 55). Each dump
// holds every thread's last records, a few MB, and upstream's file takes every dump: a false MULLION-GPU-HUNG each few
// ms wrote 4.4 GB in a minute on the rented M1. A process writes its first kFlightDumpsAny dumps, whatever their
// reason; after them only the first dump of a reason it hasn't dumped yet, so that a removed device and the exit after
// a run of stuck command buffers are still dumped. The first dump left out writes one line (flight_cap_line). Plain
// C, so that tests/host/flight_cap.cpp runs it natively.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  kFlightDumpsAny = 8,
  // the reasons a process remembers, each by its first bytes: a reason past them counts as dumped already
  kFlightReasons = 16,
  kFlightReasonBytes = 64,
};

struct flight_cap {
  unsigned written, reasons;
  bool stopped;
  char seen[kFlightReasons][kFlightReasonBytes];
};

enum flight_cap_verdict {
  FLIGHT_CAP_WRITE,
  // left out, the first time: write flight_cap_line
  FLIGHT_CAP_STOP,
  FLIGHT_CAP_SKIP,
};

static inline enum flight_cap_verdict
flight_cap_take(struct flight_cap *cap, const char *reason) {
  bool known = false;
  for (unsigned i = 0; i < cap->reasons && !known; i++)
    known = !strncmp(cap->seen[i], reason, kFlightReasonBytes - 1);
  bool fresh = !known && cap->reasons < kFlightReasons;
  if (fresh)
    snprintf(cap->seen[cap->reasons++], kFlightReasonBytes, "%s", reason);
  if (cap->written < kFlightDumpsAny || fresh) {
    cap->written++;
    return FLIGHT_CAP_WRITE;
  }
  if (cap->stopped)
    return FLIGHT_CAP_SKIP;
  cap->stopped = true;
  return FLIGHT_CAP_STOP;
}

// the line of the first dump left out, for stderr beside "mullion: flight record of ...": the file's name alone, as
// that line's (docs/swtor/README.md § Log lines)
static inline void
flight_cap_line(char *line, size_t size, const struct flight_cap *cap, const char *reason, const char *file) {
  snprintf(line, size,
           "mullion: flight record of %s not written: %s has %u dumps, and takes only a reason not dumped yet now",
           reason, file, cap->written);
}
