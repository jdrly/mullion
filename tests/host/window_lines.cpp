// contract: the window lines (src/winemetal/unix/window_lines.h) have the form docs/swtor/README.md gives; a change of
// the window's visibility is d9mt's line, which SWTOR on Mac's session report finds with its own pattern, and a change
// of activation alone is a line the report doesn't take for one; a look that changes nothing writes nothing; the limit
// writes 30 lines in 10 s and the next line says how many it left out and where the window came from; and the window
// lines of tests/swtor/fixtures/session.log are what the watch writes for their Cmd-Tab away and back
#include "../../src/winemetal/unix/window_lines.h"
#include <cstdio>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static void
equal(const std::string &got, const std::string &want) {
  check(got == want, "got\n  " + got + "\nwant\n  " + want);
}

constexpr uint32_t kShown = WINDOW_VISIBLE | WINDOW_ON_ACTIVE_SPACE;

// one look, its line without the newline ("" for none)
static std::string
Look(window_watch &watch, uint32_t visibility, bool active, const char *at, const char *why, uint64_t ns = 0) {
  char line[160];
  int length = window_look(&watch, {visibility, active}, ns, at, why, line, sizeof line);
  check(length == 0 || (length < 900 && line[length - 1] == '\n'), "a whole line, under 900 bytes");
  return length ? std::string(line, length - 1) : std::string();
}

// the report's patterns (scripts/session-report.sh, jdrly/swtor-on-mac 804ff2d): a visibility line, the two values
// it reads, and the lines a limit left out
static const std::regex kReportLine("MtlPresenter: .*visibility 0x[0-9a-fA-F]+ -> 0x[0-9a-fA-F]+");
static const std::regex kReportValues("MtlPresenter: ([0-9][0-9]:[0-9][0-9]:[0-9][0-9][^ ]*) visibility 0x([0-9a-fA-F]+)"
                                      "[^>]*> 0x([0-9a-fA-F]+)");
