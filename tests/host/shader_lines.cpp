// contract: a shader or a pipeline that fails writes one MULLION-SHADER line (src/mullion/mullion_shader_lines.hpp)
// with the fields and order of docs/swtor/README.md § The shader failure lines, exactly as
// tests/swtor/fixtures/session.log has them; every step has its name; the text after | keeps the fork's rules; a
// process writes at most 32 shaders, each once (into a temporary folder here); and a shader the converter refuses,
// written as the driver writes it, fails again with the same message through the README's airconv command. a shader
// whose container doesn't hold isn't the converter's to refuse: airconv doesn't check a container's hash.
//
//   host_shader_lines tests/swtor/fixtures/session.log <airconv>
//
// the shaders are made here, token by token, and garbled as tests/garbled.hpp garbles them.
#include "../../src/mullion/mullion_shader_lines.hpp"
#include "../../libs/DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "../garbled.hpp"
#include "sha1/sha1_util.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <unistd.h>

using namespace mullion;
using namespace mullion::shader_lines;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static const std::string a = "9f2c4e1a7b3d5f60718293a4b5c6d7e8f9012345", b = "1a2b3c4d5e6f708192a3b4c5d6e7f80910a1b2c3",
                         c = "3c4d5e6f708192a3b4c5d6e7f80910a1b2c3d4e5", d = "e5d4c3b2a1f0e9d8c7b6a5f4e3d2c1b0a9f8e7d6";

static Failure
Made(stats::Clock at, Step step, const char *stage, std::string sha1, const char *kind, Dumped dumped,
     std::string message) {
  Failure failure;
  failure.at = at, failure.step = step, failure.stage = stage, failure.sha1 = std::move(sha1), failure.kind = kind;
  failure.dumped = dumped, failure.message = std::move(message);
  return failure;
}

