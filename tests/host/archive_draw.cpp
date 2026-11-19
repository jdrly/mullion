// jdrly/mullion: contract: a pipeline made at its first draw from the archive (src/mullion/mullion_archive_draw.hpp,
// private ticket 58) counts once in ARCHIVEDRAW's `made`, and in `missed` too when a lookup of its own missed, not when
// another thread's did; nothing is counted with MULLION_STATS off; the section has the fields and order of
// docs/swtor/README.md § Log lines, says only the second's pipelines, and nothing for a second without one.
#include "../../src/mullion/mullion_archive_draw.hpp"
#include <thread>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

int
main() {
  archive_draw::Counters counters;
  archive_draw::Last last;
  check(archive_draw::Section(counters, last).empty(), "a second without a pipeline made at a draw has no section");

  // a hit, and a make whose second lookup missed (a pipeline of three Metal pipelines, one not archived)
  archive_draw::Made(true, [] {}, counters);
  archive_draw::Made(true, [] { archive_draw::Missed(); }, counters);
  // another thread's miss during this thread's make is not this make's
  archive_draw::Made(true, [] { std::thread([] { archive_draw::Missed(); }).join(); }, counters);
  check(counters.made == 3, "three pipelines made at a draw count 3");
  check(counters.missed == 1, "only the make whose own lookup missed counts as missed");

  // off: made, and nothing counted
  bool ran = false;
  archive_draw::Made(false, [&] { ran = true; archive_draw::Missed(); }, counters);
  check(ran, "with MULLION_STATS off the pipeline is still made");
  check(counters.made == 3 && counters.missed == 1, "with MULLION_STATS off nothing is counted");

  auto text = archive_draw::Section(counters, last);
  check(text.rfind("made=3 missed=1 ms=", 0) == 0, "the section says made=3 missed=1 ms=: " + text);
  check(archive_draw::Section(counters, last).empty(), "the next second, without a pipeline, has no section");
  archive_draw::Made(true, [] {}, counters);
  text = archive_draw::Section(counters, last);
  check(text.rfind("made=1 missed=0 ms=", 0) == 0, "a later second says only its own: " + text);

  if (failures)
    return 1;
  std::printf("passed\n");
  return 0;
}
