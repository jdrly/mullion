// contract: MULLION_PACE, MULLION_FPS_CAP and MULLION_DISPLAY_SYNC (src/mullion/mullion_present_pacing.hpp, private
// ticket 49). The plan: unset, Metal's duration is Pace()'s and nothing waits; with MULLION_FPS_CAP, Metal's duration
// comes from the interval alone (d3d12.preferredMaxFrameRate is ignored) and the game's thread waits for the cap's
// frame; with MULLION_PACE=wait, Metal gets no duration and the game's thread waits for the longer of the interval's
// refreshes (or preferredMaxFrameRate's frame) and the cap's frame; a rate above the display's isn't held to it, and an
// unknown refresh rate waits for nothing. The deadlines: even, a period apart; a late frame, or a late wake, moves
// them and the frames after it don't catch up; a period that changes from frame to frame still waits, counted from
// the frame before. The sleep: never before the deadline, and with a system sleep that oversleeps by half, within the
// spin of it once it has seen that, alone and with the deadlines. The switches' grammars, the
// values the session header prints, and the pacing line's new fields
#include "../../src/mullion/mullion_present_pacing.hpp"
#include "../../src/util/util_switch.hpp"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace mullion;
using namespace mullion::present_pacing;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static std::map<std::string, std::string> environment;

static std::string
fromEnvironment(const char *name) {
  auto found = environment.find(name);
  return found == environment.end() ? "" : found->second;
}

constexpr uint64_t ms = 1'000'000;

// the plan PresentFrame makes: Pace() with CapInEffect's cap, then Planned
static Plan
Make(unsigned game, int forced, double rate, int preferred, unsigned timer, bool wait) {
  return Planned(Pace(game, forced, rate, CapInEffect(preferred, timer)), timer, wait);
}

// the frames a game presents: `work` ns of its own before each Present, which waits for the deadline Cadence gives.
// Returns each present's time, after its wait
static std::vector<uint64_t>
Run(Cadence &cadence, uint64_t period, const std::vector<uint64_t> &work, uint64_t start = 1000 * ms) {
  std::vector<uint64_t> presents;
  uint64_t now = start;
  for (uint64_t w : work) {
    now += w;
    if (uint64_t until = cadence.Next(now, period)) {
      check(until > now, "a deadline is never in the past");
      now = until;
      cadence.Woke(now);
    }
    presents.push_back(now);
  }
  return presents;
}

