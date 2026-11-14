#pragma once

// The window lines (docs/swtor/README.md § The window lines), for the bridge's window watch (window.c), and the
// nextDrawable line (§ The nextDrawable line), for its -nextDrawable; and for tests/host/window_lines.cpp. C and C++,
// no AppKit.
//
// A change of the window's visibility is d9mt's line, which SWTOR on Mac's session report reads for its hidden seconds
// and its --mark:
//     MtlPresenter: <HH:MM:SS.mmm> visibility 0x<before> -> 0x<now> visible= activeSpace= miniaturized= active= why=
// A change of the app's activation alone is the fork's own, which the report doesn't read:
//     mullion: window at=<HH:MM:SS.mmm> active= visibility=0x<now> why=
// Either ends with " [<n> earlier lines suppressed]" after the limit left lines out: the lines left out since the last
// visibility line, so that an activation line in between doesn't take the count from the report.

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

// false: leave the line out, and count it in `dropped`
static inline bool
window_limit_take(struct window_limit *limit, uint64_t now_ns) {
  if (!limit->started || now_ns - limit->start_ns >= 10000000000ull) {
    limit->started = true;
    limit->start_ns = now_ns;
    limit->lines = 0;
  }
  if (limit->lines >= 30) {
    limit->dropped++;
    return false;
  }
  limit->lines++;
  return true;
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
  if (!window_limit_take(&watch->limit, now_ns))
    return 0;
  char tail[48] = "";
  if (watch->limit.dropped)
    snprintf(tail, sizeof tail, " [%u earlier lines suppressed]", watch->limit.dropped);
  int length;
  if (was.visibility != now.visibility) {
    watch->limit.dropped = 0;
    length = snprintf(buf, size,
                      "MtlPresenter: %s visibility 0x%x -> 0x%x visible=%d activeSpace=%d miniaturized=%d active=%d"
                      " why=%s%s\n",
                      at, (unsigned)was.visibility, (unsigned)now.visibility, (now.visibility & WINDOW_VISIBLE) != 0,
                      (now.visibility & WINDOW_ON_ACTIVE_SPACE) != 0, (now.visibility & WINDOW_MINIATURIZED) != 0,
                      now.active, why, tail);
  } else
    length = snprintf(buf, size, "mullion: window at=%s active=%d visibility=0x%x why=%s%s\n", at, now.active,
                      (unsigned)now.visibility, why, tail);
  return length > 0 && (size_t)length < size ? length : 0;
}

// The nextDrawable line (docs/swtor/README.md § The nextDrawable line), for the bridge's -nextDrawable, on whichever
// thread presents:
//     mullion: nextDrawable blocked <ms> ms got=<0|1> visibility=0x<flags>|unknown at=<HH:MM:SS.mmm>
// ending with " [<n> earlier lines suppressed]" after its own limit left lines out

// a call this long or longer has a line, as d9mt's
#define DRAWABLE_BLOCKED_NS 50000000ull
// the visibility of a layer that isn't the watched view's
#define DRAWABLE_VISIBILITY_UNKNOWN UINT32_MAX

static inline bool
drawable_blocked(uint64_t waited_ns) {
  return waited_ns >= DRAWABLE_BLOCKED_NS;
}

// a call that took `waited_ns` and returned a drawable or not, `visibility` the watched view's last state or
// DRAWABLE_VISIBILITY_UNKNOWN, `at` the local time. writes the line into `buf`, newline included, and returns its
// length: 0 under the threshold or when the limit leaves it out. the caller keeps `limit` to one thread at a time
static inline int
drawable_line(struct window_limit *limit, uint64_t waited_ns, bool got, uint32_t visibility, uint64_t now_ns,
              const char *at, char *buf, size_t size) {
  if (!drawable_blocked(waited_ns) || !window_limit_take(limit, now_ns))
    return 0;
  char tail[48] = "", shown[16] = "unknown";
  if (limit->dropped)
    snprintf(tail, sizeof tail, " [%u earlier lines suppressed]", limit->dropped);
  limit->dropped = 0;
  if (visibility != DRAWABLE_VISIBILITY_UNKNOWN)
    snprintf(shown, sizeof shown, "0x%x", (unsigned)visibility);
  int length = snprintf(buf, size, "mullion: nextDrawable blocked %llu ms got=%d visibility=%s at=%s%s\n",
                        (unsigned long long)(waited_ns / 1000000), got, shown, at, tail);
  return length > 0 && (size_t)length < size ? length : 0;
}

// The present gate (docs/swtor/README.md § The present gate): a window it lets present is shown, which is d9mt's
// shown: some of it can be seen, it is on the active Space and it isn't in the Dock. A partly covered window is shown
static inline bool
window_shown(uint32_t visibility) {
  return (visibility & (WINDOW_VISIBLE | WINDOW_ON_ACTIVE_SPACE | WINDOW_MINIATURIZED)) ==
         (WINDOW_VISIBLE | WINDOW_ON_ACTIVE_SPACE);
}

// the gate's line, when it closes (`closed`) or opens again, on the presenting thread:
//     mullion: present gate closed|open visibility=0x<flags>|unknown skipped=<n> at=<HH:MM:SS.mmm>
// `skipped` 0 as it closes, the presents it skipped as it opens. under `limit`, which it shares with the nextDrawable
// line; returns the line's length as drawable_line does
static inline int
gate_line(struct window_limit *limit, bool closed, uint32_t visibility, uint64_t skipped, uint64_t now_ns,
          const char *at, char *buf, size_t size) {
  if (!window_limit_take(limit, now_ns))
    return 0;
  char tail[48] = "", shown[16] = "unknown";
  if (limit->dropped)
    snprintf(tail, sizeof tail, " [%u earlier lines suppressed]", limit->dropped);
  limit->dropped = 0;
  if (visibility != DRAWABLE_VISIBILITY_UNKNOWN)
    snprintf(shown, sizeof shown, "0x%x", (unsigned)visibility);
  int length = snprintf(buf, size, "mullion: present gate %s visibility=%s skipped=%llu at=%s%s\n",
                        closed ? "closed" : "open", shown, (unsigned long long)skipped, at, tail);
  return length > 0 && (size_t)length < size ? length : 0;
}
