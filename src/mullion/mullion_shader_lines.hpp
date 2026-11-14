#pragma once

// The lines a shader or a pipeline that failed writes (docs/swtor/README.md § The shader failure lines;
// tests/swtor/fixtures/session.log has samples):
//
//   MULLION-SHADER at=<HH:MM:SS.mmm> step=<step> stage=<vs|ps|gs|hs|ds|cs|ms|as|-> sha1=<SHA-1>[,<SHA-1>...]
//     kind=<graphics|compute|mesh> dumped=<yes|no|limit> | <the converter's or Metal's text>
//     [[<n> earlier lines suppressed]]
//   MULLION-SHADER at=<HH:MM:SS.mmm> step=draw-skipped stage=- sha1=<SHA-1>[,<SHA-1>...] kind=<kind> skipped=<n>
//     [[<n> earlier lines suppressed]]
//   MULLION-SHADER-TOTALS failed=<n> byStep=<step>:<n>[,...] skippedDraws=<n> dumped=<n>
//
// each on one line: a failure at err (cache-blob at warn), draw-skipped at warn, the totals at info. The SHA-1s are
// what MULLION_SHADER_DUMP_PATH names the dumped shaders by, joined with commas as MULLION-PIPELINE's name=. The text
// after | goes through gpu_error::Text, so a path prints as (a path) and a text with the user's or the computer's
// name in it as (a name), cut to 300 bytes.
//
// Pure C++ (no Windows, no Metal): tests/host/shader_lines.cpp builds it natively.

#include "mullion_gpu_error.hpp"
#include "mullion_stats.hpp"
#include <atomic>
#include <fstream>
#include <unordered_set>

