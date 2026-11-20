// jdrly/mullion: contract: a pipeline made at its first draw from the archive (src/mullion/mullion_archive_draw.hpp,
// private ticket 58) counts once in ARCHIVEDRAW's `made`, and in `missed` too when a lookup of its own missed, not when
// another thread's did; nothing is counted with MULLION_STATS off; the section has the fields and order of
// docs/swtor/README.md § Log lines, says only the second's pipelines, and nothing for a second without one; a window
// holds whole pipelines, their misses and times with them, while others are being made.
#include "../../src/mullion/mullion_archive_draw.hpp"
#include <atomic>
#include <thread>
#include <vector>

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

  // pipelines made on four threads while the windows are taken: each window holds whole pipelines, never more
  // misses than pipelines, and the windows add up to what was made
  {
    archive_draw::Counters many;
    archive_draw::Last seen;
    std::atomic<bool> done{false};
    uint64_t made = 0, missed = 0;
    bool whole = true;
    std::thread windows([&] {
      auto take = [&] {
        auto window = archive_draw::Section(many, seen);
        unsigned long long m = 0, x = 0;
        if (!window.empty() && std::sscanf(window.c_str(), "made=%llu missed=%llu", &m, &x) == 2)
          made += m, missed += x, whole = whole && x <= m;
      };
      while (!done)
        take();
      take();
    });
    std::vector<std::thread> makers;
    for (int t = 0; t < 4; t++)
      makers.emplace_back([&] {
        for (int i = 0; i < 20000; i++)
          archive_draw::Made(true, [&] { if (i % 2) archive_draw::Missed(); }, many);
      });
    for (auto &maker : makers)
      maker.join();
    done = true;
    windows.join();
    check(whole, "no window has more misses than pipelines");
    check(made == 80000 && missed == 40000, "the windows add up to the 80000 pipelines and their 40000 misses");
  }

  if (failures)
    return 1;
  std::printf("passed\n");
  return 0;
}
