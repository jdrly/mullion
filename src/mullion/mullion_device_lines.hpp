#pragma once

// The lines a Direct3D 12 device writes when it stops or can't start (docs/swtor/README.md § Log lines;
// tests/swtor/fixtures/session.log has a sample of each):
//
//   MULLION-DEVICE-REMOVED at=<HH:MM:SS.mmm> reason=<name>(0x<hresult>) cause=<cause> frame=<presents> flight=<file>
//   MULLION-GPU-HUNG at=<HH:MM:SS.mmm> queue=<n> cmdbuf=<seq> waitedMs=<n> status=<name>(<n>)
//     encoders=<render>/<compute>/<blit> present=<yes|no> flight=<file>
//   MULLION-FEATURE-LEVEL asked=<12_0> reaches=<11_1> tiledResourcesTier=<n> macOS=<26.3>
//
// each on one line, the first at err once per device, the others at warn. No field holds a pointer or a host path:
// the flight file is its name alone, through gpu_error::Text, so a path prints as (a path) and a name with the user's
// or the computer's in it as (a name).
//
// Pure C++ (no Windows, no Metal): tests/host/device_lines.cpp builds it natively.

#include "mullion_gpu_error.hpp"

namespace mullion {

// why a device is removed, the line's cause=. LoseDevice takes one from each caller, with no default
enum class RemovalCause { GPUError, AtomicLock, AppRemoveDevice, NoCommandBuffer, BufferAllocation, OpenListExecuted };

namespace device_lines {

inline const char *
Name(RemovalCause cause) {
  switch (cause) {
  case RemovalCause::GPUError:
    return "gpu-error";
  case RemovalCause::AtomicLock:
    return "atomic-lock";
  case RemovalCause::AppRemoveDevice:
    return "app-RemoveDevice";
  case RemovalCause::NoCommandBuffer:
    return "no-command-buffer";
  case RemovalCause::BufferAllocation:
    return "buffer-allocation";
  case RemovalCause::OpenListExecuted:
    return "open-list-executed";
  }
  return "unknown";
}

// the removal reasons LoseDevice is given
inline const char *
ReasonName(uint32_t hresult) {
  switch (hresult) {
  case 0x887a0001:
    return "DXGI_ERROR_INVALID_CALL";
  case 0x887a0005:
    return "DXGI_ERROR_DEVICE_REMOVED";
  case 0x887a0006:
    return "DXGI_ERROR_DEVICE_HUNG";
  }
  return "Unknown";
}

// Metal's MTLCommandBufferStatus
inline const char *
StatusName(uint64_t status) {
  constexpr const char *names[] = {"notEnqueued", "enqueued", "committed", "scheduled", "completed", "error"};
  return status < std::size(names) ? names[status] : "unknown";
}

struct Time {
  unsigned hour = 0, minute = 0, second = 0, millisecond = 0; // local time
};

inline std::string
At(const Time &time) {
  char at[16];
  std::snprintf(at, sizeof(at), "%02u:%02u:%02u.%03u", time.hour % 100, time.minute % 100, time.second % 100,
                time.millisecond % 1000);
  return at;
}

// the flight file as a line holds it: its name, cut to 40 bytes, a space as _; "none" when the dump wrote none
inline std::string
Flight(std::string_view file, const std::vector<std::string> &names) {
  if (file.empty())
    return "none";
  auto text = gpu_error::Text(file, gpu_error::kLabelLimit, names);
  for (char &c : text)
    if (c == ' ')
      c = '_';
  return text;
}

struct Removal {
  Time at;
  uint32_t reason = 0; // the HRESULT
  RemovalCause cause = RemovalCause::GPUError;
  uint64_t frame = 0;             // the process's Presents so far
  std::string flight;             // the flight file's name, "" for none
  std::vector<std::string> names; // the user's and the computer's names (session::UserNames)
};

inline std::string
Removed(const Removal &removal) {
  char reason[16];
  std::snprintf(reason, sizeof(reason), "0x%08x", removal.reason);
  return "MULLION-DEVICE-REMOVED at=" + At(removal.at) + " reason=" + ReasonName(removal.reason) + "(" + reason +
         ") cause=" + Name(removal.cause) + " frame=" + std::to_string(removal.frame) +
         " flight=" + Flight(removal.flight, removal.names);
}

struct Hang {
  Time at;
  uint64_t queue = 0, cmdbuf = 0, waited_ms = 0;
  uint64_t status = 0;                        // MTLCommandBufferStatus
  uint64_t render = 0, compute = 0, blit = 0; // its encoders of each kind
  bool present = false;
  std::string flight;
  std::vector<std::string> names;
};

inline std::string
Hung(const Hang &hang) {
  return "MULLION-GPU-HUNG at=" + At(hang.at) + " queue=" + std::to_string(hang.queue) +
         " cmdbuf=" + std::to_string(hang.cmdbuf) + " waitedMs=" + std::to_string(hang.waited_ms) +
         " status=" + StatusName(hang.status) + "(" + std::to_string(hang.status) + ")" +
         " encoders=" + std::to_string(hang.render) + "/" + std::to_string(hang.compute) + "/" +
         std::to_string(hang.blit) + " present=" + (hang.present ? "yes" : "no") +
         " flight=" + Flight(hang.flight, hang.names);
}

struct Refusal {
  uint32_t asked = 0, reaches = 0; // D3D_FEATURE_LEVEL
  uint32_t tiled_resources_tier = 0;
  uint64_t macos[3] = {};
};

// a D3D_FEATURE_LEVEL as 12_1, as the session header's fl=
inline std::string
Level(uint32_t level) {
  return std::to_string((level >> 12) & 0xf) + "_" + std::to_string((level >> 8) & 0xf);
}

inline std::string
FeatureLevel(const Refusal &refusal) {
  auto &macos = refusal.macos;
  return "MULLION-FEATURE-LEVEL asked=" + Level(refusal.asked) + " reaches=" + Level(refusal.reaches) +
         " tiledResourcesTier=" + std::to_string(refusal.tiled_resources_tier) + " macOS=" + std::to_string(macos[0]) +
         "." + std::to_string(macos[1]) + (macos[2] ? "." + std::to_string(macos[2]) : "");
}

} // namespace device_lines
} // namespace mullion