int
main() {
  // the plan, unset: Pace()'s duration for Metal, no wait
  for (double rate : {60.0, 120.0, DBL_MAX})
    for (int preferred : {0, 30, 60})
      for (unsigned game = 0; game <= 4; game++) {
        auto plan = Make(game, kNotForced, rate, preferred, 0, false);
        auto what = "unset, interval " + std::to_string(game) + " at " + std::to_string(rate) + " Hz, cap " +
                    std::to_string(preferred);
        check(plan.after == Pace(game, kNotForced, rate, preferred).after, what + ": Pace()'s duration");
        check(plan.period_ns == 0, what + ": no wait");
      }

  // MULLION_FPS_CAP on the duration path: Metal's duration from the interval alone, the cap's frame waited for
  {
    auto plan = Make(1, kNotForced, 60.0, 60, 40, false);
    check(plan.after == 1.0 / 60, "cap 40 over preferredMaxFrameRate 60, interval 1 at 60 Hz: Metal's 1/60 s");
    check(plan.period_ns == 25 * ms, "and a wait for 25 ms: " + std::to_string(plan.period_ns));
    plan = Make(0, kNotForced, 60.0, 30, 40, false);
    check(plan.after == 0, "cap 40 over preferredMaxFrameRate 30, interval 0: no duration of the preferred cap's");
    check(CapInEffect(60, 0) == 60 && CapInEffect(60, 40) == 0, "the preferred cap counts only without a timer cap");
  }

  // MULLION_PACE=wait: no duration for Metal, ever, and the longer of the two waits
  struct Wait {
    unsigned game;
    int forced;
    double rate;
    int preferred;
    unsigned timer;
    uint64_t period;
    const char *what;
  } waits[] = {
      {0, 1, 60.0, 0, 0, 16'666'667, "forced 1 at 60 Hz: a refresh"},
      {0, 2, 60.0, 0, 0, 33'333'333, "forced 2 at 60 Hz: two refreshes"},
      {1, kNotForced, 120.0, 0, 0, 8'333'333, "the game's 1 at 120 Hz"},
      {0, kNotForced, 60.0, 60, 0, 16'666'667, "preferredMaxFrameRate 60, interval 0: its frame"},
      {0, 1, 60.0, 0, 40, 25 * ms, "forced 1 under cap 40 at 60 Hz: the cap's longer frame"},
      {0, 1, 60.0, 0, 144, 16'666'667, "forced 1 under cap 144 at 60 Hz: the refresh, longer than the cap's frame"},
      {0, kNotForced, 60.0, 0, 144, 6'944'444, "cap 144 at 60 Hz, interval 0: above the display's rate, not held to it"},
      {0, kNotForced, 60.0, 0, 30, 33'333'333, "cap 30 at 60 Hz"},
      {0, kNotForced, 60.0, 0, 45, 22'222'222, "cap 45 at 60 Hz: a rate that doesn't divide the display's"},
      {0, kNotForced, 60.0, 90, 40, 25 * ms, "cap 40 over preferredMaxFrameRate 90: the cap alone"},
      {1, kNotForced, DBL_MAX, 0, 0, 0, "interval 1 at an unknown rate: no wait"},
      {0, kNotForced, 60.0, 0, 0, 0, "interval 0, nothing set: no wait"},
      {0, 0, 60.0, 0, 0, 0, "forced 0: no wait"},
  };
  for (auto &w : waits) {
    auto plan = Make(w.game, w.forced, w.rate, w.preferred, w.timer, true);
    check(plan.after == 0, std::string("wait, ") + w.what + ": no duration for Metal");
    check(plan.period_ns == w.period,
          std::string("wait, ") + w.what + ": " + std::to_string(plan.period_ns) + " ns, not " +
              std::to_string(w.period));
  }

  // the deadlines: even, a period apart, whatever each frame's own work
  {
    Cadence cadence;
    constexpr uint64_t period = 25 * ms;
    auto presents = Run(cadence, period, {3 * ms, 10 * ms, 1 * ms, 24 * ms, 7 * ms, 12 * ms, 20 * ms, 2 * ms});
    check(presents[0] == 1003 * ms, "the first present waits for nothing");
    for (size_t i = 1; i < presents.size(); i++)
      check(presents[i] - presents[i - 1] == period,
            "even deadlines: frame " + std::to_string(i) + " came " + std::to_string(presents[i] - presents[i - 1]) +
                " ns after the one before");
  }

  // a late frame: no wait, the deadlines move to a period after it, and the next frame doesn't come early
  {
    Cadence cadence;
    constexpr uint64_t period = 25 * ms;
    auto presents = Run(cadence, period, {1 * ms, 5 * ms, 5 * ms, 30 * ms, 5 * ms, 5 * ms, 5 * ms});
    check(presents[3] - presents[2] == 30 * ms, "the late frame presents when it is done: " +
                                                    std::to_string(presents[3] - presents[2]));
    for (size_t i = 4; i < presents.size(); i++)
      check(presents[i] - presents[i - 1] == period,
            "after a late frame, no catch-up: frame " + std::to_string(i) + " came " +
                std::to_string(presents[i] - presents[i - 1]) + " ns after the one before");
    // a frame exactly at its deadline is not early, and moves nothing
    Cadence exact;
    uint64_t first = exact.Next(0, period);
    uint64_t at = exact.Next(period, period);
    check(first == 0 && at == 0, "a frame at its deadline waits for nothing");
    check(exact.Next(period + ms, period) == 2 * period, "and the next deadline is a period after it");
  }

  // a period that changes from frame to frame (a game alternating intervals 0 and 2 under a cap of 40 at 60 Hz: 25 and
  // 33.3 ms) still waits, each frame its own period after the one before
  {
    Cadence cadence;
    uint64_t now = 1000 * ms, last = 0;
    for (int frame = 0; frame < 8; frame++) {
      uint64_t period = frame % 2 ? 33'333'333 : 25 * ms;
      now += 2 * ms; // the game's own work
      uint64_t until = cadence.Next(now, period);
      if (frame) {
        check(until == last + period, "alternating periods: frame " + std::to_string(frame) + " waits until " +
                                          std::to_string(until) + ", a period after the one before");
        now = until;
      }
      cadence.Woke(now);
      last = now;
    }
    check(cadence.Next(now + ms, 0) == 0 && cadence.Next(now + 2 * ms, 0) == 0, "no period: no wait");
    check(cadence.Next(now + 3 * ms, 25 * ms) == 0, "a period after none: the first present waits for nothing");
    check(cadence.Next(now + 4 * ms, 25 * ms) == now + 28 * ms, "then a period after it");
  }

  // a wake past the deadline moves the deadlines after it: the next frame isn't early
  {
    Cadence cadence;
    constexpr uint64_t period = 25 * ms;
    check(cadence.Next(1000 * ms, period) == 0, "the first present");
    uint64_t until = cadence.Next(1001 * ms, period);
    check(until == 1025 * ms, "waits until 1025 ms");
    cadence.Woke(1030 * ms); // woke 5 ms late
    check(cadence.Next(1031 * ms, period) == 1055 * ms,
          "after a wake 5 ms late, the next deadline is a period after the wake, not 1050 ms");
    // a wake on time moves nothing
    cadence.Woke(1055 * ms);
    check(cadence.Next(1056 * ms, period) == 1080 * ms, "after a wake on time, a period after the deadline");
  }

  // the cadence and the sleep together, with a system sleep that oversleeps by half: every frame a period or more
  // after the one before, and never more than the spin's step over it once the sleep has seen the oversleep
  {
    uint64_t clock = 1000 * ms;
    auto read = [&] { return clock += 1000; };
    auto sleep = [&](uint64_t ns) { clock += ns + ns / 2; };
    Cadence cadence;
    Sleeper sleeper;
    constexpr uint64_t period = 25 * ms;
    uint64_t last = 0;
    for (int frame = 0; frame < 40; frame++) {
      clock += 3 * ms;
      uint64_t now = read();
      if (uint64_t until = cadence.Next(now, period))
        cadence.Woke(now = sleeper.SleepUntil(now, until, read, sleep));
      if (frame) {
        check(now - last >= period, "frame " + std::to_string(frame) + " came " + std::to_string(now - last) +
                                        " ns after the one before: never early");
        if (frame >= 4)
          check(now - last <= period + 2000, "frame " + std::to_string(frame) + " came " +
                                                 std::to_string(now - last) + " ns after the one before");
      }
      last = now;
    }
  }

  // the sleep, against a clock that a system sleep moves by 1.5 times what it was asked for (Wine on macOS: about
  // half late) and every other read by 1 us (the spin)
  {
    uint64_t clock = 0;
    unsigned sleeps = 0;
    auto read = [&] { return clock += 1000; };
    auto sleep = [&](uint64_t ns) { clock += ns + ns / 2, sleeps++; };
    Sleeper sleeper;
    uint64_t worst_late = 0, last_late = 0;
    for (int frame = 0; frame < 40; frame++) {
      uint64_t until = clock + 25 * ms;
      uint64_t woke = sleeper.SleepUntil(clock, until, read, sleep);
      check(woke >= until, "frame " + std::to_string(frame) + ": never woken before the deadline");
      uint64_t late = woke - until;
      worst_late = std::max(worst_late, late);
      if (frame >= 3)
        last_late = std::max(last_late, late);
    }
    check(sleeps > 40, "it sleeps in pieces: " + std::to_string(sleeps));
    check(last_late <= 2000, "once it has seen the oversleep, the spin covers it: " + std::to_string(last_late) +
                                 " ns past the deadline at most");
    check(worst_late < 1 * ms, "even at first, less than a millisecond late: " + std::to_string(worst_late));
    // an exact system sleep: the spin stays at its floor
    Sleeper exact;
    uint64_t t = 0;
    auto exact_read = [&] { return t += 1000; };
    auto exact_sleep = [&](uint64_t ns) { t += ns; };
    exact.SleepUntil(0, 25 * ms, exact_read, exact_sleep);
    check(exact.SpinNs() == 200'000, "an exact sleep spins at the floor: " + std::to_string(exact.SpinNs()));
    // a deadline within the spin: no system sleep at all
    unsigned none = 0;
    uint64_t u = 0;
    exact.SleepUntil(0, 100'000, [&] { return u += 1000; }, [&](uint64_t) { none++; });
    check(none == 0, "a deadline closer than the spin only spins");
  }

  // MULLION_PACE's grammar
  struct PaceCase {
    const char *raw;
    bool set;
    const char *value;
    bool taken;
  } paces[] = {
      {"", false, "duration", true}, {"", true, "duration", true},   {"duration", true, "duration", true},
      {"wait", true, "wait", true},  {"Wait", true, "duration", false}, {"1", true, "duration", false},
      {"wait ", true, "duration", false},
  };
  for (auto &c : paces) {
    sw::Registry registry(fromEnvironment);
    environment.clear();
    if (c.set)
      environment["MULLION_PACE"] = c.raw;
    sw::Switch<std::string> pace{"MULLION_PACE", kPaceDuration, PaceGrammar, registry};
    auto what = std::string("MULLION_PACE ") + (c.set ? "\"" + std::string(c.raw) + "\"" : "unset");
    check(pace() == c.value, what + " is " + c.value);
    check(PaceTaken(registry.requested(pace.name())) == c.taken, what + (c.taken ? " is taken" : " warns"));
    auto listed = registry.list();
    check(listed.size() == 1 && listed[0].second == c.value, what + ": the header prints " + c.value);
  }

  // MULLION_FPS_CAP's grammar
  struct CapCase {
    const char *raw;
    bool set;
    unsigned value;
    bool taken;
  } caps[] = {
      {"", false, 0, true},     {"", true, 0, true},       {"0", true, 0, true},      {"20", true, 20, true},
      {"40", true, 40, true},   {"1000", true, 1000, true}, {"0040", true, 40, true}, {"19", true, 0, false},
      {"1001", true, 0, false}, {"40x", true, 0, false},    {"-40", true, 0, false},  {" 40", true, 0, false},
      {"00040", true, 0, false}, {"40.0", true, 0, false},
  };
  for (auto &c : caps) {
    sw::Registry registry(fromEnvironment);
    environment.clear();
    if (c.set)
      environment["MULLION_FPS_CAP"] = c.raw;
    sw::Switch<unsigned> cap{"MULLION_FPS_CAP", 0, CapGrammar, registry};
    auto what = std::string("MULLION_FPS_CAP ") + (c.set ? "\"" + std::string(c.raw) + "\"" : "unset");
    check(cap() == c.value, what + " is " + std::to_string(c.value));
    check(CapTaken(registry.requested(cap.name())) == c.taken, what + (c.taken ? " is taken" : " warns"));
    auto listed = registry.list();
    check(listed.size() == 1 && listed[0].second == std::to_string(c.value),
          what + ": the header prints " + std::to_string(c.value));
  }

  // MULLION_DISPLAY_SYNC: on for exactly "1"
  for (auto [raw, on] : std::vector<std::pair<const char *, bool>>{{"", false}, {"1", true}, {"0", false},
                                                                  {"true", false}}) {
    sw::Registry registry(fromEnvironment);
    environment.clear();
    environment["MULLION_DISPLAY_SYNC"] = raw;
    sw::Switch<bool> sync{"MULLION_DISPLAY_SYNC", false, sw::exactlyOne, registry};
    check(sync() == on, std::string("MULLION_DISPLAY_SYNC \"") + raw + "\" is " + (on ? "on" : "off"));
  }

  // the line's new fields
  {
    auto pacing = Pace(0, 1, 60.0, CapInEffect(60, 40));
    auto plan = Planned(pacing, 40, true);
    Fields fields{pacing.interval, 0, 1, 60.0, 60, plan.after, 40, true, plan.period_ns, true};
    auto line = Line(fields, {9, 5, 7, 42}, 0);
    check(line == "Mullion: present pacing interval=1 game=0 forced=1 refreshHz=60.0 capFps=60 afterMs=0.0 "
                  "timerFps=40 pace=wait periodMs=25.0 displaySync=1 at=09:05:07.042",
          "the line of the wait path under a cap of 40: " + line);
    pacing = Pace(1, kNotForced, 60.0, 0);
    plan = Planned(pacing, 0, false);
    Fields old{pacing.interval, 1, kNotForced, 60.0, 0, plan.after, 0, false, plan.period_ns, false};
    line = Line(old, {0, 0, 0, 0}, 0);
    check(line == "Mullion: present pacing interval=1 game=1 forced=no refreshHz=60.0 capFps=0 afterMs=16.7 "
                  "timerFps=0 pace=duration periodMs=0.0 displaySync=0 at=00:00:00.000",
          "the line of the duration path: " + line);
    // a change of the new fields is a change the line writes
    Limiter limiter;
    uint64_t now = 0, changes = 0;
    auto clock = [&] { return now; };
    check(limiter.Present(old, clock, changes), "the first present writes");
    now += 2000 * ms;
    Fields waiting = old;
    waiting.wait = true;
    check(limiter.Present(waiting, clock, changes), "a change of the path writes");
  }

  if (failures)
    std::printf("%u failed\n", failures);
  return failures ? 1 : 0;
}
