#pragma once

// The window lines (docs/swtor/README.md § The window lines), for the bridge's window watch (window.c) and
// tests/host/window_lines.cpp. C and C++, no AppKit.
//
// A change of the window's visibility is d9mt's line, which SWTOR on Mac's session report reads for its hidden seconds
// and its --mark:
//     MtlPresenter: <HH:MM:SS.mmm> visibility 0x<before> -> 0x<now> visible= activeSpace= miniaturized= active= why=
// A change of the app's activation alone is the fork's own, which the report doesn't read:
//     mullion: window at=<HH:MM:SS.mmm> active= visibility=0x<now> why=
// Either ends with " [<n> earlier lines suppressed]" after the limit left lines out.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// d9mt's bits (its WMT_VISIBILITY_*), as the report reads them: shown is visible, on the active Space, not in the Dock
enum {
  WINDOW_VISIBLE = 0x1,         // some of the window can be seen (NSWindowOcclusionStateVisible)
  WINDOW_ON_ACTIVE_SPACE = 0x2, // isOnActiveSpace
  WINDOW_MINIATURIZED = 0x4,    // isMiniaturized
};

struct window_state {
  uint32_t visibility;
  bool active; // the app is the active one (NSApp.isActive)
};

// at most 30 lines in 10 s, as MULLION-GPU-ERROR's limit and d9mt's
struct window_limit {
  bool started;
  uint64_t start_ns;
  unsigned lines, dropped;
};

// what the watch said last, and its limit. before the first look: shown and active, as d9mt assumes, so a process
// that starts shown writes nothing
struct window_watch {
  struct window_state last;
  struct window_limit limit;
};

static inline struct window_watch
window_watch_start(void) {
  struct window_watch watch = {{WINDOW_VISIBLE | WINDOW_ON_ACTIVE_SPACE, true}, {false, 0, 0, 0}};
  return watch;
}

// -1: leave the line out; else how many lines it left out since the last one written
static inline int
window_limit_take(struct window_limit *limit, uint64_t now_ns) {
  if (!limit->started || now_ns - limit->start_ns >= 10000000000ull) {
    limit->started = true;
    limit->start_ns = now_ns;
    limit->lines = 0;
  }
  if (limit->lines >= 30) {
    limit->dropped++;
    return -1;
  }
  limit->lines++;
  int dropped = (int)limit->dropped;
  limit->dropped = 0;
  return dropped;
}

// one look at the window, `now` its state, `at` the local time, `why` what AppKit said. writes the line into `buf`,
// newline included, and returns its length: 0 when nothing changed or the limit leaves it out. `last` follows the
// window either way, so the line after a gap says where it came from
static inline int
window_look(struct window_watch *watch, struct window_state now, uint64_t now_ns, const char *at, const char *why,
            char *buf, size_t size) {
  struct window_state was = watch->last;
  if (was.visibility == now.visibility && was.active == now.active)
    return 0;
  watch->last = now;
  int dropped = window_limit_take(&watch->limit, now_ns);
  if (dropped < 0)
    return 0;
  char tail[48] = "";
  if (dropped)
    snprintf(tail, sizeof tail, " [%d earlier lines suppressed]", dropped);
  int length;
  if (was.visibility != now.visibility)
    length = snprintf(buf, size,
                      "MtlPresenter: %s visibility 0x%x -> 0x%x visible=%d activeSpace=%d miniaturized=%d active=%d"
                      " why=%s%s\n",
                      at, (unsigned)was.visibility, (unsigned)now.visibility, (now.visibility & WINDOW_VISIBLE) != 0,
                      (now.visibility & WINDOW_ON_ACTIVE_SPACE) != 0, (now.visibility & WINDOW_MINIATURIZED) != 0,
                      now.active, why, tail);
  else
    length = snprintf(buf, size, "mullion: window at=%s active=%d visibility=0x%x why=%s%s\n", at, now.active,
                      (unsigned)now.visibility, why, tail);
  return length > 0 && (size_t)length < size ? length : 0;
}