static std::string
read(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

// cs_5_0: dcl_thread_group 1, 1, 1; ret. without signatures' elements, in a container signed as a compiler signs it
static std::string
Shader() {
  using namespace microsoft;
  std::vector<uint32_t> program{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  program.insert(program.end(), {ENCODE_D3D10_SB_OPCODE_TYPE(D3D11_SB_OPCODE_DCL_THREAD_GROUP) |
                                     ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(4),
                                 1, 1, 1});
  program.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1));
  program[1] = program.size();
  const std::pair<const char *, std::vector<uint32_t>> parts[] = {{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"SHEX", program}};
  std::string out("DXBC", 4);
  out.append(16, '\0');
  auto put = [&](uint32_t value) { out.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  put(1), put(0), put(std::size(parts));
  uint32_t at = 32 + 4 * std::size(parts);
  for (auto &[name, words] : parts)
    put(at), at += 8 + 4 * words.size();
  for (auto &[name, words] : parts) {
    out.append(name, 4);
    put(4 * words.size());
    out.append(reinterpret_cast<const char *>(words.data()), 4 * words.size());
  }
  uint32_t size = out.size();
  memcpy(out.data() + Size, &size, sizeof(size));
  dxbc::sign(out);
  return out;
}

int
main(int argc, char **argv) {
  if (argc != 3) {
    std::printf("usage: %s tests/swtor/fixtures/session.log <airconv>\n", argv[0]);
    return 2;
  }

  // the fixture's lines, as Wine's log has them
  {
    std::vector<std::string> lines;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.find(" MULLION-SHADER") != std::string::npos)
        lines.push_back(line);
    const uint64_t steps[unsigned(Step::Count)] = {0, 0, 1, 0, 0, 1, 0, 1, 0};
    const std::string want[] = {
        "err:   " + Line(Made({20, 0, 4, 512}, Step::Parse, "ps", a + " ", "graphics", Dumped::Yes,
                              "Invalid DXBC bytecode: input signature not found")),
        "err:   " + Line(Made({20, 0, 4, 530}, Step::Pipeline, "-", b + " " + c + " ", "graphics", Dumped::No,
                              "Fragment input(s) `user(locn1)` mismatching vertex shader output type(s) or not written "
                              "by vertex shader")),
        "warn:  " + Line(Made({20, 0, 4, 611}, Step::CacheBlob, "cs", d + " ", "compute", Dumped::No,
                              "Invalid library file")),
        "warn:  " + SkipLine({20, 0, 4, 990}, b + " " + c + " ", "graphics", 1, 0),
        "info:  " + TotalsLine(steps, 212, 1),
    };
    check(lines.size() == std::size(want), "the fixture has " + std::to_string(std::size(want)) +
                                               " MULLION-SHADER lines, not " + std::to_string(lines.size()));
    for (size_t i = 0; i < std::size(want) && i < lines.size(); i++) {
      if (lines[i] != want[i])
        std::printf("line %zu:\n%s\nwants the fixture's:\n%s\n", i, want[i].c_str(), lines[i].c_str());
      check(lines[i] == want[i], "MULLION-SHADER line " + std::to_string(i) + " is the fixture's");
    }
  }

  // every step, each field by name in its place
  {
    const char *names[] = {"container", "features", "parse",      "convert",         "library",
                           "pipeline",  "variant",  "cache-blob", "pipeline-library"};
    static_assert(std::size(names) == unsigned(Step::Count));
    for (unsigned i = 0; i < unsigned(Step::Count); i++) {
      auto line = Line(Made({1, 2, 3, 4}, Step(i), "vs", a + " ", "mesh", Dumped::Limit, "its text"));
      auto want = std::string("MULLION-SHADER at=01:02:03.004 step=") + names[i] + " stage=vs sha1=" + a +
                  " kind=mesh dumped=limit | its text";
      check(line == want, line + "\nwants\n" + want);
    }
    check(SkipLine({23, 59, 59, 999}, a + " " + b + " ", "compute", 7, 3) ==
              "MULLION-SHADER at=23:59:59.999 step=draw-skipped stage=- sha1=" + a + "," + b +
                  " kind=compute skipped=7 [3 earlier lines suppressed]",
          SkipLine({23, 59, 59, 999}, a + " " + b + " ", "compute", 7, 3));
    uint64_t none[unsigned(Step::Count)] = {};
    check(TotalsLine(none, 0, 0) == "MULLION-SHADER-TOTALS failed=0 byStep=none skippedDraws=0 dumped=0",
          TotalsLine(none, 0, 0));
    uint64_t all[unsigned(Step::Count)] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    // cache-blob's 8 are converted again: not failed
    check(TotalsLine(all, 10, 32) == "MULLION-SHADER-TOTALS failed=37 byStep=container:1,features:2,parse:3,convert:4,"
                                     "library:5,pipeline:6,variant:7,cache-blob:8,pipeline-library:9 "
                                     "skippedDraws=10 dumped=32",
          TotalsLine(all, 10, 32));
  }

  // the stages, the SHA-1s joined, and a pipeline's dumps together
  {
    const std::pair<int, const char *> stages[] = {{0, "ps"}, {1, "vs"}, {2, "gs"}, {3, "hs"}, {4, "ds"},
                                                   {5, "cs"}, {13, "ms"}, {14, "as"}, {-1, "-"}, {6, "-"}};
    for (auto [type, stage] : stages)
      check(std::string(Stage(type)) == stage, std::to_string(type) + " is " + stage);
    check(Joined("") == "-" && Joined(a + " ") == a && Joined(a + " " + b + " ") == a + "," + b, "SHA-1s joined");
    check(Worse(Dumped::Yes, Dumped::Yes) == Dumped::Yes && Worse(Dumped::Yes, Dumped::No) == Dumped::No &&
              Worse(Dumped::No, Dumped::Limit) == Dumped::Limit && Worse(Dumped::Limit, Dumped::Yes) == Dumped::Limit,
          "a pipeline's dumps together");
  }

  // the text after |: no path, no name, at most 300 bytes; the line under 900 bytes for the most it can hold
  {
    auto failure = Made({}, Step::Library, "-", a + " ", "graphics", Dumped::No,
                        "Compiler encountered an internal error: /Users/someone/Library/Caches/x.metallib");
    check(Line(failure).ends_with("| Compiler encountered an internal error: (a path)"), Line(failure));
    failure.message = "shader of alice", failure.names = {"alice"};
    check(Line(failure).ends_with("| (a name)"), Line(failure));
    failure.names = {};
    failure.message = std::string(1000, 'x');
    check(Line(failure).ends_with("| " + std::string(kMessageLimit, 'x')), "the text cut to 300 bytes");
    failure.sha1 = "";
    for (int i = 0; i < 7; i++)
      failure.sha1 += a + " ";
    failure.suppressed = ~0ull;
    auto line = Line(failure);
    check(line.size() < gpu_error::kLineLimit && line.ends_with(" earlier lines suppressed]"),
          "under 900 bytes: " + std::to_string(line.size()));
  }

  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / ("host_shader_lines." + std::to_string(::getpid()));
  fs::create_directories(dir / "dumps");
  auto files = [&] { return std::distance(fs::directory_iterator(dir / "dumps"), fs::directory_iterator()); };

  // at most 32 shaders a process, each once, none without a folder or a shader
  {
    Dumps dumps;
    const std::string folder = (dir / "dumps").string();
    check(dumps.Write("", '/', a, "x", 1) == Dumped::No, "no folder: no");
    check(dumps.Write(folder, '/', a, nullptr, 0) == Dumped::No, "no shader: no");
    for (unsigned i = 0; i < kDumpLimit; i++) {
      auto sha1 = std::to_string(i);
      check(dumps.Write(folder, '/', sha1, sha1.data(), sha1.size()) == Dumped::Yes, "shader " + sha1 + " written");
    }
    check(dumps.Write(folder, '/', "32", "32", 2) == Dumped::Limit, "the 33rd: limit");
    check(dumps.Write(folder, '/', "0", "0", 1) == Dumped::Yes, "one already written: yes");
    check(files() == std::ptrdiff_t(kDumpLimit) && dumps.Written() == kDumpLimit,
          "32 files, not " + std::to_string(files()) + " (" + std::to_string(dumps.Written()) + " counted)");
    check(read(dir / "dumps" / "31.dxbc") == "31", "a file has its shader");
    check(Dumps().Write((dir / "nowhere").string(), '/', a, "x", 1) == Dumped::No, "a folder that isn't: no");
  }

  // the converter on garbled shaders, dumped as the driver dumps them
  {
    auto convert = [&](const fs::path &shader, std::string &message) {
      auto err = dir / "err.txt";
      std::string command = "\"" + std::string(argv[2]) + "\" -o \"" + (dir / "out.air").string() + "\" \"" +
                            shader.string() + "\" 2> \"" + err.string() + "\"";
      int status = std::system(command.c_str());
      message = read(err);
      while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
      return status;
    };
    auto shader = Shader();
    {
      std::ofstream(dir / "shader.dxbc", std::ios::binary) << shader;
      std::string message;
      check(convert(dir / "shader.dxbc", message) == 0, "the test's shader converts: " + message);
    }
    auto garblings = garbled(shader);
    auto find = [&](std::string_view what) {
      for (auto &[name, code] : garblings)
        if (name.starts_with(what))
          return code;
      check(false, "no garbling " + std::string(what));
      return std::string();
    };

    Dumps dumps;
    // its program part renamed, signed again: Direct3D takes the container, the converter finds no program in it
    auto unnamed = find("part SHEX's name");
    dxbc::sign(unnamed);
    auto sha1 = Sha1HashState::compute(unnamed.data(), unnamed.size()).string();
    check(dumps.Write((dir / "dumps").string(), '/', sha1, unnamed.data(), unnamed.size()) == Dumped::Yes,
          "the shader the converter refused is written");
    check(read(dir / "dumps" / (sha1 + ".dxbc")) == unnamed, "as <sha1>.dxbc, its bytes");
    std::string message;
    check(convert(dir / "dumps" / (sha1 + ".dxbc"), message) != 0, "airconv refuses it again");
    check(message == "Invalid DXBC bytecode: shader blob not found", "with the converter's message: " + message);
    auto line = Line(Made({}, Step::Parse, Stage(-1), sha1 + " ", "compute", Dumped::Yes, message));
    check(line.find(" step=parse stage=- sha1=" + sha1 + " kind=compute dumped=yes | " + message) != std::string::npos,
          line);

    // its hash changed: Direct3D refuses it (step=container), airconv doesn't look and converts it
    auto flipped = find("the hash");
    auto computed = dxbc::hash(flipped.data(), flipped.size());
    check(memcmp(flipped.data() + dxbc::hash_at, computed.data(), computed.size()) != 0, "its hash doesn't hold");
    std::ofstream(dir / "flipped.dxbc", std::ios::binary) << flipped;
    check(convert(dir / "flipped.dxbc", message) == 0, "airconv converts a container whose hash doesn't hold: " + message);
  }

  fs::remove_all(dir);
  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
