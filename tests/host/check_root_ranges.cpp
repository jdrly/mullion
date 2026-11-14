// jdrly/mullion: MULLION_D3D12_CHECK_RANGES (SM50_SHADER_FLAG_CHECK_ROOT_RANGES) in the converter's output, without a
// GPU. the compute shader of host_guard_static_ranges reads a constant buffer by a dynamic index, loads from a
// structured SRV, stores to a structured UAV and adds to it atomically. the native airconv writes it as LLVM assembly
// for root signatures that bind one of the three as a root descriptor and the others through a volatile descriptor
// table, and for one that binds all three through the table, each with and without -check-root-ranges. held to:
// - the table alone: one output with or without the flag. the flag changes nothing but root descriptors;
// - a root CBV, SRV or UAV: more bounds compares (`icmp ult`) with the flag than without. upstream checks a root
//   descriptor against nothing, and the flag checks each against the length its root arguments carry.
//
//   host_check_root_ranges <airconv>
//
// the shader and the root signatures are made here, token by token, so no compiler or binary fixture is needed.
#include "../../libs/DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "../dxbc_hash.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

using namespace microsoft;
using Words = std::vector<uint32_t>;

static int failures = 0;

static void
check(bool ok, const char *what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
  failures += !ok;
}

// an instruction: its opcode token, with its length, then its operands
static void
instruction(Words &program, uint32_t opcode, std::initializer_list<Words> operands = {}, uint32_t controls = 0) {
  size_t at = program.size();
  program.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(opcode) | controls);
  for (auto &operand : operands)
    program.insert(program.end(), operand.begin(), operand.end());
  program[at] |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(program.size() - at);
}

// an operand of four components, by a mask (a destination) or a swizzle or selection (a source)
static uint32_t
four(D3D10_SB_OPERAND_TYPE type, uint32_t selection, uint32_t mode, D3D10_SB_OPERAND_INDEX_DIMENSION dimension) {
  return ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
         ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(mode) | selection | ENCODE_D3D10_SB_OPERAND_TYPE(type) |
         ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(dimension);
}

static Words
masked(D3D10_SB_OPERAND_TYPE type, uint32_t index, uint32_t mask) {
  return {four(type, mask, D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE, D3D10_SB_OPERAND_INDEX_1D), index};
}

static Words
selected(D3D10_SB_OPERAND_TYPE type, uint32_t index, uint32_t component) {
  return {four(type, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(component), D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE,
               D3D10_SB_OPERAND_INDEX_1D),
          index};
}

static Words
swizzled(D3D10_SB_OPERAND_TYPE type, uint32_t index, uint32_t swizzle) {
  return {four(type, swizzle, D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE, D3D10_SB_OPERAND_INDEX_1D), index};
}

static Words
thread_x() {
  return {four(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(0),
               D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE, D3D10_SB_OPERAND_INDEX_0D)};
}

static Words
immediate(uint32_t value) {
  return {uint32_t(ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT) |
                   ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32)),
          value};
}

static Words
empty(D3D10_SB_OPERAND_TYPE type, uint32_t index) {
  return {uint32_t(ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT) |
                   ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D)),
          index};
}

