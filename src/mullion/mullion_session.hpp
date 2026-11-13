#pragma once

// The session header: what a process ran with, written once when its first Direct3D 12 device is created
// (mullion_session.cpp gathers it). Its lines, their fields and their order are a contract with SWTOR on Mac's session
// report (docs/swtor/README.md § Log lines; tests/swtor/fixtures/session.log is the sample both repositories test
// against):
//
//   Mullion: session <version> (jdrly/mullion swtor-on-mac) d3d12 fl= metal4= shaderMetal= airconv= codegen=
//   Mullion: mac macOS= build= gpu="" family= masked= ram= workingSet= arch= supportsPlacementSparse=
//   Mullion: switches <every registered switch, NAME=value, by name>     or "none"
//   Mullion: env <every MULLION_*, DXMT_*, MTL_* variable set, NAME=value, by name>     or "none"
//   Mullion: cache shaders= entries= sizeMiB=
//
// A line that would reach kLineLimit bytes goes on as `Mullion: <line>+ ...` from the field that doesn't fit, so
// Wine (1018 bytes) never splits one. No value names a host path or the player: a value from outside Mullion goes
// through Value(). "n/a" is a value that couldn't be read.
//
// Pure C++ (no Windows, no Metal): tests/host/session.cpp builds it natively.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mullion::session {

// every line is shorter
constexpr size_t kLineLimit = 900;
// the longest value, or name, that comes from outside Mullion
constexpr size_t kValueLimit = 80;

inline bool
Kept(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         std::string_view(",.;:_+=-").find(c) != std::string_view::npos;
}

// a value from outside Mullion, by SWTOR on Mac's rule for its own header (wine-wrapper.sh, session_settings): one
// with / or \ is "(a path)"; the rest keeps only its letters, digits and , . ; : _ + = -, cut to 80 bytes
inline std::string
Value(std::string_view raw) {
  if (raw.find_first_of("/\\") != std::string_view::npos)
    return "(a path)";
  std::string kept;
  for (char c : raw)
    if (Kept(c) && kept.size() < kValueLimit)
      kept += c;
  return kept;
}

// a line's text after "Mullion: <head> ": `name=value`, or the value alone when it has no name
struct Field {
  std::string name, value;
};

// `Mullion: <head>` and its fields, or `empty` when there are none, as lines shorter than kLineLimit: a field that
// would reach it starts a `Mullion: <head>+` line. A field longer than a line can hold is cut
inline std::vector<std::string>
Lines(std::string_view head, const std::vector<Field> &fields, std::string_view empty = {}) {
  std::string first = "Mullion: " + std::string(head), next = first + "+";
  std::vector<std::string> lines{first + (fields.empty() && !empty.empty() ? " " + std::string(empty) : "")};
  for (auto &field : fields) {
    std::string text = " " + (field.name.empty() ? field.value : field.name + "=" + field.value);
    if (lines.back().size() + text.size() >= kLineLimit && lines.back().size() > next.size())
      lines.push_back(next);
    lines.back() += text.substr(0, kLineLimit - 1 - lines.back().size());
  }
  return lines;
}

// what the header says, as plain values; nullopt is "n/a"
struct Header {
  std::string version;                // MULLION_VERSION, git describe of the build
  std::string api;                    // d3d12
  uint32_t feature_level = 0;         // the device's D3D_FEATURE_LEVEL
  std::string metal4;                 // on, off (MULLION_D3D12_METAL4=0) or unsupported (no Metal 4 family)
  uint32_t shader_metal = 0;          // the Metal language version shaders are converted for: 310, 320, 400
  uint32_t airconv = 0;               // AIRCONV_VERSION
  std::optional<std::string> codegen; // the converter's codegen id (private ticket 18)
  uint64_t macos[3] = {};             // major, minor, patch
  std::optional<std::string> build;   // the macOS build, kern.osversion
  std::string gpu;                    // the Metal device's name
  std::optional<uint32_t> family;     // the GPU's own newest Apple family, whatever MULLION_GPU_FAMILY says
  uint32_t masked = 0;                // the Apple family MULLION_GPU_FAMILY makes the bridge answer as; 0: none
  std::optional<uint64_t> ram;        // bytes
  uint64_t working_set = 0;           // the device's recommended maximum, bytes
  std::string arch;                   // x86_64-rosetta, arm64ec, ...
  bool placement_sparse = false;      // the device's supportsPlacementSparse
  std::vector<std::pair<std::string, std::string>> switches; // the registry's, by name
  std::vector<std::pair<std::string, std::string>> env;      // the process's environment, any order
  std::optional<std::string> cache_directory; // the shader cache's, relative to the user's cache directory or
                                              // absolute; nullopt with the cache off (MULLION_SHADER_CACHE=0)
  std::string cache_file;                     // its file in the directory
  std::optional<int64_t> cache_entries, cache_bytes;
};