namespace mullion::shader_lines {

// where a shader or a pipeline failed, in the order a pipeline is made
enum class Step : unsigned {
  Container,       // Direct3D's check of the container: its layout and its hash
  Features,        // a feature the device doesn't have (SFI0)
  Parse,           // the converter reading the shader (SM50Initialize)
  Convert,         // the converter making the Metal function (SM50Compile...)
  Library,         // Metal refusing the library the converter made
  Pipeline,        // Metal refusing the pipeline, when it is created or on a compile thread after
  Variant,         // Metal refusing a variant made at draw time: topology, targets, sampler emulation
  CacheBlob,       // Metal refusing a stored shader, which is deleted and converted again (warn)
  PipelineLibrary, // Metal refusing a shader a pipeline library stored
  Count,
};

inline constexpr const char *kStepNames[unsigned(Step::Count)] = {
    "container", "features", "parse", "convert", "library", "pipeline", "variant", "cache-blob", "pipeline-library",
};

inline const char *
Name(Step step) {
  return unsigned(step) < unsigned(Step::Count) ? kStepNames[unsigned(step)] : "unknown";
}

// the text after | at most
constexpr size_t kMessageLimit = 300;
// the shaders a session writes at most
constexpr unsigned kDumpLimit = 32;

enum class Dumped { No, Yes, Limit };

inline const char *
Name(Dumped dumped) {
  return dumped == Dumped::Yes ? "yes" : dumped == Dumped::Limit ? "limit" : "no";
}

// a pipeline's shaders together: limit when any met the limit, else no when any isn't on disk
inline Dumped
Worse(Dumped a, Dumped b) {
  return a == Dumped::Limit || b == Dumped::Limit ? Dumped::Limit
         : a == Dumped::No || b == Dumped::No     ? Dumped::No
                                                  : Dumped::Yes;
}

// a shader's stage from its program type, the version token's top half (D3D10_SB_TOKENIZED_PROGRAM_TYPE, and DXIL's
// DXIL::ShaderKind for mesh and amplification); "-" for none or another
inline const char *
Stage(int type) {
  switch (type) {
  case 0:
    return "ps";
  case 1:
    return "vs";
  case 2:
    return "gs";
  case 3:
    return "hs";
  case 4:
    return "ds";
  case 5:
    return "cs";
  case 13:
    return "ms";
  case 14:
    return "as";
  }
  return "-";
}

inline std::string
At(const stats::Clock &at) {
  char text[16];
  std::snprintf(text, sizeof(text), "%02u:%02u:%02u.%03u", at.hour % 100, at.minute % 100, at.second % 100,
                at.millisecond % 1000);
  return text;
}

// SHA-1s each followed by a space (MTLD3D12PipelineState::name), joined with commas; "-" for none
inline std::string
Joined(std::string_view spaced) {
  std::string joined;
  for (size_t at = 0; at < spaced.size();) {
    size_t end = std::min(spaced.find(' ', at), spaced.size());
    if (end > at)
      joined += (joined.empty() ? "" : ",") + std::string(spaced.substr(at, end - at));
    at = end + 1;
  }
  return joined.empty() ? "-" : joined;
}

struct Failure {
  stats::Clock at; // local time
  Step step = Step::Parse;
  std::string stage = "-";
  std::string sha1; // SHA-1s each followed by a space
  std::string kind = "graphics";
  Dumped dumped = Dumped::No;
  std::string message;            // the converter's or Metal's
  uint64_t suppressed = 0;        // lines the limit left out before this one
  std::vector<std::string> names; // the user's and the computer's names (session::UserNames)
};

inline std::string
Suppressed(uint64_t suppressed) {
  return suppressed ? " [" + std::to_string(suppressed) + " earlier lines suppressed]" : "";
}

inline std::string
Line(const Failure &failure) {
  std::string line = "MULLION-SHADER at=" + At(failure.at) + " step=" + Name(failure.step) + " stage=" + failure.stage +
                     " sha1=" + Joined(failure.sha1) + " kind=" + failure.kind + " dumped=" + Name(failure.dumped);
  auto tail = Suppressed(failure.suppressed);
  auto text = gpu_error::Text(failure.message, kMessageLimit, failure.names);
  size_t room = gpu_error::kLineLimit - 1 - std::min(gpu_error::kLineLimit - 1, line.size() + 3 + tail.size());
  if (text.size() > room)
    text.resize(room);
  return line + " | " + text + tail;
}

// the first draw (or dispatch) a pipeline drops for want of a Metal pipeline; `skipped` the process's so far
inline std::string
SkipLine(const stats::Clock &at, std::string_view sha1, std::string_view kind, uint64_t skipped, uint64_t suppressed) {
  return "MULLION-SHADER at=" + At(at) + " step=draw-skipped stage=- sha1=" + Joined(sha1) + " kind=" + std::string(kind) +
         " skipped=" + std::to_string(skipped) + Suppressed(suppressed);
}

// the process's, whatever the limit leaves out: the lines of each step, and the draws and dispatches dropped
struct Totals {
  std::atomic<uint64_t> steps[unsigned(Step::Count)] = {};
  std::atomic<uint64_t> skipped{0};
};
inline Totals totals;

// `failed` every step's but cache-blob's, whose shader is converted again; byStep each step with a line, cache-blob
// included, "none" for none
inline std::string
TotalsLine(const uint64_t (&steps)[unsigned(Step::Count)], uint64_t skipped, uint64_t dumped) {
  uint64_t failed = 0;
  std::string by;
  for (unsigned i = 0; i < unsigned(Step::Count); i++) {
    if (!steps[i])
      continue;
    if (Step(i) != Step::CacheBlob)
      failed += steps[i];
    by += (by.empty() ? "" : ",") + std::string(kStepNames[i]) + ":" + std::to_string(steps[i]);
  }
  return "MULLION-SHADER-TOTALS failed=" + std::to_string(failed) + " byStep=" + (by.empty() ? "none" : by) +
         " skippedDraws=" + std::to_string(skipped) + " dumped=" + std::to_string(dumped);
}

// the shaders that failed, written as <folder><separator><SHA-1>.dxbc for airconv, each once, at most kDumpLimit
// files a process
class Dumps {
public:
  // yes when the file is there (written now or before), limit once kDumpLimit files are written, no without a folder
  // or a shader, or when the file can't be written
  Dumped
  Write(const std::string &folder, char separator, const std::string &sha1, const void *data, size_t size) {
    if (folder.empty() || !data)
      return Dumped::No;
    std::lock_guard lock(mutex_);
    if (written_.contains(sha1))
      return Dumped::Yes;
    if (written_.size() >= kDumpLimit)
      return Dumped::Limit;
    std::ofstream file(folder + separator + sha1 + ".dxbc", std::ios::binary);
    if (!file.write(static_cast<const char *>(data), size) || (file.close(), file.fail()))
      return Dumped::No;
    written_.insert(sha1);
    return Dumped::Yes;
  }

  size_t
  Written() {
    std::lock_guard lock(mutex_);
    return written_.size();
  }

private:
  std::mutex mutex_;
  std::unordered_set<std::string> written_;
};

} // namespace mullion::shader_lines
