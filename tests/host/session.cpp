// contract: the session header (src/mullion/mullion_session.hpp) writes the lines, fields and order of
// docs/swtor/README.md § Log lines, exactly as tests/swtor/fixtures/session.log has them (SWTOR on Mac's session report
// tests copy that file); no line reaches 900 bytes, a longer one going on as `Mullion: <line>+` at a field boundary;
// no value names a path; and a switch (src/util/util_switch.hpp) is listed by being declared, and read anew each time
// by its grammar
#include "../../src/mullion/mullion_session.hpp"
#include "../../src/util/util_switch.hpp"
#include <cstdio>
#include <fstream>
#include <map>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static std::map<std::string, std::string> environment;

static std::string
fromEnvironment(const char *name) {
  auto found = environment.find(name);
  return found == environment.end() ? "" : found->second;
}

static uint32_t
number(const std::string &raw, const uint32_t &fallback) {
  return raw.empty() ? fallback : uint32_t(std::stoul(raw));
}

// the fixture's session: an M5 Max on macOS 26.1
static session::Header
Sample(const sw::Registry &registry) {
  session::Header header;
  header.version = "v0.5-alpha.3-118-g1a2b3c4";
  header.api = "d3d12";
  header.feature_level = 0xc100;
  header.metal4 = "on";
  header.shader_metal = 400;
  header.airconv = 103;
  header.macos[0] = 26, header.macos[1] = 1;
  header.build = "25B78";
  header.gpu = "Apple M5 Max";
  header.family = 10;
  header.ram = 128ull << 30;
  header.working_set = 108ull << 30;
  header.arch = "x86_64-rosetta";
  header.placement_sparse = true;
  header.switches = registry.list();
  header.env = {{"MULLION_SHADER_FLAGS", "0x10"},
                {"HOME", "/Users/someone"},
                {"MULLION_LOG_PATH", "/Users/someone/Library/Logs/SWTOR"},
                {"MTL_HUD_ENABLED", "1"},
                {"DXMT_LOG_LEVEL", "info"},
                {"WINEDEBUG", "-all"}};
  header.cache_directory = "mullion/swtor_RC2.exe/";
  header.cache_file = "shaders_400.db";
  header.cache_bytes = (700ll << 20) + (512 << 10);
  return header;
}

