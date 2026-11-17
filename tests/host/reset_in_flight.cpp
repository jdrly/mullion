// contract: a command allocator's Reset with lists in flight (src/mullion/mullion_reset_in_flight.hpp) writes the
// MULLION-RESET-IN-FLIGHT line and the RESETINFLIGHT section with the fields and order of docs/swtor/README.md § Log
// lines: the line's numbers in full, and the section the second's Resets and lists only, nothing for a second without.
#include "../../src/mullion/mullion_reset_in_flight.hpp"

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

int
main() {
  // the first Reset's line
  {
    reset_in_flight::First first;
    first.at = {2, 14, 7, 512};
    first.type = "direct", first.lists = 2, first.frame = 4210, first.allocator = 1;
    auto line = reset_in_flight::FirstLine(first);
    check(line == "MULLION-RESET-IN-FLIGHT at=02:14:07.512 type=direct lists=2 frame=4210 allocator=1",
          "the line: " + line);
    first.at = {23, 59, 59, 999};
    first.type = "compute", first.lists = UINT32_MAX, first.frame = UINT64_MAX, first.allocator = UINT64_MAX;
    line = reset_in_flight::FirstLine(first);
    check(line == "MULLION-RESET-IN-FLIGHT at=23:59:59.999 type=compute lists=4294967295 frame=18446744073709551615 "
                  "allocator=18446744073709551615",
          "the line's largest numbers: " + line);
  }

  // RESETINFLIGHT: the second's Resets and the lists they handed on; nothing for a second without one
  {
    reset_in_flight::Counters counters;
    uint64_t resets = 0, lists = 0;
    check(reset_in_flight::Section(counters, resets, lists).empty(), "no section without a Reset");
    counters.Add(2), counters.Add(1), counters.Add(2);
    auto text = reset_in_flight::Section(counters, resets, lists);
    check(text == "resets=3 lists=5", "the section: " + text);
    check(reset_in_flight::Section(counters, resets, lists).empty(), "no section for the second after, without one");
    counters.Add(4);
    text = reset_in_flight::Section(counters, resets, lists);
    check(text == "resets=1 lists=4", "the second's own: " + text);
  }

  if (failures)
    return 1;
  std::printf("reset_in_flight: all passed\n");
  return 0;
}
