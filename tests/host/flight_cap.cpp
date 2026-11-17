// jdrly/mullion: contract: a process writes at most its first 8 flight dumps whatever their reason, and after them
// only the first dump of each reason it hasn't dumped yet (src/winemetal/unix/flight_cap.h, private ticket 55). The
// first dump left out says so once, in the line docs/swtor/README.md § Log lines gives; later ones say nothing.
#include "flight_cap.h"
#include <cstdio>
#include <string>

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static const char *
Name(flight_cap_verdict verdict) {
  return verdict == FLIGHT_CAP_WRITE ? "write" : verdict == FLIGHT_CAP_STOP ? "stop" : "skip";
}

int
main() {
  // a run of stuck command buffers, then the device removed, then the process's end
  flight_cap cap{};
  unsigned written = 0;
  for (int i = 0; i < 8; i++)
    written += flight_cap_take(&cap, "a stuck command buffer") == FLIGHT_CAP_WRITE;
  check(written == 8, "the first 8 dumps of a reason are written");
  auto ninth = flight_cap_take(&cap, "a stuck command buffer");
  std::printf("the 9th stuck command buffer: %s\n", Name(ninth));
  check(ninth == FLIGHT_CAP_STOP, "the 9th dump of a reason is left out, with the line");
  char line[512];
  flight_cap_line(line, sizeof(line), &cap, "a stuck command buffer", "swtor_RC2.exe.flight");
  std::printf("%s\n", line);
  check(std::string(line) == "mullion: flight record of a stuck command buffer not written: swtor_RC2.exe.flight has "
                             "8 dumps, and takes only a reason not dumped yet now",
        "the line names the reason, the file and the dumps it has");
  unsigned later[3] = {};
  for (int i = 0; i < 1000; i++)
    later[flight_cap_take(&cap, "a stuck command buffer")]++;
  check(!later[FLIGHT_CAP_WRITE], "no later dump of a reason dumped already is written");
  check(!later[FLIGHT_CAP_STOP], "the line is written once");
  auto removed = flight_cap_take(&cap, "a removed device");
  std::printf("then a removed device: %s\n", Name(removed));
  check(removed == FLIGHT_CAP_WRITE, "a removed device after the run of stuck command buffers is dumped");
  check(flight_cap_take(&cap, "exit") == FLIGHT_CAP_WRITE, "the exit after them is dumped");
  check(flight_cap_take(&cap, "a removed device") == FLIGHT_CAP_SKIP, "a second removed device is not");
  check(cap.written == 10, "10 dumps written");

  // tests/flight.py's busy: 8 dumps of one reason and the exit, all written
  flight_cap busy{};
  written = 0;
  for (int i = 0; i < 8; i++)
    written += flight_cap_take(&busy, "busy") == FLIGHT_CAP_WRITE;
  written += flight_cap_take(&busy, "exit") == FLIGHT_CAP_WRITE;
  check(written == 9, "8 dumps of one reason and the exit are written");

  // reasons past what the cap remembers count as dumped: an exception's text names its address, and no run of them
  // grows the file without end
  flight_cap many{};
  written = 0;
  for (int i = 0; i < 100; i++)
    written += flight_cap_take(&many, ("exception in module.dll+0x" + std::to_string(i)).c_str()) == FLIGHT_CAP_WRITE;
  std::printf("100 reasons: %u written\n", written);
  check(written == kFlightReasons, "at most as many dumps as the reasons remembered");

  if (failures)
    std::printf("FAILED: %u checks\n", failures);
  else
    std::printf("ok\n");
  return failures ? 1 : 0;
}
