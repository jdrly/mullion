// contract: with MULLION_D3D12_RESET_WAITS=1 a command allocator's Reset waits (src/mullion/mullion_reset_wait.hpp)
// until every list a queue took from it has completed, or the device is removed, and not for longer: a wait with
// nothing pending returns at once, without touching what wakes it; one with two lists pending returns after the
// second completes, not after the first; one whose lists never complete returns when the device is removed; and no
// completion that races the wait's start is lost. The first wait's MULLION-RESET-WAIT line and the RESETWAIT section
// have the fields and order of docs/swtor/README.md § Log lines.
#include "../../src/mullion/mullion_reset_wait.hpp"
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace mullion;
using namespace std::chrono_literals;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

// waits on another thread; true once it has returned. A wait that never returns can't be joined: the test ends there
struct Waiter {
  std::atomic<bool> returned{false};
  std::thread thread;

  Waiter(const std::atomic<uint32_t> &pending, reset_wait::Wakes &wakes, const std::atomic<bool> &removed)
      : thread([this, pending = &pending, wakes = &wakes, removed = &removed] {
          reset_wait::Wait(*pending, *wakes, [removed] { return removed->load(); });
          returned = true;
        }) {}

  // true when it returned within `limit`
  bool
  Joined(std::chrono::milliseconds limit, const char *what) {
    for (auto until = std::chrono::steady_clock::now() + limit; !returned && std::chrono::steady_clock::now() < until;)
      std::this_thread::sleep_for(1ms);
    if (!returned) {
      std::printf("failed: %s: the wait has not returned after %lld ms\n", what, (long long)limit.count());
      std::fflush(stdout);
      std::_Exit(1);
    }
    thread.join();
    return true;
  }
};

int
main() {
  // nothing pending: returns at once, and reads the wakes' count only
  {
    std::atomic<uint32_t> pending{0};
    reset_wait::Wakes wakes;
    std::atomic<bool> removed{false};
    reset_wait::Wait(pending, wakes, [&] { return removed.load(); });
    check(wakes.count == 0, "a wait with nothing pending leaves the wakes as they were");
  }

  // two lists pending: the first completing doesn't end the wait, the second does; only the last raises the wakes
  {
    std::atomic<uint32_t> pending{2};
    reset_wait::Wakes wakes;
    std::atomic<bool> removed{false};
    Waiter waiter(pending, wakes, removed);
    std::this_thread::sleep_for(20ms);
    check(!waiter.returned, "the wait goes on with two lists pending");
    reset_wait::Completed(pending, wakes);
    check(wakes.count == 0, "a list that isn't the last raises nothing");
    std::this_thread::sleep_for(50ms);
    check(!waiter.returned, "the wait goes on while one list is pending");
    reset_wait::Completed(pending, wakes);
    check(wakes.count == 1, "the last list raises the wakes once");
    waiter.Joined(2000ms, "two lists completed");
    check(pending == 0, "nothing is pending when the wait returns");
  }

  // a list that never completes: the device's removal ends the wait
  {
    std::atomic<uint32_t> pending{1};
    reset_wait::Wakes wakes;
    std::atomic<bool> removed{false};
    Waiter waiter(pending, wakes, removed);
    std::this_thread::sleep_for(50ms);
    check(!waiter.returned, "the wait goes on while the list is pending");
    removed = true;
    wakes.Raise();
    waiter.Joined(2000ms, "the device removed");
    check(pending == 1, "the removal ends the wait with the list still pending");
  }

  // the wait races the last completion: none is lost
  for (unsigned round = 0; round < 500; round++) {
    std::atomic<uint32_t> pending{1};
    reset_wait::Wakes wakes;
    std::atomic<bool> removed{false};
    std::thread completer([&] { reset_wait::Completed(pending, wakes); });
    Waiter waiter(pending, wakes, removed);
    completer.join();
    waiter.Joined(2000ms, "a completion racing the wait");
  }

  // the first wait's line
  {
    reset_wait::First first;
    first.at = {2, 14, 7, 512};
    first.type = "direct", first.lists = 2, first.frame = 4210, first.allocator = 1;
    auto line = reset_wait::FirstLine(first);
    check(line == "MULLION-RESET-WAIT at=02:14:07.512 type=direct lists=2 frame=4210 allocator=1", "the line: " + line);
    first.at = {23, 59, 59, 999};
    first.type = "compute", first.lists = UINT32_MAX, first.frame = UINT64_MAX, first.allocator = UINT64_MAX;
    line = reset_wait::FirstLine(first);
    check(line == "MULLION-RESET-WAIT at=23:59:59.999 type=compute lists=4294967295 frame=18446744073709551615 "
                  "allocator=18446744073709551615",
          "the line's largest numbers: " + line);
  }

  // RESETWAIT: the second's waits, their ms and the longest; nothing for a second without one
  {
    reset_wait::Counters counters;
    uint64_t waits = 0, ns = 0;
    check(reset_wait::Section(counters, waits, ns).empty(), "no section without a wait");
    counters.Add(2'000'000), counters.Add(1'500'000), counters.Add(600'000);
    auto text = reset_wait::Section(counters, waits, ns);
    check(text == "waits=3 ms=4.1 longestMs=2.0", "the section: " + text);
    check(reset_wait::Section(counters, waits, ns).empty(), "no section for the second after, without a wait");
    counters.Add(300'000);
    text = reset_wait::Section(counters, waits, ns);
    check(text == "waits=1 ms=0.3 longestMs=0.3", "the longest is the second's: " + text);
  }

  if (failures)
    return 1;
  std::printf("reset_wait: all passed\n");
  return 0;
}
