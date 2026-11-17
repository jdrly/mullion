// contract: MULLION_SYNC_INTERVAL's pacing (src/mullion/mullion_present_pacing.hpp). With nothing forced, the
// interval in effect is the game's and the least time on screen is exactly what PresentFrame computed before the
// switch, for intervals 0 to 4 at 60 and 120 Hz, an unknown rate, with and without a cap; a forced interval paces
// every game interval by itself, the cap's floor kept. The switch's grammar: unset and empty are the game's, "0" to
// "4" force that interval, anything else is the game's and gets the warning; the session header prints the value in
// effect. The pacing line's format, and its limit: the first present writes, a change after a quiet second writes at
// once, changes within a second are held and the first present a second after the last line writes them with
// changes=<n>, and a present with nothing new writes nothing and reads no clock
#include "../../src/mullion/mullion_present_pacing.hpp"
#include "../../src/util/util_switch.hpp"
#include <cstdio>
#include <map>
#include <string>

using namespace mullion;
using namespace mullion::present_pacing;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

// PresentFrame's expression at 8b3fb93, as it was
static double
Old(unsigned SyncInterval, double init_refresh_rate_, int preferred_max_frame_rate) {
  return std::max(
      SyncInterval * 1.0 / (preferred_max_frame_rate ? preferred_max_frame_rate : init_refresh_rate_),
      preferred_max_frame_rate ? 1.0 / preferred_max_frame_rate : 0
  );
}

static std::map<std::string, std::string> environment;

static std::string
fromEnvironment(const char *name) {
  auto found = environment.find(name);
  return found == environment.end() ? "" : found->second;
}

// a steady clock the limiter reads, counting its reads
struct Clock {
  uint64_t ns = 0;
  unsigned reads = 0;
  uint64_t
  operator()() {
    reads++;
    return ns;
  }
};

static Fields
Game(unsigned game) {
  auto pacing = Pace(game, kNotForced, 60.0, 0);
  return {pacing.interval, game, kNotForced, 60.0, 0, pacing.after};
}

