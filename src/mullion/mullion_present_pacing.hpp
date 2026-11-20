#pragma once

// MULLION_SYNC_INTERVAL: the interval a Direct3D 12 swap chain paces its presents by, and the line that says which
// (docs/swtor/README.md § Switches, § Log lines):
//
//   Mullion: present pacing interval=<n> game=<n> forced=<n|no> refreshHz=<x.x|unknown> capFps=<n> afterMs=<x.x>
//     timerFps=<n> pace=<duration|wait> periodMs=<x.x> displaySync=<0|1> at=<HH:MM:SS.mmm> [changes=<n>]
//
// on one line. Pace() gives the least time a present is shown: `interval / rate`, the rate
// d3d12.preferredMaxFrameRate when set (capFps) and the display's refresh rate otherwise, and never less than one
// frame of the cap. The game's call is checked with the interval it passed; only the pacing takes the forced one.
//
// MULLION_PACE and MULLION_FPS_CAP (private ticket 49) say where that pacing happens. With MULLION_PACE=duration (the
// default) it is Metal's minimum duration (`afterMs`), as before. With MULLION_PACE=wait the game's thread waits
// before the present for the frame's deadline, `periodMs` after the one before, and Metal gets no duration
// (`afterMs=0.0`). MULLION_FPS_CAP is a deadline of its own, 1 / cap, on either path, and takes over from
// d3d12.preferredMaxFrameRate while it is set. Planned() makes the plan, Cadence the deadlines, Sleeper the wait.
//
// Pure C++ (no Windows, no Metal): tests/host/present_pacing.cpp and present_wait.cpp build it natively.

