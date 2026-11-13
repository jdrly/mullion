#pragma once

// The line a command buffer the GPU failed writes, at err, whatever MULLION_LOG_LEVEL is short of none
// (docs/swtor/README.md § Log lines; tests/swtor/fixtures/session.log has a sample):
//
//   MULLION-GPU-ERROR at=<HH:MM:SS.mmm> queue=<n> cmdbuf=<seq> frame=<presents> error=<Name>(<code>) [domain=<d>]
//     first=<yes|no> gpu=<start>-<end>ms encoders=<n> [#<i> <state> '<label>' ...] [+<k> more] | <Metal's text>
//     [[<n> earlier lines suppressed]]
//
// on one line. The bridge reads Metal's error (WMTGetGPUError): the error's name from its code, the encoders that
// didn't complete with their labels, and Metal's own text. "n/a" is a value Metal didn't give. No field holds a
// pointer or a host path: Metal's text goes through Text().
//
// Pure C++ (no Windows, no Metal): tests/host/gpu_error.cpp builds it natively.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mullion::gpu_error {

// every line is shorter, as every fork line (mullion_session.hpp)
constexpr size_t kLineLimit = 900;
// Metal's text, and a label, at most
constexpr size_t kTextLimit = 200;
constexpr size_t kLabelLimit = 40;
// the encoders a line lists at most; the bridge's WMTGPUError holds as many
constexpr size_t kEncoders = 8;
// at most kLines lines in kWindow; the next line after says how many were left out
constexpr unsigned kLines = 30;
constexpr uint64_t kWindow = 10'000'000'000ull;

struct Encoder {
  uint32_t index;    // its place in the command buffer, from 0
  std::string state; // affected, pending, faulted (completed, unknown)
  std::string label; // "" for none
};

struct Report {
  unsigned hour = 0, minute = 0, second = 0, millisecond = 0; // local time
  uint64_t queue = 0, cmdbuf = 0, frame = 0;
  std::string name;   // the error's name from its code; "" when the bridge's table doesn't know it
  int64_t code = 0;
  std::string domain; // "" for Metal's command buffer and Metal 4 queue domains
  bool first = true;  // the device's first; later ones are what the first one's fault took with it
  uint64_t gpu_start = 0, gpu_end = 0; // Metal's GPU times, ns; both 0 when it gave none
  std::optional<uint32_t> encoders;    // how many Metal listed; none when it listed none
  std::vector<Encoder> listed;         // the ones that didn't complete, at most kEncoders
  uint32_t unlisted = 0;               // more that didn't complete
  std::string text;                    // Metal's localized description
  uint64_t suppressed = 0;             // lines the limit left out before this one
};

inline bool
Hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Metal's text as the line holds it: printable ASCII, other bytes '?'; a hexadecimal number of 6 digits or more (an
// address) as 0x?; a word that is a path (starts with / or ~, or has :\) as (a path); cut to `limit` bytes
inline std::string
Text(std::string_view raw, size_t limit = kTextLimit) {
  std::string text;
  for (size_t at = 0; at < raw.size();) {
    if (raw.substr(at, 2) == "0x" || raw.substr(at, 2) == "0X") {
      size_t end = at + 2;
      while (end < raw.size() && Hex(raw[end]))
        end++;
      if (end - at - 2 >= 6) {
        text += "0x?";
        at = end;
        continue;
      }
    }
    // a word starts after a space, ( " ' or =
    constexpr std::string_view before = " (\"'=";
    if ((at == 0 || before.find(raw[at - 1]) != std::string_view::npos) && before.find(raw[at]) == std::string_view::npos) {
      size_t end = raw.find_first_of(" )\"'", at);
      auto token = raw.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
      if (token.starts_with('/') || token.starts_with('~') || token.find(":\\") != std::string_view::npos) {
        text += "(a path)";
        at += token.size();
        continue;
      }
    }
    char c = raw[at++];
    text += c >= ' ' && c <= '~' ? c : c == '\n' || c == '\r' || c == '\t' ? ' ' : '?';
  }
  if (text.size() > limit)
    text.resize(limit);
  return text;
}

// a label in the line, between single quotes: Text() with ' as "
inline std::string
Label(std::string_view raw) {
  auto label = Text(raw, kLabelLimit);
  for (char &c : label)
    if (c == '\'')
      c = '"';
  return label;
}

inline std::string
Milliseconds(uint64_t ns) {
  char text[32];
  std::snprintf(text, sizeof(text), "%llu.%03llu", (unsigned long long)(ns / 1'000'000),
                (unsigned long long)(ns / 1000 % 1000));
  return text;
}

inline std::string
Line(const Report &report) {
  char at[16];
  std::snprintf(at, sizeof(at), "%02u:%02u:%02u.%03u", report.hour % 100, report.minute % 100, report.second % 100,
                report.millisecond % 1000);
  std::string line = std::string("MULLION-GPU-ERROR at=") + at + " queue=" + std::to_string(report.queue) +
                     " cmdbuf=" + std::to_string(report.cmdbuf) + " frame=" + std::to_string(report.frame) +
                     " error=" + (report.name.empty() ? "Unknown" : Text(report.name, 24)) + "(" +
                     std::to_string(report.code) + ")";
  if (!report.domain.empty()) {
    std::string domain;
    for (char c : Text(report.domain, 48))
      if (c != ' ')
        domain += c;
    line += " domain=" + domain;
  }
  line += std::string(" first=") + (report.first ? "yes" : "no");
  line += " gpu=" + (report.gpu_start || report.gpu_end
                         ? Milliseconds(report.gpu_start) + "-" + Milliseconds(report.gpu_end) + "ms"
                         : std::string("n/a"));
  line += " encoders=" + (report.encoders ? std::to_string(*report.encoders) : std::string("n/a"));
  for (size_t i = 0; i < report.listed.size() && i < kEncoders; i++) {
    auto &encoder = report.listed[i];
    line += " #" + std::to_string(encoder.index) + " " + Text(encoder.state, 12);
    if (!encoder.label.empty())
      line += " '" + Label(encoder.label) + "'";
  }
  uint64_t more = report.unlisted + (report.listed.size() > kEncoders ? report.listed.size() - kEncoders : 0);
  if (more)
    line += " +" + std::to_string(more) + " more";
  std::string tail = report.suppressed ? " [" + std::to_string(report.suppressed) + " earlier lines suppressed]" : "";
  // Metal's text takes what is left of the line
  auto text = Text(report.text);
  size_t room = kLineLimit - 1 - std::min(kLineLimit - 1, line.size() + 3 + tail.size());
  if (text.size() > room)
    text.resize(room);
  return line + " | " + text + tail;
}

// at most kLines lines in a window of kWindow ns, which starts with the first line after the last window ended. Allow
// says whether a line is written now, and `suppressed` how many were left out since the last one written
class Limiter {
public:
  bool
  Allow(uint64_t now, uint64_t &suppressed) {
    std::lock_guard lock(mutex_);
    if (!started_ || now - start_ >= kWindow)
      started_ = true, start_ = now, count_ = 0;
    if (count_ >= kLines) {
      dropped_++;
      return false;
    }
    count_++;
    suppressed = dropped_;
    dropped_ = 0;
    return true;
  }

private:
  std::mutex mutex_;
  bool started_ = false;
  uint64_t start_ = 0, dropped_ = 0;
  unsigned count_ = 0;
};

} // namespace mullion::gpu_error