// cs_5_0:
//   dcl_constantbuffer cb0[4], dynamicIndexed
//   dcl_resource_structured t0, 4
//   dcl_uav_structured u0, 4
//   dcl_input vThreadID.x
//   dcl_temps 2
//   dcl_thread_group 1, 1, 1
//   mov r1.x, vThreadID.x
//   mov r1.y, l(0)
//   ld_structured r0.x, vThreadID.x, l(0), t0.xxxx
//   iadd r0.x, r0.x, cb0[r1.x + 0].x
//   store_structured u0.x, vThreadID.x, l(0), r0.x
//   atomic_iadd u0, r1.xy, r0.x
//   ret
static Words
program() {
  using namespace std;
  constexpr uint32_t x = D3D10_SB_OPERAND_4_COMPONENT_MASK_X, y = D3D10_SB_OPERAND_4_COMPONENT_MASK_Y;
  Words p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  // cb0[4], dynamically indexed: a 2D operand, the register and its size
  instruction(
      p, D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER,
      {{four(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE,
             D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE, D3D10_SB_OPERAND_INDEX_2D),
        0, 4}},
      ENCODE_D3D10_SB_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED)
  );
  instruction(p, D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED, {empty(D3D10_SB_OPERAND_TYPE_RESOURCE, 0), {4}});
  instruction(p, D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED,
              {empty(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0), {4}});
  instruction(p, D3D10_SB_OPCODE_DCL_INPUT,
              {{four(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID, x, D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE,
                     D3D10_SB_OPERAND_INDEX_0D)}});
  instruction(p, D3D10_SB_OPCODE_DCL_TEMPS, {{2}});
  instruction(p, D3D11_SB_OPCODE_DCL_THREAD_GROUP, {{1, 1, 1}});
  instruction(p, D3D10_SB_OPCODE_MOV, {masked(D3D10_SB_OPERAND_TYPE_TEMP, 1, x), thread_x()});
  instruction(p, D3D10_SB_OPCODE_MOV, {masked(D3D10_SB_OPERAND_TYPE_TEMP, 1, y), immediate(0)});
  instruction(p, D3D11_SB_OPCODE_LD_STRUCTURED,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, x), thread_x(), immediate(0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 0, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
  // cb0[r1.x + 0].x: the register immediate, the element relative to a temp (the converter takes no other)
  Words element{four(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(0),
                     D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE, D3D10_SB_OPERAND_INDEX_2D) |
                    ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
                    ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE),
                0, 0};
  auto relative = selected(D3D10_SB_OPERAND_TYPE_TEMP, 1, 0);
  element.insert(element.end(), relative.begin(), relative.end());
  instruction(p, D3D10_SB_OPCODE_IADD,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, x), selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0), element});
  instruction(p, D3D11_SB_OPCODE_STORE_STRUCTURED,
              {masked(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0, x), thread_x(), immediate(0),
               selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0)});
  instruction(p, D3D11_SB_OPCODE_ATOMIC_IADD,
              {masked(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0, x),
               swizzled(D3D10_SB_OPERAND_TYPE_TEMP, 1, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE(0, 1, 1, 1)),
               selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0)});
  instruction(p, D3D10_SB_OPCODE_RET);
  p[1] = p.size();
  return p;
}

// cs_5_0, the loads of root SRVs whose bounds checks can fail, one of each path the converter takes:
//   dcl_resource_structured t0, 16
//   dcl_resource_raw t1
//   dcl_uav_structured u0, 16
//   dcl_input vThreadID.x
//   dcl_temps 2
//   dcl_thread_group 1, 1, 1
//   ld_structured r0.x, vThreadID.x, l(0), t0.xxxx       ; a constant offset: one compare for the element
//   ld_structured r1.xyzw, vThreadID.x, r0.x, t0.xyzw    ; an offset the shader computes: four units, one bound
//   ld_raw r0.y, r0.x, t1.xxxx                           ; a raw load: its own bound
//   iadd r1.x, r1.x, r0.y
//   store_structured u0.xyzw, vThreadID.x, l(0), r1.xyzw
//   ret
static Words
fallback_program() {
  constexpr uint32_t x = D3D10_SB_OPERAND_4_COMPONENT_MASK_X, y = D3D10_SB_OPERAND_4_COMPONENT_MASK_Y,
                     all = D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL;
  Words p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  instruction(p, D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED, {empty(D3D10_SB_OPERAND_TYPE_RESOURCE, 0), {16}});
  instruction(p, D3D11_SB_OPCODE_DCL_RESOURCE_RAW, {empty(D3D10_SB_OPERAND_TYPE_RESOURCE, 1)});
  instruction(p, D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED,
              {empty(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0), {16}});
  instruction(p, D3D10_SB_OPCODE_DCL_INPUT,
              {{four(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID, x, D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE,
                     D3D10_SB_OPERAND_INDEX_0D)}});
  instruction(p, D3D10_SB_OPCODE_DCL_TEMPS, {{2}});
  instruction(p, D3D11_SB_OPCODE_DCL_THREAD_GROUP, {{1, 1, 1}});
  instruction(p, D3D11_SB_OPCODE_LD_STRUCTURED,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, x), thread_x(), immediate(0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 0, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
  instruction(p, D3D11_SB_OPCODE_LD_STRUCTURED,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 1, all), thread_x(), selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 0, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)});
  instruction(p, D3D11_SB_OPCODE_LD_RAW,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, y), selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 1, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
  instruction(p, D3D10_SB_OPCODE_IADD,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 1, x), selected(D3D10_SB_OPERAND_TYPE_TEMP, 1, 0),
               selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 1)});
  instruction(p, D3D11_SB_OPCODE_STORE_STRUCTURED,
              {masked(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0, all), thread_x(), immediate(0),
               swizzled(D3D10_SB_OPERAND_TYPE_TEMP, 1, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)});
  instruction(p, D3D10_SB_OPCODE_RET);
  p[1] = p.size();
  return p;
}