#include "mullion_stats.hpp"
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mullion::present_pacing {

// MULLION_SYNC_INTERVAL's value when nothing is forced, as the session header prints it
constexpr const char *kUnset = "unset";
// Forced()'s answer when nothing is forced
constexpr int kNotForced = -1;

// the interval a value of the switch forces: "0" to "4"; kNotForced for anything else, kUnset included
inline int
Forced(const std::string &value) {
  return value.size() == 1 && value[0] >= '0' && value[0] <= '4' ? value[0] - '0' : kNotForced;
}

// MULLION_SYNC_INTERVAL's grammar: "0" to "4" as they are; unset, empty or anything else the fallback (kUnset)
inline std::string
Grammar(const std::string &raw, const std::string &fallback) {
  return Forced(raw) != kNotForced ? raw : fallback;
}

// whether the raw text is one the switch takes, unset and empty included; anything else gets one warning
inline bool
Taken(const std::string &raw) {
  return raw.empty() || Forced(raw) != kNotForced;
}

struct Pacing {
  unsigned interval = 0; // the one in effect
  double after = 0;      // the least time on screen, in seconds, as presentDrawableAfterMinimumDuration takes it
};

// the game's interval, the forced one (kNotForced for none), the refresh rate (DBL_MAX when unknown) and the cap (0
// for none). With nothing forced, PresentFrame's expression before the switch, operand for operand
inline Pacing
Pace(unsigned game, int forced, double refresh_hz, int cap_fps) {
  unsigned interval = forced == kNotForced ? game : unsigned(forced);
  return {interval, std::max(interval * 1.0 / (cap_fps ? cap_fps : refresh_hz), cap_fps ? 1.0 / cap_fps : 0)};
}

// what a swap chain's line says
struct Fields {
  unsigned interval = 0, game = 0;
  int forced = kNotForced;
  double refresh_hz = DBL_MAX;
  int cap_fps = 0;
  double after = 0;
  // MULLION_FPS_CAP (0 for none), MULLION_PACE=wait, the game's thread's least time between deadlines in ns (0 for no
  // wait), and the layer's display sync (MULLION_DISPLAY_SYNC, where it applies)
  unsigned timer_fps = 0;
  bool wait = false;
  uint64_t period_ns = 0;
  bool display_sync = false;

  bool
  operator==(const Fields &) const = default;
};

inline std::string
Line(const Fields &fields, const stats::Clock &at, uint64_t changes) {
  char text[320];
  char refresh[24] = "unknown";
  if (fields.refresh_hz > 0 && fields.refresh_hz < 1e6)
    std::snprintf(refresh, sizeof(refresh), "%.1f", fields.refresh_hz);
  std::string forced = fields.forced == kNotForced ? "no" : std::to_string(fields.forced);
  std::snprintf(text, sizeof(text),
                "Mullion: present pacing interval=%u game=%u forced=%s refreshHz=%s capFps=%d afterMs=%.1f "
                "timerFps=%u pace=%s periodMs=%.1f displaySync=%d at=%02u:%02u:%02u.%03u",
                fields.interval, fields.game, forced.c_str(), refresh, fields.cap_fps, fields.after * 1000.0,
                fields.timer_fps, fields.wait ? "wait" : "duration", fields.period_ns / 1e6, int(fields.display_sync),
                at.hour % 100, at.minute % 100, at.second % 100, at.millisecond % 1000);
  return text + (changes ? " changes=" + std::to_string(changes) : std::string());
}

// when a swap chain writes its line: at its first present, and at a present whose fields differ from the present's
// before; at most once a second. A change within a second of the last line is held, and the first present a second
// or more after that line writes it with changes=<n>, the changes since that line. A change after a quiet second
// writes at once, without changes=. A held change waits for a present; none comes when the game stops presenting
class Limiter {
public:
  static constexpr uint64_t kSecondNs = 1'000'000'000;

  // whether this present writes the line, and `changes` for it (0 for none). `now` gives steady nanoseconds, read
  // only at a change or while one is held, so a present with nothing new reads no clock
  template <class Now>
  bool
  Present(const Fields &fields, Now &&now, uint64_t &changes) {
    changes = 0;
    if (!started_) {
      started_ = true, seen_ = fields, said_ns_ = now();
      return true;
    }
    bool changed = fields != seen_;
    if (!changed && !held_)
      return false;
    seen_ = fields;
    uint64_t at = now();
    bool quiet = at - said_ns_ >= kSecondNs;
    if (changed && !held_ && quiet) {
      said_ns_ = at;
      return true;
    }
    held_ += changed;
    if (!quiet)
      return false;
    changes = held_;
    held_ = 0, said_ns_ = at;
    return true;
  }

private:
  bool started_ = false;
  Fields seen_;          // the last present's
  uint64_t said_ns_ = 0; // the last line's time
  uint64_t held_ = 0;    // the changes since the last line, not written yet
};

// MULLION_PACE's values, as the session header prints them: "duration" (the default) hands the pacing to Metal as a
// minimum duration; "wait" makes the game's thread wait for each frame's deadline and presents with no duration
constexpr const char *kPaceDuration = "duration";
constexpr const char *kPaceWait = "wait";

// MULLION_PACE's grammar: "duration" and "wait" as they are; unset, empty or anything else the fallback
inline std::string
PaceGrammar(const std::string &raw, const std::string &fallback) {
  return raw == kPaceDuration || raw == kPaceWait ? raw : fallback;
}

// whether MULLION_PACE's raw text is one it takes, unset and empty included; anything else gets one warning
inline bool
PaceTaken(const std::string &raw) {
  return raw.empty() || PaceGrammar(raw, "") == raw;
}

// MULLION_FPS_CAP: 0 for none, else 20 to 1000 frames a second (d9mt's D9MT_FPS_CAP)
constexpr unsigned kCapMin = 20, kCapMax = 1000;

// MULLION_FPS_CAP's grammar: "0" and "20" to "1000" in at most four digits; unset, empty or anything else the fallback
inline unsigned
CapGrammar(const std::string &raw, const unsigned &fallback) {
  if (raw.empty() || raw.size() > 4 || raw.find_first_not_of("0123456789") != std::string::npos)
    return fallback;
  unsigned fps = unsigned(std::stoul(raw));
  return fps == 0 || (fps >= kCapMin && fps <= kCapMax) ? fps : fallback;
}

// whether MULLION_FPS_CAP's raw text is one it takes, unset and empty included; anything else gets one warning
inline bool
CapTaken(const std::string &raw) {
  return raw.empty() || CapGrammar(raw, kCapMax + 1) <= kCapMax;
}

// d3d12.preferredMaxFrameRate as Pace() takes it: none while MULLION_FPS_CAP caps, which takes over from it, so that
// Metal's duration (on the duration path) comes from the interval alone
inline int
CapInEffect(int preferred_fps, unsigned timer_fps) {
  return timer_fps ? 0 : preferred_fps;
}

// how a present is paced
struct Plan {
  double after = 0;       // Metal's minimum duration in seconds, 0 for a plain present
  uint64_t period_ns = 0; // the game's thread's least time from one frame's deadline to the next, 0 for no wait
};

// the plan for `pacing`, Pace()'s answer with CapInEffect's cap. The duration path keeps Pace()'s duration for Metal,
// and waits only for MULLION_FPS_CAP's deadline. The wait path gives Metal no duration and waits for the longer of
// the two: Pace()'s duration (an interval's refreshes, or d3d12.preferredMaxFrameRate's frame) and the cap's frame. An
// unknown refresh rate (DBL_MAX) makes an interval's duration a few femtoseconds, which rounds to no wait
inline Plan
Planned(const Pacing &pacing, unsigned timer_fps, bool wait) {
  uint64_t timer_ns = timer_fps ? uint64_t(1e9 / timer_fps + 0.5) : 0;
  if (!wait)
    return {pacing.after, timer_ns};
  uint64_t paced_ns = pacing.after > 0 ? uint64_t(pacing.after * 1e9 + 0.5) : 0;
  return {0, std::max(paced_ns, timer_ns)};
}

// a swap chain's deadlines: each frame's is its own period after the time the frame before it presented, which is that
// frame's deadline, or later when it came or woke late. So a late frame moves the deadlines and the frames after it
// don't catch up (d9mt's Cadence keeps its grid for a frame late by less than a period; the ticket asks for no
// catch-up), and a period that changes from one frame to the next (a game alternating its intervals under a cap)
// still waits, counted from the frame before
class Cadence {
public:
  // a present at `now` (steady ns) paced by `period` ns: the deadline to wait for, or 0 for none. The first present, and
  // the first after one with no period, wait for nothing and start the deadlines. After a wait, Woke gives the time
  uint64_t
  Next(uint64_t now, uint64_t period) {
    if (!period) {
      started_ = false;
      return 0;
    }
    if (!started_ || now >= shown_ + period) {
      started_ = true, shown_ = now;
      return 0;
    }
    shown_ += period;
    return shown_;
  }

  // the wait for Next's deadline ended at `at`: a wake past the deadline moves the deadlines after it
  void
  Woke(uint64_t at) {
    shown_ = std::max(shown_, at);
  }

private:
  // a paced present went on, and when
  bool started_ = false;
  uint64_t shown_ = 0;
};

// d9mt's Sleeper (d3d9_frame_cap.h): sleeps to a deadline with the system's sleep in halves, down to a last piece of at
// most 1 ms, then spins for as long as the last piece recently overslept, between `floor_ns` and 4 ms. macOS coalesces
// timers, and under Wine a sleep wakes late by a share of its length (d9mt measured about half on an M5 Max); the long
// first pieces' lateness is made up by the shorter ones after them. One per thread that sleeps; not thread-safe
class Sleeper {
public:
  static constexpr uint64_t kMaxSpinNs = 4'000'000;
  static constexpr uint64_t kLastNs = 1'000'000;

  explicit Sleeper(uint64_t floor_ns = 200'000) : floor_ns_(std::min(floor_ns, kMaxSpinNs)) {}

  // the spin before a deadline: the recent oversleep plus a quarter and 100 us, between the floor and kMaxSpinNs
  uint64_t
  SpinNs() const {
    uint64_t ns = late_ns_ + late_ns_ / 4 + 100'000;
    return std::min(std::max(ns, floor_ns_), kMaxSpinNs);
  }

  // from `now` until `until`, both on `clock`'s steady ns, with `sleep` the system's sleep for a number of ns. Returns
  // the time it woke at, `until` or later
  template <class Clock, class Sleep>
  uint64_t
  SleepUntil(uint64_t now, uint64_t until, Clock &&clock, Sleep &&sleep) {
    const uint64_t spin = SpinNs();
    while (now + spin < until) {
      uint64_t left = until - spin - now;
      bool last = left <= kLastNs;
      uint64_t ns = last ? left : left / 2;
      uint64_t target = now + ns;
      sleep(ns);
      now = clock();
      // what the spin has to cover: the last piece's lateness, or any piece's that overslept past the spin's start
      if (last || now + spin > until) {
        uint64_t late = now > target ? now - target : 0;
        late_ns_ = late > late_ns_ ? late : late_ns_ - (late_ns_ - late) / 64;
      }
    }
    while (now < until)
      now = clock();
    return now;
  }

private:
  uint64_t floor_ns_;
  uint64_t late_ns_ = 0;
};

} // namespace mullion::present_pacing
