// contract: a command buffer the GPU failed writes one MULLION-GPU-ERROR line (src/mullion/mullion_gpu_error.hpp) with
// the fields and order of docs/swtor/README.md § Log lines, exactly as tests/swtor/fixtures/session.log has it (SWTOR
// on Mac's session report tests copy that file); it holds no address and no host path, lists at most 8 encoders with
// labels of at most 40 bytes, stays under 900 bytes; and at most 30 lines are written in 10 s, the next one saying
// how many were left out
#include "../../src/mullion/mullion_gpu_error.hpp"
#include <fstream>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

// the fixture's error: a page fault in the second of two encoders, which has a label
static gpu_error::Report
Sample() {
  gpu_error::Report report;
  report.hour = 21, report.minute = 4, report.second = 5, report.millisecond = 123;
  report.queue = 1, report.cmdbuf = 4812, report.frame = 3601;
  report.name = "PageFault", report.code = 3;
  report.gpu_start = 4512345678901, report.gpu_end = 4512361012345;
  report.encoders = 2;
  report.listed = {{1, "faulted", "render 5e6f7a8b rt0=1920x1080 RGBA8"}};
  report.text = "Caused GPU Address Fault Error (0000000b:kIOGPUCommandBufferCallbackErrorPageFault)";
  return report;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }

  // the fixture's line, as Wine's log has it: "err:   " first
  {
    std::string want;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.starts_with("err:   MULLION-GPU-ERROR "))
        want = line;
    check(!want.empty(), std::string("the fixture ") + argv[1] + " has a MULLION-GPU-ERROR line");
    auto got = "err:   " + gpu_error::Line(Sample());
    if (got != want)
      std::printf("the line:\n%s\nwants the fixture's:\n%s\n", got.c_str(), want.c_str());
    check(got == want, "the line is the fixture's");
  }

  // what Metal didn't give is n/a; a code the bridge's table doesn't know keeps its number, with its domain
  {
    auto report = Sample();
    report.name.clear(), report.code = 77, report.domain = "NSPOSIXErrorDomain";
    report.first = false;
    report.gpu_start = report.gpu_end = 0;
    report.encoders.reset(), report.listed.clear();
    report.text.clear();
    auto line = gpu_error::Line(report);
    check(line.find(" error=Unknown(77) domain=NSPOSIXErrorDomain first=no gpu=n/a encoders=n/a | ") !=
              std::string::npos,
          line);
  }

  // no address, no host path
  {
    check(gpu_error::Text("buffer at 0x600001234abc failed") == "buffer at 0x? failed", "an address");
    check(gpu_error::Text("code 0x1f") == "code 0x1f", "a short hex number stays");
    check(gpu_error::Text("in /Users/someone/Library/x.metallib now") == "in (a path) now", "a Unix path");
    check(gpu_error::Text("at (C:\\users\\someone\\x)") == "at ((a path))", "a Windows path");
    check(gpu_error::Text("from ~/Library") == "from (a path)", "a home path");
    check(gpu_error::Text("a\nb\tc\x01\xc3\xa9") == "a b c???", "printable ASCII only");
    check(gpu_error::Text("failed\n/Users/someone/cache") == "failed (a path)", "a path after a newline");
    check(gpu_error::Text("at file:///Users/someone/cache now") == "at (a path) now", "a file: URL");
    check(gpu_error::Text("relative x/y and C:/z") == "relative (a path) and (a path)", "any word with a slash");
    check(gpu_error::Text("Code=3 \"PageFault\" (00000003:kIOGPU)") == "Code=3 \"PageFault\" (00000003:kIOGPU)",
          "Metal's own words stay");
    auto report = Sample();
    report.text = "Error Domain=MTLCommandBufferErrorDomain <MTLEncoderInfo: 0x7f8a12345678> /Users/someone/x";
    report.listed[0].label = "pass in /Users/someone/x";
    auto line = gpu_error::Line(report);
    check(line.find("0x7f8a") == std::string::npos && line.find("someone") == std::string::npos, line);
  }

  // no user's or computer's name, in any case, in Metal's text, a label or the domain (session::Names)
  {
    auto report = Sample();
    report.names = {"alice", "ALICES-MBP", "jd"};
    report.text = "owner=Alice";
    report.listed[0].label = "render for alices-mbp";
    report.domain = "AliceDomain";
    report.name.clear();
    auto line = gpu_error::Line(report);
    check(line.find("lice") == std::string::npos, "no name: " + line);
    check(line.find(" domain=(a_name) ") != std::string::npos && line.find(" '(a name)' | (a name)") != std::string::npos,
          line);
    report = Sample();
    report.names = {"jd"};
    report.text = "jd ran 0xjd1";
    check(gpu_error::Line(report).ends_with(" | (a name)"), "a short name as a whole word");
    report.text = "a jdx b";
    check(gpu_error::Line(report).ends_with(" | a jdx b"), "a short name inside a word stays");
  }

  // at most 8 encoders, then how many more; a label cut to 40 bytes, its ' as "
  {
    auto report = Sample();
    report.encoders = 120;
    report.listed.clear();
    for (uint32_t i = 0; i < 10; i++)
      report.listed.push_back({10 + i, i ? "affected" : "faulted", i ? "" : std::string(60, 'x') + "'"});
    report.listed[1].label = "rt0=8x8 R8 'Scene'";
    report.unlisted = 3;
    auto line = gpu_error::Line(report);
    check(line.find(" encoders=120 #10 faulted '" + std::string(40, 'x') + "' #11 affected 'rt0=8x8 R8 \"Scene\"' #12 "
                    "affected #13") != std::string::npos,
          line);
    check(line.find(" #17 affected +5 more | ") != std::string::npos && line.find("#18") == std::string::npos, line);
  }

  // under 900 bytes, however long what Metal gave
  {
    auto report = Sample();
    report.domain = std::string(200, 'd');
    report.name = std::string(100, 'n');
    report.encoders = 4000000000u;
    report.listed.assign(8, {4000000000u, std::string(50, 's'), std::string(100, 'l')});
    report.unlisted = 4000000000u;
    report.text = std::string(2000, 't');
    report.suppressed = ~0ull;
    report.queue = report.cmdbuf = report.frame = ~0ull;
    report.code = INT64_MIN;
    auto line = gpu_error::Line(report);
    check(line.size() < gpu_error::kLineLimit, "under 900 bytes: " + std::to_string(line.size()));
    check(line.ends_with(" earlier lines suppressed]"), "the tail stays: " + line);
  }

  // 100 errors in a second: 30 lines, and the first line after the window says 70 were left out
  {
    gpu_error::Limiter limiter;
    unsigned written = 0;
    uint64_t suppressed = 0, total = 0;
    for (uint64_t i = 0; i < 100; i++)
      if (limiter.Allow(1'000'000'000 + i * 10'000'000, suppressed))
        written++, total += suppressed;
    check(written == 30 && total == 0, "30 lines of 100 in a second: " + std::to_string(written));
    check(!limiter.Allow(10'999'999'999, suppressed), "still the same window at 9.99 s");
    check(limiter.Allow(11'000'000'000, suppressed) && suppressed == 71, "the next window: " + std::to_string(suppressed));
    check(limiter.Allow(11'000'000'001, suppressed) && suppressed == 0, "then none");
    auto report = Sample();
    report.suppressed = 71;
    check(gpu_error::Line(report).ends_with(
              "kIOGPUCommandBufferCallbackErrorPageFault) [71 earlier lines suppressed]"),
          gpu_error::Line(report));
  }

  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
