#pragma once

// MULLION_SYNC_INTERVAL: the interval a Direct3D 12 swap chain paces its presents by, and the line that says which
// (docs/swtor/README.md § Switches, § Log lines):
//
//   Mullion: present pacing interval=<n> game=<n> forced=<n|no> refreshHz=<x.x|unknown> capFps=<n> afterMs=<x.x>
//     at=<HH:MM:SS.mmm> [changes=<n>]
//
// on one line. A present is shown for at least `afterMs`: `interval / rate`, the rate d3d12.preferredMaxFrameRate
// when set (capFps) and the display's refresh rate otherwise, and never less than one frame of the cap. The game's
// call is checked with the interval it passed; only the pacing takes the forced one.
//
// Pure C++ (no Windows, no Metal): tests/host/present_pacing.cpp builds it natively.

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

  bool
  operator==(const Fields &) const = default;
};

inline std::string
Line(const Fields &fields, const stats::Clock &at, uint64_t changes) {
  char text[256];
  char refresh[24] = "unknown";
  if (fields.refresh_hz > 0 && fields.refresh_hz < 1e6)
    std::snprintf(refresh, sizeof(refresh), "%.1f", fields.refresh_hz);
  std::string forced = fields.forced == kNotForced ? "no" : std::to_string(fields.forced);
  std::snprintf(text, sizeof(text),
                "Mullion: present pacing interval=%u game=%u forced=%s refreshHz=%s capFps=%d afterMs=%.1f "
                "at=%02u:%02u:%02u.%03u",
                fields.interval, fields.game, forced.c_str(), refresh, fields.cap_fps, fields.after * 1000.0,
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

} // namespace mullion::present_pacing