// the variables the env line shows: MULLION_*, DXMT_* and MTL_*, as SWTOR on Mac picks its own (a name of letters,
// digits and _)
inline bool
Shown(std::string_view name) {
  for (std::string_view prefix : {"MULLION_", "DXMT_", "MTL_"})
    if (name.starts_with(prefix))
      return name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
             std::string_view::npos;
  return false;
}

inline std::vector<std::string>
Format(const Header &header) {
  auto number = [](auto value) { return std::to_string(value); };
  auto known = [](const auto &value, auto text) { return value ? text(*value) : std::string("n/a"); };
  auto mib = [](uint64_t bytes) { return std::to_string(bytes >> 20); };
  auto family = [](uint32_t n) { return "Apple" + std::to_string(n); };

  std::string macos =
      number(header.macos[0]) + "." + number(header.macos[1]) + (header.macos[2] ? "." + number(header.macos[2]) : "");
  std::string gpu;
  for (char c : header.gpu)
    if (c != '"' && c != '\\' && c >= ' ' && c <= '~' && gpu.size() < kValueLimit)
      gpu += c;

  std::vector<Field> switches, env;
  for (auto &[name, value] : header.switches)
    switches.push_back({name, Value(value)});
  for (auto &[name, value] : header.env)
    if (Shown(name))
      env.push_back({name.substr(0, kValueLimit), Value(value)});
  std::sort(env.begin(), env.end(), [](auto &a, auto &b) { return a.name < b.name; });

  // a relative directory is below the user's cache directory, which names the user, and prints without it; an
  // absolute one doesn't print
  std::string shaders = "off";
  if (auto &directory = header.cache_directory) {
    shaders = directory->starts_with("/") ? "(a path)/" : "";
    if (shaders.empty())
      for (char c : *directory)
        if (c == '/' || Kept(c))
          shaders += c;
    shaders += Value(header.cache_file);
  }

  std::vector<std::string> lines;
  auto add = [&](std::string_view head, const std::vector<Field> &fields, std::string_view empty = {}) {
    auto more = Lines(head, fields, empty);
    lines.insert(lines.end(), more.begin(), more.end());
  };
  add("session", {{"", Value(header.version)},
                  {"", "(jdrly/mullion swtor-on-mac)"},
                  {"", header.api},
                  {"fl", number((header.feature_level >> 12) & 0xf) + "_" + number((header.feature_level >> 8) & 0xf)},
                  {"metal4", header.metal4},
                  {"shaderMetal", number(header.shader_metal)},
                  {"airconv", number(header.airconv)},
                  {"codegen", known(header.codegen, Value)}});
  add("mac", {{"macOS", macos},
              {"build", known(header.build, Value)},
              {"gpu", "\"" + gpu + "\""},
              {"family", known(header.family, family)},
              {"masked", header.masked ? family(header.masked) : "no"},
              {"ram", known(header.ram, mib)},
              {"workingSet", mib(header.working_set)},
              {"arch", header.arch},
              {"supportsPlacementSparse", header.placement_sparse ? "1" : "0"}});
  add("switches", switches, "none");
  add("env", env, "none");
  add("cache", {{"shaders", shaders},
                {"entries", known(header.cache_entries, number)},
                {"sizeMiB", known(header.cache_bytes, mib)}});
  return lines;
}

} // namespace mullion::session

namespace mullion {

// mullion_session.cpp: the header for the process's first device (a WMT::Device's handle; `metal4`: whether it uses
// Metal 4, `shader_metal` the Metal version its shaders are converted for), written at info once per process. Nothing
// is read with MULLION_LOG_LEVEL above info
void WriteSessionHeader(uint64_t device, const char *api, uint32_t feature_level, bool metal4, uint32_t shader_metal);

} // namespace mullion