static const std::regex kReportSuppressed("\\[([0-9]+) earlier lines suppressed");

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }

  // the first look at a window that is shown, in the active app, writes nothing: d9mt's assumption
  {
    auto watch = window_watch_start();
    equal(Look(watch, kShown, true, "20:00:00.000", "view"), "");
  }

  // a Space switch away, by hand, and what the report reads of it
  {
    auto watch = window_watch_start();
    auto line = Look(watch, WINDOW_VISIBLE, true, "20:00:34.655", "space");
    equal(line, "MtlPresenter: 20:00:34.655 visibility 0x3 -> 0x1 visible=1 activeSpace=0 miniaturized=0 active=1"
                " why=space");
    std::smatch m;
    check(std::regex_search(line, kReportLine), "the report takes it for a visibility line");
    check(std::regex_search(line, m, kReportValues) && m[1] == "20:00:34.655" && m[2] == "3" && m[3] == "1",
          "the report reads its time, 0x3 and 0x1");
    // the same state again: nothing
    equal(Look(watch, WINDOW_VISIBLE, true, "20:00:34.700", "occlusion"), "");
    // in the Dock
    equal(Look(watch, WINDOW_MINIATURIZED, true, "20:00:35.000", "miniaturize"),
          "MtlPresenter: 20:00:35.000 visibility 0x1 -> 0x4 visible=0 activeSpace=0 miniaturized=1 active=1"
          " why=miniaturize");
  }

  // the app's activation alone: the fork's line, which the report doesn't take for a visibility change
  {
    auto watch = window_watch_start();
    auto line = Look(watch, kShown, false, "20:00:34.412", "resign");
    equal(line, "mullion: window at=20:00:34.412 active=0 visibility=0x3 why=resign");
    check(!std::regex_search(line, kReportLine), "an activation alone isn't a visibility line for the report");
    // both at once: the visibility line, with the activation in it
    equal(Look(watch, 0, true, "20:00:34.500", "activate"),
          "MtlPresenter: 20:00:34.500 visibility 0x3 -> 0x0 visible=0 activeSpace=0 miniaturized=0 active=1"
          " why=activate");
  }

  // the limit: 30 lines in 10 s, the rest left out; the next line says how many and comes from where the window was
  {
    auto watch = window_watch_start();
    const uint64_t s = 1000000000ull;
    unsigned written = 0;
    for (unsigned i = 0; i < 41; i++)
      written += !Look(watch, i % 2 ? kShown : uint32_t(WINDOW_VISIBLE), true, "20:00:00.000", "occlusion", i * s / 10)
                      .empty();
    check(written == 30, "30 lines in 10 s, not " + std::to_string(written));
    // the 41st look, left out, left the window on another Space (0x1); the next, 10 s after the first, hides it
    auto line = Look(watch, 0, true, "20:00:10.000", "space", 10 * s);
    equal(line, "MtlPresenter: 20:00:10.000 visibility 0x1 -> 0x0 visible=0 activeSpace=0 miniaturized=0 active=1"
                " why=space [11 earlier lines suppressed]");
    std::smatch m;
    check(std::regex_search(line, m, kReportSuppressed) && m[1] == "11", "the report reads the 11 left out");
    equal(Look(watch, kShown, true, "20:00:10.100", "space", 10 * s + s / 10),
          "MtlPresenter: 20:00:10.100 visibility 0x0 -> 0x3 visible=1 activeSpace=1 miniaturized=0 active=1 why=space");
  }

  // an activation line after the limit says how many it left out, and leaves the count to the next visibility line:
  // the report reads only those, and a trip away and back that was left out ends where it began
  {
    auto watch = window_watch_start();
    const uint64_t s = 1000000000ull;
    for (unsigned i = 0; i < 30; i++)
      Look(watch, i % 2 ? kShown : uint32_t(WINDOW_VISIBLE), true, "20:00:00.000", "occlusion", i * s / 30);
    // the 30th left it shown; away and back, both left out
    check(Look(watch, 0, true, "20:00:01.000", "space", s).empty() &&
              Look(watch, kShown, true, "20:00:01.500", "space", s + s / 2).empty(),
          "the limit leaves out the trip away and back");
    equal(Look(watch, kShown, false, "20:00:10.000", "resign", 10 * s),
          "mullion: window at=20:00:10.000 active=0 visibility=0x3 why=resign [2 earlier lines suppressed]");
    auto line = Look(watch, WINDOW_VISIBLE, false, "20:00:10.100", "space", 10 * s + s / 10);
    equal(line, "MtlPresenter: 20:00:10.100 visibility 0x3 -> 0x1 visible=1 activeSpace=0 miniaturized=0 active=0"
                " why=space [2 earlier lines suppressed]");
    std::smatch m;
    check(std::regex_search(line, m, kReportSuppressed) && m[1] == "2",
          "the report reads the 2 left out after an activation line");
    equal(Look(watch, kShown, false, "20:00:10.200", "space", 10 * s + s / 5),
          "MtlPresenter: 20:00:10.200 visibility 0x1 -> 0x3 visible=1 activeSpace=1 miniaturized=0 active=0 why=space");
  }

  // the nextDrawable line: from 50 ms, with the watched view's visibility or unknown, and its own limit
  {
    const uint64_t ms = 1000000ull, s = 1000 * ms;
    window_limit limit{};
    auto line = [&](uint64_t waited_ns, bool got, uint32_t visibility, uint64_t now_ns) {
      char buf[160];
      int length = drawable_line(&limit, waited_ns, got, visibility, now_ns, "20:00:34.700", buf, sizeof buf);
      check(length == 0 || buf[length - 1] == '\n', "a whole nextDrawable line");
      return length ? std::string(buf, length - 1) : std::string();
    };
    equal(line(49 * ms + 999999, true, kShown, 0), "");
    check(limit.lines == 0, "a call under 50 ms takes nothing of the limit");
    equal(line(50 * ms, true, kShown, 0), "mullion: nextDrawable blocked 50 ms got=1 visibility=0x3 at=20:00:34.700");
    // a Space switch away: the window off the active Space, the drawable returned after the block
    equal(line(1149 * ms + 400000, true, WINDOW_VISIBLE, s),
          "mullion: nextDrawable blocked 1149 ms got=1 visibility=0x1 at=20:00:34.700");
    // the drawable timeout's nil, on a layer that isn't the watched view's
    equal(line(1000 * ms, false, DRAWABLE_VISIBILITY_UNKNOWN, 2 * s),
          "mullion: nextDrawable blocked 1000 ms got=0 visibility=unknown at=20:00:34.700");
    // 30 lines in 10 s; the first after them says how many were left out, and the next doesn't again
    limit = {};
    unsigned written = 0;
    for (unsigned i = 0; i < 35; i++)
      written += !line(60 * ms, true, 0, i * s / 10).empty();
    check(written == 30, "30 nextDrawable lines in 10 s, not " + std::to_string(written));
    equal(line(70 * ms, true, 0, 10 * s),
          "mullion: nextDrawable blocked 70 ms got=1 visibility=0x0 at=20:00:34.700 [5 earlier lines suppressed]");
    equal(line(80 * ms, true, 0, 10 * s), "mullion: nextDrawable blocked 80 ms got=1 visibility=0x0 at=20:00:34.700");
  }

  // the fixture: its window lines are a Cmd-Tab to an app on another Space and back, between STATS t=30 and t=31,
  // and t=33 and t=34
  {
    auto watch = window_watch_start();
    std::vector<std::string> got;
    struct {
      uint32_t visibility;
      bool active;
      const char *at, *why;
    } looks[] = {
        {kShown, false, "20:00:34.412", "resign"},
        {WINDOW_VISIBLE, false, "20:00:34.655", "space"},
        {0, false, "20:00:34.702", "occlusion"},
        {0, false, "20:00:35.120", "occlusion"}, // another window's: nothing
        {0, true, "20:00:37.301", "activate"},
        {WINDOW_ON_ACTIVE_SPACE, true, "20:00:37.517", "space"},
        {kShown, true, "20:00:37.566", "occlusion"},
    };
    for (auto &look : looks)
      if (auto line = Look(watch, look.visibility, look.active, look.at, look.why); !line.empty())
        got.push_back(line);
    std::vector<std::string> want;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.starts_with("MtlPresenter: ") || line.starts_with("mullion: window "))
        want.push_back(line);
    if (got != want) {
      std::printf("the session's lines:\n");
      for (auto &line : got)
        std::printf("%s\n", line.c_str());
    }
    check(got == want, std::string("the window lines of ") + argv[1]);
  }

  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
