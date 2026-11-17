// jdrly/mullion: contract: mullion::condition_variable (src/util/thread.hpp, the Win32 one) waits as
// std::condition_variable does (private ticket 55). A wait with a predicate returns false only once its time is up,
// whatever SleepConditionVariableSRW does before: a wakeup with the predicate still false (Wine's can wake with nothing
// notified) and a return before the time asked for both wait again, for what is left of it. A wait until a time asks
// for the time left, never its negative, and one until a time already past returns at once.
//
// The class is copied out of thread.hpp at build time (condition_variable.py) and runs against the scripted
// SleepConditionVariableSRW below: each call takes the next of the script's acts and records the milliseconds it was
// asked for. The waits until a time run on a clock of the test's, which only a sleep moves; the waits for a duration
// run on steady_clock, and the script sleeps for real.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

typedef int BOOL;
typedef uint32_t DWORD;
typedef unsigned long ULONG;
#define INFINITE 0xFFFFFFFFu
struct SRWLOCK {
  int unused;
};
typedef SRWLOCK *PSRWLOCK;
struct CONDITION_VARIABLE {
  int unused;
};
typedef CONDITION_VARIABLE *PCONDITION_VARIABLE;

// the test's clock: only a sleep of the script moves it
struct TestClock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<TestClock>;
  static constexpr bool is_steady = true;
  static inline time_point at{};
  static time_point
  now() {
    return at;
  }
};

enum class Act {
  Spurious, // returns TRUE at once, nothing notified
  Early,    // sleeps half of what it was asked for and returns FALSE, as a timeout
  Notify,   // the predicate's flag set, TRUE at once
  Full,     // sleeps what it was asked for and returns FALSE
};

static struct {
  std::deque<Act> acts;
  std::vector<DWORD> asked;
  bool real = false;
  bool *flag = nullptr;
} script;

static void
Slept(DWORD ms) {
  TestClock::at += std::chrono::milliseconds(ms);
  if (script.real)
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void
InitializeConditionVariable(PCONDITION_VARIABLE) {}
static void
WakeConditionVariable(PCONDITION_VARIABLE) {}
static void
WakeAllConditionVariable(PCONDITION_VARIABLE) {}

static BOOL
SleepConditionVariableSRW(PCONDITION_VARIABLE, PSRWLOCK, DWORD ms, ULONG) {
  script.asked.push_back(ms);
  // a wait the script didn't plan for: its whole time, which a negative one asks as about 49 days
  Act act = Act::Full;
  if (!script.acts.empty())
    act = script.acts.front(), script.acts.pop_front();
  switch (act) {
  case Act::Spurious:
    return 1;
  case Act::Early:
    Slept(ms / 2);
    return 0;
  case Act::Notify:
    *script.flag = true;
    return 1;
  case Act::Full:
    // nothing in this test asks for a minute: only a time's negative does, and the test ends there
    if (ms > 60'000) {
      std::printf("failed: a wait asks for %u ms\n", unsigned(ms));
      std::exit(1);
    }
    Slept(ms);
    return 0;
  }
  return 0;
}

namespace mullion {
class mutex {
public:
  using native_handle_type = PSRWLOCK;
  void lock() {}
  void unlock() {}
  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = {};
};
#include "condition_variable.inc"
} // namespace mullion

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static std::string
Asked() {
  std::string out = "asked";
  for (auto ms : script.asked)
    out += " " + std::to_string(ms);
  return out;
}

// a fresh script, the test's clock at an hour
static void
Script(std::deque<Act> acts, bool real = false, bool *flag = nullptr) {
  script.acts = std::move(acts), script.asked.clear(), script.real = real, script.flag = flag;
  TestClock::at = TestClock::time_point(1h);
}

int
main() {
  mullion::mutex mutex;
  std::unique_lock<mullion::mutex> lock(mutex);
  mullion::condition_variable cv;
  bool flag = false;
  auto pred = [&] { return flag; };

  // until a time: the time left, once, and a timeout
  Script({Act::Full});
  auto status = cv.wait_until(lock, TestClock::now() + 100ms);
  std::printf("until 100 ms ahead: %s, %s\n", status == std::cv_status::timeout ? "timeout" : "no timeout",
              Asked().c_str());
  check(script.asked == std::vector<DWORD>{100}, "a wait until 100 ms ahead asks for 100 ms, not -100");
  check(status == std::cv_status::timeout, "a wait until a time that has come is a timeout");

  // until a time past: at once
  Script({});
  status = cv.wait_until(lock, TestClock::now() - 1s);
  check(script.asked.empty() && status == std::cv_status::timeout, "a wait until a time past returns at once");
  flag = false;
  Script({});
  check(!cv.wait_until(lock, TestClock::now() - 1s, pred) && script.asked.empty(),
        "a wait with a predicate until a time past returns false at once");
  flag = true;
  check(cv.wait_until(lock, TestClock::now() - 1s, pred), "a wait until a time past returns a true predicate");

  // until a time, with a predicate: wakeups with nothing notified and a return before the time wait again
  flag = false;
  Script({Act::Spurious, Act::Spurious, Act::Early, Act::Spurious, Act::Full});
  auto start = TestClock::now();
  bool result = cv.wait_until(lock, start + 100ms, pred);
  std::printf("until 100 ms ahead, woken 3 times and once early: %s after %lld ms, %s\n", result ? "true" : "false",
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(TestClock::now() - start).count(),
              Asked().c_str());
  check(!result, "a predicate never true gives false");
  check(TestClock::now() - start == 100ms, "a wait until a time returns false only once it has come");
  check(script.asked == std::vector<DWORD>({100, 100, 100, 50, 50}), "each wait asks for the time left");

  // ... and the predicate true at the third wakeup
  flag = false;
  Script({Act::Spurious, Act::Spurious, Act::Notify}, false, &flag);
  check(cv.wait_until(lock, TestClock::now() + 100ms, pred) && script.asked.size() == 3,
        "a predicate made true after two wakeups with nothing notified gives true at the third");

  // for a duration, with a predicate, on steady_clock: the same, in real time
  flag = false;
  Script({Act::Spurious, Act::Spurious, Act::Spurious}, true);
  auto real = std::chrono::steady_clock::now();
  result = cv.wait_for(lock, 80ms, pred);
  auto waited = std::chrono::steady_clock::now() - real;
  std::printf("for 80 ms, woken 3 times: %s after %lld ms, %s\n", result ? "true" : "false",
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(), Asked().c_str());
  check(!result && waited >= 80ms, "a wait for 80 ms woken with nothing notified returns false only after 80 ms");
  for (auto ms : script.asked)
    check(ms <= 80, "no wait for 80 ms asks for more than 80 ms");

  flag = false;
  Script({Act::Early, Act::Early}, true);
  real = std::chrono::steady_clock::now();
  result = cv.wait_for(lock, 80ms, pred);
  waited = std::chrono::steady_clock::now() - real;
  std::printf("for 80 ms, returned early twice: %s after %lld ms, %s\n", result ? "true" : "false",
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(), Asked().c_str());
  check(!result && waited >= 80ms, "a wait for 80 ms returned early returns false only after 80 ms");

  flag = false;
  Script({Act::Spurious, Act::Notify}, true, &flag);
  check(cv.wait_for(lock, 10s, pred) && script.asked.size() == 2,
        "a wait for a duration gives true once the predicate is, before its time");

  if (failures)
    std::printf("FAILED: %u checks\n", failures);
  else
    std::printf("ok\n");
  return failures ? 1 : 0;
}