static std::string
Joined(const std::vector<std::string> &lines) {
  std::string text;
  for (auto &line : lines)
    text += line + "\n";
  return text;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::printf("usage: %s tests/swtor/fixtures/session.log\n", argv[0]);
    return 2;
  }
  sw::Registry registry(fromEnvironment);
  sw::Switch<bool> sample_switch{"MULLION_SAMPLE_SWITCH", true, sw::notZero, registry};
  sw::Switch<uint32_t> sample_limit{"MULLION_SAMPLE_LIMIT", 4096, number, registry};

  // the fixture: the header's lines as Wine's log has them, "info:  " first (its other lines are tests/host/stats.cpp's)
  {
    std::vector<std::string> want;
    std::ifstream fixture(argv[1]);
    for (std::string line; std::getline(fixture, line);)
      if (line.starts_with("info:  Mullion: "))
        want.push_back(line);
    check(!want.empty(), std::string("the fixture ") + argv[1] + " has lines");
    std::vector<std::string> got;
    for (auto &line : session::Format(Sample(registry)))
      got.push_back("info:  " + line);
    if (got != want)
      std::printf("the header:\n%swants the fixture:\n%s", Joined(got).c_str(), Joined(want).c_str());
    check(got == want, "the header is the fixture's");
  }

  // what couldn't be read is n/a; the cache off, a masked family, no switches and no variables
  {
    auto header = Sample(registry);
    header.build.reset(), header.family.reset(), header.ram.reset();
    header.masked = 9;
    header.metal4 = "unsupported";
    header.macos[2] = 2;
    header.switches.clear(), header.env.clear();
    header.cache_directory.reset();
    header.cache_entries.reset(), header.cache_bytes.reset();
    auto lines = session::Format(header);
    check(lines.size() == 5, "five lines");
    check(lines[0].ends_with(" metal4=unsupported shaderMetal=400 airconv=103 codegen=n/a"), lines[0]);
    check(lines[1].starts_with("Mullion: mac macOS=26.1.2 build=n/a gpu=\"Apple M5 Max\" family=n/a masked=Apple9 ram=n/a "),
          lines[1]);
    check(lines[2] == "Mullion: switches none", lines[2]);
    check(lines[3] == "Mullion: env none", lines[3]);
    check(lines[4] == "Mullion: cache shaders=off entries=n/a sizeMiB=n/a", lines[4]);
  }

  // the path rule: SWTOR on Mac's for its own header
  check(session::Value("/Users/someone/Library") == "(a path)", "an absolute path");
  check(session::Value("C:\\users\\someone") == "(a path)", "a Windows path");
  check(session::Value("a b\"c'd$e`f") == "abcdef", "only letters, digits and , . ; : _ + = - stay");
  check(session::Value("0x10,a;b:c_d+e=f-g.h") == "0x10,a;b:c_d+e=f-g.h", "those stay as they are");
  check(session::Value(std::string(100, 'x')) == std::string(80, 'x'), "cut to 80");
  {
    auto header = Sample(registry);
    header.version = "v1-dirty /Users/someone";
    header.gpu = "Apple \"M5\" Max\\/";
    header.cache_directory = "/Users/someone/caches/";
    header.env.push_back({"MULLION_SHADER_CACHE_PATH", "/Users/someone/caches"});
    header.env.push_back({"MULLION_LOG_FILE", "C:\\users\\someone\\log.txt"});
    header.env.push_back({"MULLION_BAD-NAME", "x"});
    header.env.push_back({"mullion_lower", "x"});
    auto text = Joined(session::Format(header));
    check(text.find("/Users") == std::string::npos && text.find("someone") == std::string::npos, "no path:\n" + text);
    check(text.find("shaders=(a path)/shaders_400.db ") != std::string::npos, "an absolute cache directory");
    check(text.find("gpu=\"Apple M5 Max\" ") != std::string::npos, "a GPU name keeps no quote, slash or backslash");
    check(text.find("BAD-NAME") == std::string::npos && text.find("mullion_lower") == std::string::npos,
          "only MULLION_, DXMT_ and MTL_ names of letters, digits and _");
  }

  // no value has the user's login or the computer's name in it, in any case: a short one as a whole word
  {
    auto header = Sample(registry);
    header.env.push_back({"USER", "jandoe"});
    header.env.push_back({"COMPUTERNAME", "JANS-MBP"});
    header.env.push_back({"USERPROFILE", "C:\\users\\jd\\"});
    header.env.push_back({"MULLION_PROFILE", "JanDoe"});
    header.env.push_back({"MULLION_NOTE", "for-jans-mbp-tests"});
    header.env.push_back({"MULLION_SHORT", "a,jd"});
    header.env.push_back({"MULLION_HEX", "0xjd1"});
    header.env.push_back({"MULLION_JANDOE_X", "1"});
    header.switches.push_back({"MULLION_SAMPLE_TEXT", "by JanDoe"});
    header.version = "v1-jandoe";
    header.gpu = "Apple M5 Max / jandoe";
    header.cache_file = "shaders_jandoe.db";
    auto text = Joined(session::Format(header));
    check(text.find("jandoe") == std::string::npos && text.find("JanDoe") == std::string::npos &&
              text.find("jans-mbp") == std::string::npos,
          "no name:\n" + text);
    for (auto field : {"MULLION_PROFILE=(a name)", "MULLION_NOTE=(a name)", "MULLION_SHORT=(a name)",
                       "MULLION_HEX=0xjd1", "MULLION_SAMPLE_TEXT=(a name)", "session (a name) ", "gpu=\"(a name)\"",
                       "shaders=mullion/swtor_RC2.exe/(a name) "})
      check(text.find(field) != std::string::npos, std::string("has ") + field + ":\n" + text);
    check(text.find("MULLION_JANDOE_X") == std::string::npos, "a variable whose name names the user isn't shown");
  }
  // Windows' variable names in any case: UserName and UserProfile are USERNAME and USERPROFILE
  {
    auto header = Sample(registry);
    header.env = {{"UserName", "JanDoe"}, {"UserProfile", "C:\\users\\JanDoe"}, {"MULLION_PROFILE", "JanDoe"}};
    auto text = Joined(session::Format(header));
    check(text.find("MULLION_PROFILE=(a name)") != std::string::npos, "UserName in any case:\n" + text);
  }

  // wrapping: no line reaches 900 bytes, and every field is whole on one line, in its order
  {
    auto header = Sample(registry);
    std::vector<std::string> fields;
    for (unsigned i = 0; i < 100; i++) {
      char name[32];
      std::snprintf(name, sizeof(name), "MULLION_VARIABLE_%03u", i);
      header.env.push_back({name, std::string(90, 'a' + i % 26)});
      fields.push_back(std::string(name) + "=" + std::string(80, 'a' + i % 26));
    }
    // and one name longer than a line holds
    header.env.push_back({"MULLION_" + std::string(2000, 'Z'), "1"});
    auto lines = session::Format(header);
    std::vector<std::string> env;
    unsigned continued = 0;
    for (auto &line : lines) {
      check(line.size() < session::kLineLimit, "shorter than 900: " + std::to_string(line.size()));
      bool first = line.starts_with("Mullion: env "), more = line.starts_with("Mullion: env+ ");
      continued += more;
      if (!first && !more)
        continue;
      for (size_t at = line.find(' ', 9) + 1, end; at; at = end + 1) {
        end = line.find(' ', at);
        env.push_back(line.substr(at, end - at));
        if (end == std::string::npos)
          break;
      }
    }
    check(continued >= 10, "the env line goes on over " + std::to_string(continued) + " lines");
    std::vector<std::string> shown;
    for (auto &field : env)
      if (field.starts_with("MULLION_VARIABLE_"))
        shown.push_back(field);
    check(shown == fields, "each variable whole, by name");
    check(std::count(env.begin(), env.end(), "MULLION_" + std::string(72, 'Z') + "=1") == 1,
          "a long name cut to 80");
    auto one = session::Lines("x", {{"", std::string(2000, 'a')}});
    check(one.size() == 1 && one[0].size() == session::kLineLimit - 1, "a field longer than a line is cut");
  }

  // a declared switch is listed with its value now, by name; read anew each time by its grammar
  {
    check(registry.list() == std::vector<std::pair<std::string, std::string>>{{"MULLION_SAMPLE_LIMIT", "4096"},
                                                                              {"MULLION_SAMPLE_SWITCH", "on"}},
          "both switches, by name");
    environment["MULLION_SAMPLE_SWITCH"] = "0";
    environment["MULLION_SAMPLE_LIMIT"] = "12";
    check(!sample_switch() && sample_limit() == 12, "the environment is read at each read");
    check(registry.list()[1].second == "off" && registry.list()[0].second == "12", "listed as they read now");
    environment.clear();
    check(sample_switch() && sample_limit() == 4096, "the fallback when unset");
    check(!registry.add("MULLION_SAMPLE_SWITCH", [] { return std::string("x"); }), "one declaration per name");
    check(sw::notZero("2", false) && !sw::notZero("0", true) && sw::notZero("", true), "!= \"0\"");
    check(sw::exactlyOne("1", false) && !sw::exactlyOne("2", true) && sw::exactlyOne("", true), "== \"1\"");
  }

  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