int
main() {
  // nothing forced: the game's interval, and the old duration bit for bit
  for (double rate : {60.0, 120.0, 59.94, DBL_MAX})
    for (int cap : {0, 30, 60, 144})
      for (unsigned game = 0; game <= 4; game++) {
        auto pacing = Pace(game, kNotForced, rate, cap);
        auto what = "unforced interval " + std::to_string(game) + " at " + std::to_string(rate) + " Hz, cap " +
                    std::to_string(cap);
        check(pacing.interval == game, what + ": the game's interval");
        check(pacing.after == Old(game, rate, cap), what + ": the old duration");
      }
  check(Pace(1, kNotForced, 60.0, 0).after == 1.0 / 60, "interval 1 at 60 Hz: 1/60 s");
  check(Pace(0, kNotForced, 60.0, 0).after == 0, "interval 0 without a cap: no wait");

  // forced: every game interval paced by the forced one, as the old duration of that interval
  for (int forced : {0, 1, 2})
    for (double rate : {60.0, 120.0})
      for (int cap : {0, 30})
        for (unsigned game = 0; game <= 4; game++) {
          auto pacing = Pace(game, forced, rate, cap);
          auto what = "forced " + std::to_string(forced) + " over interval " + std::to_string(game) + " at " +
                      std::to_string(rate) + " Hz, cap " + std::to_string(cap);
          check(pacing.interval == unsigned(forced), what + ": the forced interval");
          check(pacing.after == Old(forced, rate, cap), what + ": its duration");
        }
  check(Pace(0, 1, 60.0, 0).after == 1.0 / 60, "forced 1 over interval 0 at 60 Hz: 1/60 s");
  check(Pace(0, 2, 120.0, 0).after == 2.0 / 120, "forced 2 over interval 0 at 120 Hz: 1/60 s");
  check(Pace(1, 0, 60.0, 0).after == 0, "forced 0 over interval 1 without a cap: no wait");
  check(Pace(1, 0, 60.0, 30).after == 1.0 / 30, "forced 0 under a cap of 30: the cap's frame");

  // the grammar: the value in effect, the interval it forces, and whether it warns
  struct Case {
    const char *raw;
    bool set;
    const char *value;
    int forced;
    bool taken;
  } cases[] = {
      {"", false, "unset", kNotForced, true}, {"", true, "unset", kNotForced, true}, {"0", true, "0", 0, true},
      {"4", true, "4", 4, true},              {"5", true, "unset", kNotForced, false},
      {"x", true, "unset", kNotForced, false}, {"1 ", true, "unset", kNotForced, false},
      {"01", true, "unset", kNotForced, false}, {"-1", true, "unset", kNotForced, false},
  };
  for (auto &c : cases) {
    sw::Registry registry(fromEnvironment);
    environment.clear();
    if (c.set)
      environment["MULLION_SYNC_INTERVAL"] = c.raw;
    sw::Switch<std::string> sync{"MULLION_SYNC_INTERVAL", kUnset, Grammar, registry};
    auto what = std::string("MULLION_SYNC_INTERVAL ") + (c.set ? "\"" + std::string(c.raw) + "\"" : "unset");
    check(sync() == c.value, what + " is " + c.value);
    check(Forced(sync()) == c.forced, what + " forces " + std::to_string(c.forced));
    check(Taken(registry.requested(sync.name())) == c.taken, what + (c.taken ? " is taken" : " warns"));
    auto listed = registry.list();
    check(listed.size() == 1 && listed[0].first == "MULLION_SYNC_INTERVAL" && listed[0].second == c.value,
          what + ": the header prints " + c.value);
  }

  // the line
  {
    auto pacing = Pace(0, 1, 60.0, 0);
    Fields fields{pacing.interval, 0, 1, 60.0, 0, pacing.after};
    auto line = Line(fields, {9, 5, 7, 42}, 0);
    check(line == "Mullion: present pacing interval=1 game=0 forced=1 refreshHz=60.0 capFps=0 afterMs=16.7 "
                  "at=09:05:07.042",
          "the line of forced 1 at 60 Hz: " + line);
    line = Line(Game(2), {23, 59, 59, 999}, 7);
    check(line == "Mullion: present pacing interval=2 game=2 forced=no refreshHz=60.0 capFps=0 afterMs=33.3 "
                  "at=23:59:59.999 changes=7",
          "the line of the game's 2 with changes: " + line);
    Fields unknown{0, 0, kNotForced, DBL_MAX, 144, Pace(0, kNotForced, DBL_MAX, 144).after};
    line = Line(unknown, {0, 0, 0, 0}, 0);
    check(line == "Mullion: present pacing interval=0 game=0 forced=no refreshHz=unknown capFps=144 afterMs=6.9 "
                  "at=00:00:00.000",
          "the line of an unknown rate under a cap: " + line);
  }

  // the limit
  {
    Limiter limiter;
    Clock clock;
    uint64_t changes = 99;
    constexpr uint64_t ms = 1'000'000;
    check(limiter.Present(Game(0), clock, changes) && changes == 0, "the first present writes");
    unsigned reads = clock.reads;
    clock.ns = 5000 * ms;
    for (int i = 0; i < 100; i++)
      check(!limiter.Present(Game(0), clock, changes), "a present with nothing new writes nothing");
    check(clock.reads == reads, "and reads no clock");
    // a change after a quiet second: at once
    check(limiter.Present(Game(1), clock, changes) && changes == 0, "a change after a quiet second writes at once");
    // a game that alternates: held, then one line for them all
    unsigned written = 0;
    for (int i = 0; i < 10; i++) {
      clock.ns += 50 * ms;
      written += limiter.Present(Game(i % 2 ? 1 : 0), clock, changes);
    }
    check(written == 0, "changes within a second of the last line are held");
    clock.ns += 400 * ms; // 900 ms after the line
    check(!limiter.Present(Game(1), clock, changes), "and stay held until the second is over");
    clock.ns += 100 * ms; // a second after it
    check(limiter.Present(Game(1), clock, changes) && changes == 10,
          "the first present a second after the line writes the 10 held: " + std::to_string(changes));
    // held changes that return to the fields of the line are still written
    clock.ns += 10 * ms;
    check(!limiter.Present(Game(0), clock, changes), "a change right after a line is held");
    clock.ns += 10 * ms;
    check(!limiter.Present(Game(1), clock, changes), "and its way back too");
    clock.ns += 1000 * ms;
    check(limiter.Present(Game(1), clock, changes) && changes == 2, "both written a second later, as changes=2");
    clock.ns += 2000 * ms;
    check(!limiter.Present(Game(1), clock, changes), "then nothing new writes nothing");
    // a forced interval: what the game passes still changes the fields
    Limiter forced;
    auto pace = [](unsigned game) {
      auto pacing = Pace(game, 1, 60.0, 0);
      return Fields{pacing.interval, game, 1, 60.0, 0, pacing.after};
    };
    check(forced.Present(pace(0), clock, changes), "a forced swap chain's first present writes");
    clock.ns += 2000 * ms;
    check(forced.Present(pace(3), clock, changes), "and the game's interval changing writes");
  }

  if (failures)
    std::printf("%u failed\n", failures);
  return failures ? 1 : 0;
}
