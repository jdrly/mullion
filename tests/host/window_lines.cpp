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
