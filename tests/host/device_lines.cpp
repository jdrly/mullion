// contract: a removed device, a command buffer that hasn't completed and a refused feature level each write one line
// (src/mullion/mullion_device_lines.hpp) with the fields and order of docs/swtor/README.md § Log lines, exactly as
// tests/swtor/fixtures/session.log has them (SWTOR on Mac's session report tests copy that file); every cause and
// reason has its name; and no line holds a host path or the user's or the computer's name
#include "../../src/mullion/mullion_device_lines.hpp"
#include <fstream>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

// the fixture's: the device a page fault removed, at frame 3601
static device_lines::Removal
Removal() {
  device_lines::Removal removal;
  removal.at = {21, 4, 5, 124};
  removal.reason = 0x887a0006, removal.cause = RemovalCause::GPUError, removal.frame = 3601;
  removal.flight = "swtor_RC2.exe.flight";
  return removal;
}

// the fixture's: the same command buffer, 10 s before
static device_lines::Hang
Hang() {
  device_lines::Hang hang;
  hang.at = {21, 3, 55, 120};
  hang.queue = 1, hang.cmdbuf = 4812, hang.waited_ms = 10000, hang.status = 3;
  hang.render = 2, hang.compute = 0, hang.blit = 0, hang.present = true;
  hang.flight = "swtor_RC2.exe.flight";
  return hang;
}

// the fixture's: 12_2 asked of a device that reaches 12_1 on macOS 26.1
static device_lines::Refusal
Refusal() {
  device_lines::Refusal refusal;
  refusal.asked = 0xc200, refusal.reaches = 0xc100, refusal.tiled_resources_tier = 2;
  refusal.macos[0] = 26, refusal.macos[1] = 1;
  return refusal;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }

  // the fixture's lines, as Wine's log has them
  {
    std::string removed, hung, level;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);) {
      if (line.starts_with("err:   MULLION-DEVICE-REMOVED "))
        removed = line;
      if (line.starts_with("warn:  MULLION-GPU-HUNG "))
        hung = line;
      if (line.starts_with("warn:  MULLION-FEATURE-LEVEL "))
        level = line;
    }
    auto same = [](const std::string &got, const std::string &want, const char *what) {
      if (got != want)
        std::printf("%s:\n%s\nwants the fixture's:\n%s\n", what, got.c_str(), want.c_str());
      check(got == want, std::string(what) + " is the fixture's");
    };
    same("err:   " + device_lines::Removed(Removal()), removed, "MULLION-DEVICE-REMOVED");
    same("warn:  " + device_lines::Hung(Hang()), hung, "MULLION-GPU-HUNG");
    same("warn:  " + device_lines::FeatureLevel(Refusal()), level, "MULLION-FEATURE-LEVEL");
  }

  // every cause and reason LoseDevice is given has its name
  {
    std::pair<RemovalCause, const char *> causes[] = {
        {RemovalCause::GPUError, "gpu-error"},
        {RemovalCause::AtomicLock, "atomic-lock"},
        {RemovalCause::AppRemoveDevice, "app-RemoveDevice"},
        {RemovalCause::NoCommandBuffer, "no-command-buffer"},
        {RemovalCause::BufferAllocation, "buffer-allocation"},
        {RemovalCause::OpenListExecuted, "open-list-executed"},
    };
    for (auto [cause, name] : causes) {
      auto removal = Removal();
      removal.cause = cause;
      auto line = device_lines::Removed(removal);
      check(line.find(std::string(" cause=") + name + " ") != std::string::npos, line);
    }
    std::pair<uint32_t, const char *> reasons[] = {
        {0x887a0006, "DXGI_ERROR_DEVICE_HUNG(0x887a0006)"},
        {0x887a0005, "DXGI_ERROR_DEVICE_REMOVED(0x887a0005)"},
        {0x887a0001, "DXGI_ERROR_INVALID_CALL(0x887a0001)"},
        {0x80004005, "Unknown(0x80004005)"},
    };
    for (auto [reason, name] : reasons) {
      auto removal = Removal();
      removal.reason = reason;
      auto line = device_lines::Removed(removal);
      check(line.find(std::string(" reason=") + name + " ") != std::string::npos, line);
    }
  }

  // Metal's statuses by name, and a value it doesn't have
  {
    auto hang = Hang();
    hang.status = 2;
    check(device_lines::Hung(hang).find(" status=committed(2) ") != std::string::npos, device_lines::Hung(hang));
    hang.status = 9, hang.present = false;
    check(device_lines::Hung(hang).find(" status=unknown(9) encoders=2/0/0 present=no ") != std::string::npos,
          device_lines::Hung(hang));
  }

  // the flight file: its name alone, none when the dump wrote none, no path, no name, at most 40 bytes
  {
    auto removal = Removal();
    removal.flight = "";
    check(device_lines::Removed(removal).ends_with(" flight=none"), device_lines::Removed(removal));
    removal.flight = "/Users/someone/Library/Logs/Mullion/swtor.exe.flight";
    check(device_lines::Removed(removal).ends_with(" flight=(a_path)"), device_lines::Removed(removal));
    removal.flight = "alice game.exe.flight";
    removal.names = {"alice"};
    check(device_lines::Removed(removal).ends_with(" flight=(a_name)"), device_lines::Removed(removal));
    auto hang = Hang();
    hang.flight = std::string(100, 'x') + ".flight";
    check(device_lines::Hung(hang).ends_with(" flight=" + std::string(40, 'x')), device_lines::Hung(hang));
    hang.flight = "my game.exe.flight";
    check(device_lines::Hung(hang).ends_with(" flight=my_game.exe.flight"), device_lines::Hung(hang));
  }

  // a third macOS number when it has one
  {
    auto refusal = Refusal();
    refusal.asked = 0xc000, refusal.reaches = 0xb100, refusal.macos[1] = 3, refusal.macos[2] = 1;
    check(device_lines::FeatureLevel(refusal) ==
              "MULLION-FEATURE-LEVEL asked=12_0 reaches=11_1 tiledResourcesTier=2 macOS=26.3.1",
          device_lines::FeatureLevel(refusal));
  }

  // under 900 bytes for the largest numbers
  {
    auto removal = Removal();
    removal.reason = ~0u, removal.frame = ~0ull, removal.flight = std::string(2000, 'f');
    auto hang = Hang();
    hang.queue = hang.cmdbuf = hang.waited_ms = hang.status = hang.render = hang.compute = hang.blit = ~0ull;
    hang.flight = std::string(2000, 'f');
    auto refusal = Refusal();
    refusal.asked = refusal.reaches = refusal.tiled_resources_tier = ~0u;
    refusal.macos[0] = refusal.macos[1] = refusal.macos[2] = ~0ull;
    for (auto &line : {device_lines::Removed(removal), device_lines::Hung(hang), device_lines::FeatureLevel(refusal)})
      check(line.size() < gpu_error::kLineLimit, "under 900 bytes: " + line);
  }

  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