// a container of parts, each a name and its words, signed with the hash Direct3D checks
static std::string
container(std::initializer_list<std::pair<const char *, Words>> parts) {
  std::string out("DXBC", 4);
  out.append(16, '\0'); // the hash, written last
  auto put = [&](uint32_t value) { out.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  put(1);    // version 1.0
  put(0);    // the size, written below
  put(parts.size());
  uint32_t at = 32 + 4 * parts.size();
  for (auto &[name, words] : parts) {
    put(at);
    at += 8 + 4 * words.size();
  }
  for (auto &[name, words] : parts) {
    out.append(name, 4);
    put(4 * words.size());
    out.append(reinterpret_cast<const char *>(words.data()), 4 * words.size());
  }
  uint32_t size = out.size();
  memcpy(out.data() + 24, &size, sizeof(size));
  dxbc::sign(out);
  return out;
}

// a version 1.1 root signature (RTS0) of `parameters`: each a root descriptor of `kind` (D3D12_ROOT_PARAMETER_TYPE:
// 2 CBV, 3 SRV, 4 UAV) at `reg`, or a descriptor table (0) of volatile ranges of `kinds` (D3D12_DESCRIPTOR_RANGE_TYPE:
// 0 SRV, 1 UAV, 2 CBV), one descriptor each at register 0
struct Parameter {
  uint32_t kind;
  std::vector<uint32_t> ranges;
  uint32_t reg = 0;
};

static Words
root_signature(std::initializer_list<Parameter> parameters) {
  constexpr uint32_t version_1_1 = 2, all = 0, volatile_ = 1, append = 0xffffffff;
  Words header{version_1_1, uint32_t(parameters.size()), 24, 0, 0, 0}, entries, payloads;
  uint32_t at = 24 + 12 * parameters.size();
  for (auto &parameter : parameters) {
    entries.insert(entries.end(), {parameter.kind, all, uint32_t(at + 4 * payloads.size())});
    if (parameter.kind) {
      payloads.insert(payloads.end(), {parameter.reg, 0, 0}); // register, space, flags
      continue;
    }
    uint32_t ranges = at + 4 * (payloads.size() + 2);
    payloads.insert(payloads.end(), {uint32_t(parameter.ranges.size()), ranges});
    for (auto kind : parameter.ranges)
      payloads.insert(payloads.end(), {kind, 1, 0, 0, volatile_, append});
  }
  header.insert(header.end(), entries.begin(), entries.end());
  header.insert(header.end(), payloads.begin(), payloads.end());
  return header;
}

static std::string
read(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

static size_t
count(const std::string &text, const std::string &what) {
  size_t n = 0;
  for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
    n++;
  return n;
}

// the loads of one function of LLVM assembly whose address comes from a root view: a pointer the function loads from
// one of `views`, fields of its argument buffer struct. each is `guarded` when its address passes a select between a
// pointer from the root view and one from field `locks`, the lock table, which a load whose check fails reads from.
// the trace follows definitions by name, through getelementptr, casts and selects, not by their order or numbering
struct Loads {
  unsigned from_views = 0, unguarded = 0;
};

static Loads
trace(const std::string &ir, std::initializer_list<uint32_t> views, uint32_t locks) {
  struct Definition {
    std::string op;
    std::vector<std::string> operands; // the values it uses, in order
    int field = -1;                    // a getelementptr of the argument buffer struct: its field
  };
  std::map<std::string, Definition> defs;
  std::vector<std::string> loads;
  std::istringstream lines(ir);
  bool in_function = false;
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("define ", 0) == 0) {
      in_function = true;
      continue;
    }
    if (line == "}")
      in_function = false;
    auto eq = line.find(" = ");
    if (!in_function || line.size() < 3 || line.compare(0, 3, "  %") || eq == std::string::npos)
      continue;
    auto name = line.substr(2, eq - 2);
    std::istringstream words(line.substr(eq + 3));
    Definition def;
    words >> def.op;
    for (std::string word; words >> word;) {
      while (!word.empty() && (word.back() == ',' || word.back() == ')'))
        word.pop_back();
      if (word.size() > 1 && word[0] == '%')
        def.operands.push_back(word);
    }
    if (def.op == "getelementptr" && line.find("%argument_buffer_struct, ") != std::string::npos) {
      auto at = line.rfind("i32 ");
      def.field = std::stoi(line.substr(at + 4));
    }
    if (def.op == "load")
      loads.push_back(name);
    defs[name] = def;
  }
  // the values a pointer is a view or the lock table by: a load from that field
  std::function<int(const std::string &)> field_of = [&](const std::string &value) -> int {
    auto it = defs.find(value);
    if (it == defs.end() || it->second.op != "load")
      return -1;
    for (auto &operand : it->second.operands)
      if (auto at = defs.find(operand); at != defs.end() && at->second.field >= 0)
        return at->second.field;
    return -1;
  };
  // which fields a pointer comes from, and whether it passed a select of a view and the lock table on the way
  std::function<void(const std::string &, std::set<int> &, bool &)> reach = [&](const std::string &value,
                                                                                std::set<int> &fields, bool &guarded) {
    if (int field = field_of(value); field >= 0) {
      fields.insert(field);
      return;
    }
    auto it = defs.find(value);
    if (it == defs.end())
      return;
    auto &def = it->second;
    if (def.op == "select" && def.operands.size() == 3) {
      std::set<int> a, b;
      bool ga = false, gb = false;
      reach(def.operands[1], a, ga);
      reach(def.operands[2], b, gb);
      auto view = [&](const std::set<int> &f) {
        return std::any_of(views.begin(), views.end(), [&](uint32_t v) { return f.count(v); });
      };
      guarded = guarded || (view(a) && b.count(locks)) || (view(b) && a.count(locks)) || ga || gb;
      fields.insert(a.begin(), a.end());
      fields.insert(b.begin(), b.end());
      return;
    }
    if ((def.op == "getelementptr" || def.op == "bitcast" || def.op == "addrspacecast") && !def.operands.empty())
      reach(def.operands[0], fields, guarded);
  };
  Loads out;
  for (auto &load : loads) {
    if (defs[load].operands.empty())
      continue;
    std::set<int> fields;
    bool guarded = false;
    reach(defs[load].operands[0], fields, guarded);
    if (!std::any_of(views.begin(), views.end(), [&](uint32_t v) { return fields.count(v); }))
      continue;
    out.from_views++;
    out.unguarded += !guarded;
  }
  return out;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <airconv>\n", argv[0]);
    return 2;
  }
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / ("host_check_root_ranges." + std::to_string(::getpid()));
  fs::create_directories(dir);
  auto write = [&](const std::string &name, const std::string &bytes) {
    std::ofstream(dir / name, std::ios::binary) << bytes;
  };
  constexpr uint32_t table = 0, cbv = 2, srv = 3, uav = 4, range_srv = 0, range_uav = 1, range_cbv = 2;
  write("shader.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"SHEX", program()}}));
  write("table.rs", container({{"RTS0", root_signature({{table, {range_cbv, range_srv, range_uav}}})}}));
  write("cbv.rs", container({{"RTS0", root_signature({{cbv, {}}, {table, {range_srv, range_uav}}})}}));
  write("srv.rs", container({{"RTS0", root_signature({{srv, {}}, {table, {range_cbv, range_uav}}})}}));
  write("uav.rs", container({{"RTS0", root_signature({{uav, {}}, {table, {range_cbv, range_srv}}})}}));

  // the converter's LLVM assembly for root signature `name`, with or without the flag
  auto convert = [&](const std::string &name, bool checked) {
    std::string out = name + (checked ? ".checked.ll" : ".ll");
    std::string command = "\"" + std::string(argv[1]) + "\" -S" + (checked ? " -check-root-ranges" : "") +
                          " -root-signature \"" + (dir / (name + ".rs")).string() + "\" -o \"" +
                          (dir / out).string() + "\" \"" + (dir / "shader.dxbc").string() + "\"";
    if (std::system(command.c_str()) != 0) {
      std::printf("FAILED: %s\n", command.c_str());
      failures++;
      return std::string();
    }
    return read(dir / out);
  };
  auto table_plain = convert("table", false), table_checked = convert("table", true);
  if (!failures)
    check(!table_plain.empty() && table_plain == table_checked, "a table alone converts the same with the flag");
  for (auto [name, what] : {std::pair{"cbv", "a root CBV is checked with the flag"},
                            std::pair{"srv", "a root SRV is checked with the flag"},
                            std::pair{"uav", "a root UAV is checked with the flag"}}) {
    auto plain = convert(name, false), checked = convert(name, true);
    if (plain.empty() || checked.empty())
      continue;
    auto before = count(plain, "icmp ult"), after = count(checked, "icmp ult");
    std::printf("%s: %zu bounds compares without the flag, %zu with it\n", name, before, after);
    check(after > before, what);
  }
  // the loads of root SRVs whose checks can fail: with the flag every one reads a failed check's value from the lock
  // table, not from the view's start, which may have fewer bytes than one load; without it they read the view
  // (fields: t0 0, t1 1, u0 2, the lock table 3, then the three lengths)
  write("fallback.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"SHEX", fallback_program()}}));
  write("fallback.rs", container({{"RTS0", root_signature({{srv, {}, 0}, {srv, {}, 1}, {uav, {}, 0}})}}));
  auto convert_fallback = [&](bool checked) {
    std::string out = checked ? "fallback.checked.ll" : "fallback.ll";
    std::string command = "\"" + std::string(argv[1]) + "\" -S" + (checked ? " -check-root-ranges" : "") +
                          " -root-signature \"" + (dir / "fallback.rs").string() + "\" -o \"" + (dir / out).string() +
                          "\" \"" + (dir / "fallback.dxbc").string() + "\"";
    if (std::system(command.c_str()) != 0) {
      std::printf("FAILED: %s\n", command.c_str());
      failures++;
      return std::string();
    }
    return read(dir / out);
  };
  auto plain = convert_fallback(false), checked = convert_fallback(true);
  if (!plain.empty() && !checked.empty()) {
    auto before = trace(plain, {0, 1}, 3), after = trace(checked, {0, 1}, 3);
    std::printf("fallback: %u loads of root SRVs, %u unguarded without the flag; %u, %u unguarded with it\n",
                before.from_views, before.unguarded, after.from_views, after.unguarded);
    check(before.unguarded > 0, "without the flag the loads of root SRVs read the view, as the trace can tell");
    check(after.from_views >= 6, "the shader's six loads of root SRVs are found with the flag");
    check(after.from_views && !after.unguarded, "every load past a root SRV reads the lock table with the flag");
  }
  // MULLION_KEEP_OUTPUT: the outputs stay, for a pull request to attach
  if (std::getenv("MULLION_KEEP_OUTPUT"))
    std::printf("outputs in %s\n", dir.c_str());
  else
    fs::remove_all(dir);
  return failures ? 1 : 0;
}
