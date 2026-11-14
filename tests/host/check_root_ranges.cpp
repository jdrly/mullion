// jdrly/mullion: MULLION_D3D12_CHECK_RANGES (SM50_SHADER_FLAG_CHECK_ROOT_RANGES) in the converter's output, without a
// GPU. the compute shader of host_guard_static_ranges reads a constant buffer by a dynamic index, loads from a
// structured SRV, stores to a structured UAV and adds to it atomically. the native airconv writes it as LLVM assembly
// for root signatures that bind one of the three as a root descriptor and the others through a volatile descriptor
// table, and for one that binds all three through the table, each with and without -check-root-ranges. held to:
// - the table alone: one output with or without the flag. the flag changes nothing but root descriptors;
// - a root CBV, SRV or UAV: more bounds compares (`icmp ult`) with the flag than without. upstream checks a root
//   descriptor against nothing, and the flag checks each against the length its root arguments carry.
// - a constant buffer, SRV or UAV through a descriptor table, the other two root descriptors: more bounds compares with
//   -check-descriptor-indices (SM50_SHADER_FLAG_CHECK_DESCRIPTOR_INDICES) than without, each kind on its own. its
//   descriptor index is clamped below the count of descriptors the heap has from the table's start.
// - a check any of the three flags adds counts where it fails (private ticket 33): a shader with one access down each
//   path a check takes (a constant buffer's row, a raw load, a structured load at a constant offset and at one it
//   computes, a structured store) is converted with root views (-check-root-ranges), with a table of static ranges
//   (-guard-static-ranges) and with a volatile table (-check-descriptor-indices). Each kind's count is one atomic add
//   to the kind's word of the lock table, behind a branch on the bounds compares of that kind's checks: every one of
//   them, and nothing that passes reaches the atomic. Without the flags nothing counts.
//
//   host_check_root_ranges <airconv>
//
// the shader and the root signatures are made here, token by token, so no compiler or binary fixture is needed.
#include "airconv_host.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

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

// cs_5_0, the descriptors the other shaders don't fetch through a table: a texture, a sampler and a UAV's counter
//   dcl_sampler s0, mode_default
//   dcl_resource_texture2d (float,float,float,float) t0
//   dcl_uav_structured u0, 4 (with an order-preserving counter)
//   dcl_temps 2
//   dcl_thread_group 1, 1, 1
//   sample_l r0.x, l(0.5, 0.5, 0, 0), t0.xyzw, s0, l(0)
//   imm_atomic_alloc r1.x, u0
//   store_structured u0.x, r1.x, l(0), r0.x
//   ret
static Words
heap_program() {
  constexpr uint32_t x = D3D10_SB_OPERAND_4_COMPONENT_MASK_X;
  Words p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  instruction(p, D3D10_SB_OPCODE_DCL_SAMPLER, {empty(D3D10_SB_OPERAND_TYPE_SAMPLER, 0)},
              ENCODE_D3D10_SB_SAMPLER_MODE(D3D10_SB_SAMPLER_MODE_DEFAULT));
  uint32_t returns = 0;
  for (uint32_t c = 0; c < 4; c++)
    returns |= ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, c);
  instruction(p, D3D10_SB_OPCODE_DCL_RESOURCE, {empty(D3D10_SB_OPERAND_TYPE_RESOURCE, 0), {returns}},
              ENCODE_D3D10_SB_RESOURCE_DIMENSION(D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D));
  instruction(p, D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED,
              {empty(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0), {4}}, D3D11_SB_UAV_HAS_ORDER_PRESERVING_COUNTER);
  instruction(p, D3D10_SB_OPCODE_DCL_TEMPS, {{2}});
  instruction(p, D3D11_SB_OPCODE_DCL_THREAD_GROUP, {{1, 1, 1}});
  const float half = 0.5f;
  uint32_t h;
  memcpy(&h, &half, sizeof(h));
  Words coordinates{four(D3D10_SB_OPERAND_TYPE_IMMEDIATE32, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE,
                         D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE, D3D10_SB_OPERAND_INDEX_0D),
                    h, h, 0, 0};
  instruction(p, D3D10_SB_OPCODE_SAMPLE_L,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, x), coordinates,
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 0, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE),
               empty(D3D10_SB_OPERAND_TYPE_SAMPLER, 0), immediate(0)});
  instruction(p, D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 1, x), empty(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0)});
  instruction(p, D3D11_SB_OPCODE_STORE_STRUCTURED,
              {masked(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0, x), selected(D3D10_SB_OPERAND_TYPE_TEMP, 1, 0),
               immediate(0), selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0)});
  instruction(p, D3D10_SB_OPCODE_RET);
  p[1] = p.size();
  return p;
}

// cs_5_0, an access down each path a bounds check takes, each against its own bound:
//   dcl_constantbuffer cb0[4], dynamicIndexed
//   dcl_resource_structured t0, 16
//   dcl_resource_raw t1
//   dcl_uav_structured u0, 16
//   dcl_input vThreadID.x
//   dcl_temps 3
//   dcl_thread_group 1, 1, 1
//   mov r1.x, vThreadID.x
//   ld_structured r0.x, vThreadID.x, l(0), t0.xxxx       ; a constant offset: one compare for the element
//   ld_structured r2.xyzw, vThreadID.x, r0.x, t0.xyzw    ; an offset the shader computes: four units, one bound
//   ld_raw r0.y, r0.x, t1.xxxx                           ; a raw load: its own bound
//   iadd r2.x, r2.x, r0.y
//   iadd r2.x, r2.x, cb0[r1.x + 0].x                     ; a constant buffer's row
//   store_structured u0.x, vThreadID.x, l(0), r2.x       ; a store
//   ret
static Words
counted_program() {
  constexpr uint32_t x = D3D10_SB_OPERAND_4_COMPONENT_MASK_X, y = D3D10_SB_OPERAND_4_COMPONENT_MASK_Y,
                     all = D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL;
  Words p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D11_SB_COMPUTE_SHADER, 5, 0), 0};
  instruction(
      p, D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER,
      {{four(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE,
             D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE, D3D10_SB_OPERAND_INDEX_2D),
        0, 4}},
      ENCODE_D3D10_SB_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED)
  );
  instruction(p, D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED, {empty(D3D10_SB_OPERAND_TYPE_RESOURCE, 0), {16}});
  instruction(p, D3D11_SB_OPCODE_DCL_RESOURCE_RAW, {empty(D3D10_SB_OPERAND_TYPE_RESOURCE, 1)});
  instruction(p, D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED,
              {empty(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0), {16}});
  instruction(p, D3D10_SB_OPCODE_DCL_INPUT,
              {{four(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID, x, D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE,
                     D3D10_SB_OPERAND_INDEX_0D)}});
  instruction(p, D3D10_SB_OPCODE_DCL_TEMPS, {{3}});
  instruction(p, D3D11_SB_OPCODE_DCL_THREAD_GROUP, {{1, 1, 1}});
  instruction(p, D3D10_SB_OPCODE_MOV, {masked(D3D10_SB_OPERAND_TYPE_TEMP, 1, x), thread_x()});
  instruction(p, D3D11_SB_OPCODE_LD_STRUCTURED,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, x), thread_x(), immediate(0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 0, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
  instruction(p, D3D11_SB_OPCODE_LD_STRUCTURED,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 2, all), thread_x(), selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 0, D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)});
  instruction(p, D3D11_SB_OPCODE_LD_RAW,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 0, y), selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0),
               swizzled(D3D10_SB_OPERAND_TYPE_RESOURCE, 1, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
  instruction(p, D3D10_SB_OPCODE_IADD,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 2, x), selected(D3D10_SB_OPERAND_TYPE_TEMP, 2, 0),
               selected(D3D10_SB_OPERAND_TYPE_TEMP, 0, 1)});
  Words element{four(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(0),
                     D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE, D3D10_SB_OPERAND_INDEX_2D) |
                    ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
                    ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE),
                0, 0};
  auto relative = selected(D3D10_SB_OPERAND_TYPE_TEMP, 1, 0);
  element.insert(element.end(), relative.begin(), relative.end());
  instruction(p, D3D10_SB_OPCODE_IADD,
              {masked(D3D10_SB_OPERAND_TYPE_TEMP, 2, x), selected(D3D10_SB_OPERAND_TYPE_TEMP, 2, 0), element});
  instruction(p, D3D11_SB_OPCODE_STORE_STRUCTURED,
              {masked(D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW, 0, x), thread_x(), immediate(0),
               selected(D3D10_SB_OPERAND_TYPE_TEMP, 2, 0)});
  instruction(p, D3D10_SB_OPCODE_RET);
  p[1] = p.size();
  return p;
}

// a version 1.1 root signature (RTS0) of `parameters`: each a root descriptor of `kind` (D3D12_ROOT_PARAMETER_TYPE:
// 2 CBV, 3 SRV, 4 UAV) at `reg`, or a descriptor table (0) of volatile or static ranges of `kinds`
// (D3D12_DESCRIPTOR_RANGE_TYPE: 0 SRV, 1 UAV, 2 CBV), one descriptor each at its register
struct Parameter {
  uint32_t kind;
  std::vector<uint32_t> ranges;
  uint32_t reg = 0;
  std::vector<uint32_t> registers = {}; // a table's ranges' registers, 0 for each without one
  bool is_static = false;               // a table's ranges are static (flags 0), not volatile
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
    for (size_t i = 0; i < parameter.ranges.size(); i++)
      payloads.insert(payloads.end(), {parameter.ranges[i], 1, i < parameter.registers.size() ? parameter.registers[i] : 0,
                                       0, parameter.is_static ? 0 : volatile_, append});
  }
  header.insert(header.end(), entries.begin(), entries.end());
  header.insert(header.end(), payloads.begin(), payloads.end());
  return header;
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
    // the shader's own function: helpers after it reuse its names
    if (line.rfind("define ", 0) == 0) {
      in_function = line.find("@shader_main(") != std::string::npos;
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
      if (word.size() > 1 && word[0] == '%' && word.rfind("%struct.", 0) != 0)
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

// the loads of descriptors through the table at field `table` of one function's argument buffer struct, and how many of
// them have an index that depends on field `count`, the table's descriptor count (-check-descriptor-indices). a
// descriptor's address is a getelementptr of the table's pointer, whose indices the clamp computes from the count; the
// optimizer folds the clamp into whatever form it likes, so the test follows what the indices depend on, by name
struct Descriptors {
  unsigned loads = 0, clamped = 0;
};

static Descriptors
descriptors(const std::string &ir, int table, int count) {
  struct Definition {
    std::string op;
    std::vector<std::string> operands;
    int field = -1; // the struct field a getelementptr or bitcast of the argument buffer struct points at
  };
  std::map<std::string, Definition> defs;
  std::vector<std::string> loads;
  std::string arguments; // the argument buffer struct's argument
  std::istringstream lines(ir);
  bool in_function = false;
  for (std::string line; std::getline(lines, line);) {
    // the shader's own function: helpers after it reuse its names
    if (line.rfind("define ", 0) == 0) {
      in_function = line.find("@shader_main(") != std::string::npos;
      if (!in_function)
        continue;
      auto at = line.find("%argument_buffer_struct addrspace(2)* %");
      if (at != std::string::npos) {
        arguments = line.substr(line.find('%', at + 1));
        arguments = arguments.substr(0, arguments.find_first_of(",)"));
      }
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
      // a value, not a named type (%argument_buffer_struct, %struct._texture_2d_t and their kind)
      if (word.size() > 1 && word[0] == '%' && word.find("argument_buffer_struct") == std::string::npos &&
          word.rfind("%struct.", 0) != 0)
        def.operands.push_back(word);
    }
    if (def.op == "getelementptr" && line.find("%argument_buffer_struct, ") != std::string::npos)
      def.field = std::stoi(line.substr(line.rfind("i32 ") + 4));
    else if (def.op == "bitcast" && !def.operands.empty() && def.operands[0] == arguments)
      def.field = 0;
    if (def.op == "load")
      loads.push_back(name);
    defs[name] = def;
  }
  // the field a value was loaded from, through bitcasts of the field's address, -1 for none
  auto field_of = [&](const std::string &value) {
    auto it = defs.find(value);
    if (it == defs.end() || it->second.op != "load" || it->second.operands.empty())
      return -1;
    for (auto at = defs.find(it->second.operands[0]); at != defs.end(); at = defs.find(at->second.operands[0])) {
      if (at->second.field >= 0)
        return at->second.field;
      if (at->second.op != "bitcast" || at->second.operands.empty())
        break;
    }
    return -1;
  };
  // whether a value depends on a load of field `field`, through any arithmetic, compare, cast or select
  std::set<std::string> seen;
  std::function<bool(const std::string &, int)> depends = [&](const std::string &value, int field) -> bool {
    if (field_of(value) == field)
      return true;
    auto it = defs.find(value);
    if (it == defs.end() || it->second.op == "load" || !seen.insert(value + "@" + std::to_string(field)).second)
      return false;
    for (auto &operand : it->second.operands)
      if (depends(operand, field))
        return true;
    return false;
  };
  // a load's address: the getelementptrs and casts down to a pointer loaded from a field; through the table, whether an
  // index on the way depends on the count
  Descriptors out;
  for (auto &load : loads) {
    if (defs[load].operands.empty())
      continue;
    bool through_table = false, clamped = false;
    std::string at = defs[load].operands[0];
    for (int steps = 0; steps < 16; steps++) {
      if (field_of(at) == table) {
        through_table = true;
        break;
      }
      auto it = defs.find(at);
      if (it == defs.end() || it->second.operands.empty())
        break;
      auto &def = it->second;
      if (def.op == "getelementptr")
        for (size_t i = 1; i < def.operands.size(); i++) {
          seen.clear();
          clamped = clamped || depends(def.operands[i], count);
        }
      else if (def.op != "bitcast" && def.op != "addrspacecast")
        break;
      at = def.operands[0];
    }
    if (!through_table)
      continue;
    out.loads++;
    out.clamped += clamped;
  }
  return out;
}

// the checks one function counts (SM50_CHECK_KIND): each atomic add to a word of the lock table at field `locks`, by its
// kind, and the bounds compares the nearest branch that guards it depends on (its leaves). A word's address is the
// table, indexed by the table's word 0 (its mask) plus 3 and the kind. A guard's leaves are the compares reached from
// its condition through the logic that gathers a function's checks (select, and, or, the test of a kind's bit),
// stopping at an ordering compare (`icmp ult` and the like), which is a bounds check's. `bounds` lists, for the fields
// given, every ordering compare that depends on a load of one of them: the checks against a root view's length or a
// table's count. A guard is then evaluated: with every bounds compare passing its branch has to go the other way than
// the atomic, and with any one of its leaves failing alone, the atomic's way. All by name, as the trace above
struct Counted {
  std::map<int, unsigned> atomics;              // per kind
  std::map<int, std::set<std::string>> leaves;  // per kind, of every atomic's guard
  std::map<int, unsigned> unguarded;            // per kind: atomics no conditional branch leads to
  std::map<int, unsigned> misread;              // per kind: atomics a passing check reaches or a failing one doesn't
  std::set<std::string> bounds;
};

static Counted
counted(const std::string &ir, int locks, std::initializer_list<int> fields = {}) {
  struct Definition {
    std::string op, predicate, block;
    std::vector<std::string> operands;
    std::vector<long long> constants; // integer constants among its operands
    int field = -1;
    bool from_i1 = false; // a zext of an i1, which is bit 0 only
    std::vector<std::string> values; // its operands in order, names and literals (true, false, integers)
  };
  struct Block {
    std::vector<std::string> preds;
    std::string condition; // of its conditional branch, else ""
    std::string taken;     // the block its conditional branch goes to when the condition holds
  };
  std::map<std::string, Definition> defs;
  std::map<std::string, Block> blocks;
  std::vector<std::pair<std::string, std::string>> adds; // an atomic add's address and its block
  std::istringstream lines(ir);
  bool in_function = false;
  std::string block;
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("define ", 0) == 0) {
      in_function = line.find("@shader_main(") != std::string::npos;
      block = "entry";
      continue;
    }
    if (line == "}")
      in_function = false;
    if (!in_function || line.empty())
      continue;
    if (line[0] != ' ' && line[0] != ';') {
      block = "%" + line.substr(0, line.find(':'));
      if (block == "%entry")
        block = "entry";
      continue;
    }
    // a branch: its targets' predecessor, and its condition
    if (line.rfind("  br ", 0) == 0) {
      std::istringstream words(line);
      std::string word, condition;
      std::vector<std::string> targets;
      for (bool next_label = false; words >> word;) {
        while (!word.empty() && word.back() == ',')
          word.pop_back();
        if (next_label)
          targets.push_back(word == "%entry" ? "entry" : word);
        else if (word[0] == '%')
          condition = word;
        next_label = word == "label";
      }
      for (auto &target : targets)
        blocks[target].preds.push_back(block);
      if (targets.size() == 2)
        blocks[block].condition = condition, blocks[block].taken = targets[0];
      continue;
    }
    if (line.find("@air.atomic.global.add.u.i32(") != std::string::npos) {
      auto at = line.find("* %", line.find("@air.atomic.global.add.u.i32("));
      auto pointer = line.substr(at + 2);
      adds.push_back({pointer.substr(0, pointer.find_first_of(", ")), block});
      continue;
    }
    auto eq = line.find(" = ");
    if (line.compare(0, 3, "  %") || eq == std::string::npos)
      continue;
    auto name = line.substr(2, eq - 2);
    std::istringstream words(line.substr(eq + 3));
    Definition def;
    def.block = block;
    words >> def.op;
    if (def.op == "icmp")
      words >> def.predicate;
    def.from_i1 = (def.op == "zext" || def.op == "sext") && line.find(def.op + " i1 ") != std::string::npos;
    for (std::string word; words >> word;) {
      while (!word.empty() && (word.back() == ',' || word.back() == ')' || word.back() == ']'))
        word.pop_back();
      if (word.size() > 1 && word[0] == '%' && word.find("argument_buffer_struct") == std::string::npos &&
          word.rfind("%struct.", 0) != 0)
        def.operands.push_back(word), def.values.push_back(word);
      else if (!word.empty() && (std::isdigit((unsigned char)word[0]) || (word[0] == '-' && word.size() > 1)))
        def.constants.push_back(std::atoll(word.c_str())), def.values.push_back(word);
      else if (word == "true" || word == "false")
        def.values.push_back(word);
    }
    if (def.op == "getelementptr" && line.find("%argument_buffer_struct, ") != std::string::npos)
      def.field = std::stoi(line.substr(line.rfind("i32 ") + 4));
    defs[name] = def;
  }
  auto def_of = [&](const std::string &value) -> const Definition * {
    auto it = defs.find(value);
    return it == defs.end() ? nullptr : &it->second;
  };
  // the field a value was loaded from, -1 for none
  auto field_of = [&](const std::string &value) {
    auto d = def_of(value);
    if (!d || d->op != "load" || d->operands.empty())
      return -1;
    auto at = def_of(d->operands[0]);
    return at ? at->field : -1;
  };
  // an atomic's kind: its address is the lock table indexed by the table's word 0 plus 3 and the kind
  auto kind_of = [&](const std::string &pointer) {
    auto gep = def_of(pointer);
    if (!gep || gep->op != "getelementptr" || gep->operands.size() != 2 || field_of(gep->operands[0]) != locks)
      return -1;
    auto index = def_of(gep->operands[1]);
    while (index && (index->op == "sext" || index->op == "zext") && !index->operands.empty())
      index = def_of(index->operands[0]);
    if (!index || index->op != "add" || index->operands.size() != 1 || index->constants.size() != 1)
      return -1;
    auto mask = def_of(index->operands[0]);
    if (!mask || mask->op != "load" || mask->operands.empty() || mask->operands[0] != gep->operands[0])
      return -1;
    return int(index->constants[0]) - 3;
  };
  // the leaves of `value`, following only the bits of `mask`: a kind's bit test (`and` with its bit) follows that bit,
  // a select of constants follows its condition when one of them has a bit followed, and an i1 is all its bits
  std::set<std::string> seen;
  std::function<void(const std::string &, uint64_t, std::set<std::string> &)> leaves_of =
      [&](const std::string &value, uint64_t mask, std::set<std::string> &out) {
        auto d = def_of(value);
        if (!d || !mask || !seen.insert(value + "/" + std::to_string(mask)).second)
          return;
        if (d->op == "icmp" && d->predicate != "eq" && d->predicate != "ne") {
          out.insert(value);
          return;
        }
        if (d->from_i1 && d->op == "zext") {
          if (mask & 1)
            leaves_of(d->operands[0], ~0ull, out);
        } else if (d->op == "icmp" || d->op == "freeze" || d->op == "zext" || d->op == "sext" || d->op == "trunc") {
          for (auto &operand : d->operands)
            leaves_of(operand, d->op == "icmp" ? ~0ull : mask, out);
        } else if (d->op == "and" && d->constants.size() == 1) {
          leaves_of(d->operands[0], mask & d->constants[0], out);
        } else if ((d->op == "shl" || d->op == "lshr") && d->constants.size() == 1 && d->operands.size() == 1) {
          leaves_of(d->operands[0], d->op == "shl" ? mask >> d->constants[0] : mask << d->constants[0], out);
        } else if (d->op == "and" || d->op == "or" || d->op == "xor" || d->op == "phi") {
          for (auto &operand : d->operands)
            if (operand.rfind("%", 0) == 0 && !blocks.count(operand))
              leaves_of(operand, mask, out);
        } else if (d->op == "select" && !d->operands.empty()) {
          bool chooses = false;
          for (auto constant : d->constants)
            chooses = chooses || (uint64_t(constant) & mask);
          std::set<std::string> values;
          for (size_t i = 1; i < d->operands.size(); i++)
            leaves_of(d->operands[i], mask, values);
          if (chooses || !values.empty() || d->operands.size() == 3)
            leaves_of(d->operands[0], ~0ull, out);
          out.insert(values.begin(), values.end());
        }
      };
  // a value of the logic that gathers the checks, with each bounds compare passing but those in `failing`; none for a
  // value it can't tell
  std::function<std::optional<uint64_t>(const std::string &, const std::set<std::string> &)> eval =
      [&](const std::string &value, const std::set<std::string> &failing) -> std::optional<uint64_t> {
    if (value == "true" || value == "false")
      return value == "true";
    if (!value.empty() && value[0] != '%')
      return uint64_t(std::atoll(value.c_str()));
    auto d = def_of(value);
    if (!d)
      return std::nullopt;
    if (d->op == "icmp" && d->predicate != "eq" && d->predicate != "ne")
      return !failing.count(value);
    std::vector<uint64_t> v;
    for (auto &operand : d->values)
      if (auto got = eval(operand, failing))
        v.push_back(*got);
      else
        return std::nullopt;
    if (d->op == "icmp" && v.size() == 2)
      return (v[0] == v[1]) == (d->predicate == "eq");
    if (d->op == "select" && v.size() == 3)
      return v[0] & 1 ? v[1] : v[2];
    if ((d->op == "zext" || d->op == "trunc" || d->op == "freeze") && v.size() == 1)
      return v[0];
    if (d->op == "sext" && v.size() == 1)
      return d->from_i1 ? (v[0] & 1 ? ~0ull : 0) : v[0];
    if (v.size() == 2) {
      if (d->op == "and")
        return v[0] & v[1];
      if (d->op == "or")
        return v[0] | v[1];
      if (d->op == "xor")
        return v[0] ^ v[1];
      if (d->op == "shl")
        return v[0] << v[1];
      if (d->op == "lshr")
        return v[0] >> v[1];
    }
    return std::nullopt;
  };
  Counted out;
  for (auto &[pointer, at] : adds) {
    int kind = kind_of(pointer);
    if (kind < 0)
      continue;
    out.atomics[kind]++;
    // the nearest conditional branch on the way to it, through blocks of one predecessor
    std::string guard;
    bool toward = false; // the guard's value on the way to the atomic
    for (auto b = at; blocks[b].preds.size() == 1;) {
      auto from = b;
      b = blocks[b].preds[0];
      if (!blocks[b].condition.empty()) {
        guard = blocks[b].condition;
        toward = blocks[b].taken == from;
        break;
      }
    }
    std::set<std::string> leaves;
    seen.clear();
    if (!guard.empty())
      leaves_of(guard, ~0ull, leaves);
    out.unguarded[kind] += leaves.empty();
    out.leaves[kind].insert(leaves.begin(), leaves.end());
    // every check passing goes the other way; any one of them failing goes the atomic's
    bool right = !leaves.empty();
    auto all_pass = eval(guard, {});
    right = right && all_pass && bool(*all_pass & 1) != toward;
    for (auto &leaf : leaves) {
      auto one_fails = eval(guard, {leaf});
      right = right && one_fails && bool(*one_fails & 1) == toward;
    }
    out.misread[kind] += !right;
  }
  // the ordering compares that depend on a load of one of `fields`
  std::function<bool(const std::string &)> depends = [&](const std::string &value) -> bool {
    if (std::find(fields.begin(), fields.end(), field_of(value)) != fields.end())
      return true;
    auto d = def_of(value);
    if (!d || d->op == "load" || !seen.insert(value).second)
      return false;
    for (auto &operand : d->operands)
      if (depends(operand))
        return true;
    return false;
  };
  for (auto &[name, def] : defs) {
    seen.clear();
    if (def.op == "icmp" && def.predicate != "eq" && def.predicate != "ne" && depends(name))
      out.bounds.insert(name);
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
  // descriptor indices: one kind through a table, the others root descriptors, so that its clamp alone counts
  write("tcbv.rs", container({{"RTS0", root_signature({{table, {range_cbv}}, {srv, {}}, {uav, {}}})}}));
  write("tsrv.rs", container({{"RTS0", root_signature({{table, {range_srv}}, {cbv, {}}, {uav, {}}})}}));
  write("tuav.rs", container({{"RTS0", root_signature({{table, {range_uav}}, {cbv, {}}, {srv, {}}})}}));
  auto convert_indices = [&](const std::string &name, bool clamped) {
    std::string out = name + (clamped ? ".clamped.ll" : ".unclamped.ll");
    std::string command = "\"" + std::string(argv[1]) + "\" -S" + (clamped ? " -check-descriptor-indices" : "") +
                          " -root-signature \"" + (dir / (name + ".rs")).string() + "\" -o \"" + (dir / out).string() +
                          "\" \"" + (dir / "shader.dxbc").string() + "\"";
    if (std::system(command.c_str()) != 0) {
      std::printf("FAILED: %s\n", command.c_str());
      failures++;
      return std::string();
    }
    return read(dir / out);
  };
  // fields: the table 0, two root views 1 and 2, the lock table 3, the table's count 4
  for (auto [name, what] : {std::pair{"tcbv", "a constant buffer's descriptor index is clamped with the flag"},
                            std::pair{"tsrv", "an SRV's descriptor index is clamped with the flag"},
                            std::pair{"tuav", "a UAV's descriptor index is clamped with the flag"}}) {
    auto plain = convert_indices(name, false), clamped = convert_indices(name, true);
    if (plain.empty() || clamped.empty())
      continue;
    auto before = descriptors(plain, 0, 4), after = descriptors(clamped, 0, 4);
    std::printf("%s: %u descriptor loads, %u clamped without the flag; %u, %u clamped with it\n", name, before.loads,
                before.clamped, after.loads, after.clamped);
    check(after.loads && after.clamped == after.loads && !before.clamped, what);
  }
  // a texture, a UAV with its counter through one table, a sampler through another: every descriptor load through
  // either is clamped with the flag. fields: the resource table 0, the sampler table 1, the lock table 2, their counts
  // 3 and 4
  constexpr uint32_t range_sampler = 3;
  write("heaps.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"SHEX", heap_program()}}));
  write("heaps.rs", container({{"RTS0", root_signature({{table, {range_srv, range_uav}}, {table, {range_sampler}}})}}));
  auto convert_heaps = [&](bool clamped) {
    std::string out = clamped ? "heaps.clamped.ll" : "heaps.unclamped.ll";
    std::string command = "\"" + std::string(argv[1]) + "\" -S" + (clamped ? " -check-descriptor-indices" : "") +
                          " -root-signature \"" + (dir / "heaps.rs").string() + "\" -o \"" + (dir / out).string() +
                          "\" \"" + (dir / "heaps.dxbc").string() + "\"";
    if (std::system(command.c_str()) != 0) {
      std::printf("FAILED: %s\n", command.c_str());
      failures++;
      return std::string();
    }
    return read(dir / out);
  };
  if (auto plain = convert_heaps(false), clamped = convert_heaps(true); !plain.empty() && !clamped.empty()) {
    for (auto [field, count, what] :
         {std::tuple{0, 3, "every descriptor load through the resource table is clamped with the flag"},
          std::tuple{1, 4, "every descriptor load through the sampler table is clamped with the flag"}}) {
      auto before = descriptors(plain, field, count), after = descriptors(clamped, field, count);
      std::printf("table %d: %u descriptor loads, %u clamped without the flag; %u, %u clamped with it\n", field,
                  before.loads, before.clamped, after.loads, after.clamped);
      check(after.loads && after.clamped == after.loads && !before.clamped, what);
    }
  }
  // the checks count where they fail (SM50_CHECK_KIND: 0 a static range's, 1 a root view's, 2 a descriptor index's).
  // root views: the CBV, the two SRVs and the UAV, fields 0 to 3, the lock table 4, their lengths 5 to 8. tables: the
  // table 0, the lock table 1, its count 2
  constexpr uint32_t range_static = 0, range_view = 1, range_index = 2;
  write("counted.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"SHEX", counted_program()}}));
  write("views.rs", container({{"RTS0", root_signature({{cbv, {}, 0}, {srv, {}, 0}, {srv, {}, 1}, {uav, {}, 0}})}}));
  // each table's first range is one the shader doesn't use: an index 0's clamp folds into `count == 0`, which says
  // nothing of which way a failed check goes
  write("static.rs", container({{"RTS0", root_signature({{table, {range_srv, range_cbv, range_srv, range_srv, range_uav},
                                                            0, {9, 0, 0, 1, 0}, true}})}}));
  write("volatile.rs", container({{"RTS0", root_signature({{table, {range_srv, range_cbv, range_srv, range_srv, range_uav},
                                                              0, {9, 0, 0, 1, 0}}})}}));
  auto convert_counted = [&](const std::string &name, const std::string &flags) {
    std::string out = name + (flags.empty() ? ".plain.ll" : ".counted.ll");
    std::string command = "\"" + std::string(argv[1]) + "\" -S " + flags + " -root-signature \"" +
                          (dir / (name + ".rs")).string() + "\" -o \"" + (dir / out).string() + "\" \"" +
                          (dir / "counted.dxbc").string() + "\"";
    if (std::system(command.c_str()) != 0) {
      std::printf("FAILED: %s\n", command.c_str());
      failures++;
      return std::string();
    }
    return read(dir / out);
  };
  auto show = [](const char *name, const Counted &c, int kind) {
    std::printf("%s: kind %d, %u atomics, %zu checks guard them, %u unguarded, %u reached the wrong way; %zu bounds "
                "compares of its fields\n",
                name, kind, c.atomics.count(kind) ? c.atomics.at(kind) : 0,
                c.leaves.count(kind) ? c.leaves.at(kind).size() : 0, c.unguarded.count(kind) ? c.unguarded.at(kind) : 0,
                c.misread.count(kind) ? c.misread.at(kind) : 0, c.bounds.size());
  };
  // one atomic per kind that counts, nothing that passes reaches it, and every check of the kind guards it
  auto counts_alone = [](const Counted &c, int kind) {
    return c.atomics.size() == 1 && c.atomics.count(kind) && c.atomics.at(kind) == 1 && !c.unguarded.at(kind) &&
           !c.misread.at(kind);
  };
  std::set<std::string> view_checks;
  if (auto plain = convert_counted("views", ""), checked = convert_counted("views", "-check-root-ranges");
      !plain.empty() && !checked.empty()) {
    auto before = counted(plain, 4), after = counted(checked, 4, {5, 6, 7, 8});
    show("views", after, range_view);
    check(before.atomics.empty(), "without the flag no check of a root view counts");
    check(counts_alone(after, range_view), "a root view's checks count in one atomic, which no check that passes reaches");
    // the constant buffer's row, the raw load, the structured loads at a constant and a computed offset, the store
    check(after.bounds.size() >= 5 && after.leaves[range_view] == after.bounds,
          "every check of a root view counts where it fails");
    view_checks = after.leaves[range_view];
  }
  if (auto plain = convert_counted("static", ""), guarded = convert_counted("static", "-guard-static-ranges");
      !plain.empty() && !guarded.empty()) {
    auto before = counted(plain, 1), after = counted(guarded, 1);
    show("static", after, range_static);
    check(before.atomics.empty(), "without the flag no check of a static range counts");
    check(counts_alone(after, range_static),
          "a static range's checks count in one atomic, which no check that passes reaches");
    check(!view_checks.empty() && after.leaves[range_static].size() == view_checks.size(),
          "every check of a static range counts where it fails, as a root view's do");
  }
  if (auto plain = convert_counted("volatile", ""), guarded = convert_counted("volatile", "-guard-static-ranges");
      !plain.empty() && !guarded.empty())
    check(plain == guarded && counted(plain, 1).atomics.empty(), "a volatile range's checks count nothing");
  if (auto plain = convert_counted("volatile", ""), clamped = convert_counted("volatile", "-check-descriptor-indices");
      !plain.empty() && !clamped.empty()) {
    auto after = counted(clamped, 1, {2});
    show("indices", after, range_index);
    check(counts_alone(after, range_index),
          "a descriptor index's clamps count in one atomic, which no index below the count reaches");
    // a constant index's clamp may fold into a larger one's against the same count: each that remains guards it
    check(!after.leaves[range_index].empty() &&
              std::includes(after.bounds.begin(), after.bounds.end(), after.leaves[range_index].begin(),
                            after.leaves[range_index].end()),
          "a descriptor index's clamps count where they clamp");
  }
  // both, in one function: each kind's atomic is guarded by its own checks
  if (auto both = convert_counted("static", "-guard-static-ranges -check-descriptor-indices"); !both.empty()) {
    auto after = counted(both, 1, {2});
    show("static and indices", after, range_static);
    show("static and indices", after, range_index);
    bool apart = after.atomics.size() == 2 && after.atomics[range_static] == 1 && after.atomics[range_index] == 1 &&
                 !after.unguarded[range_static] && !after.unguarded[range_index] && !after.misread[range_static] &&
                 !after.misread[range_index] &&
                 !after.leaves[range_index].empty() &&
                 std::includes(after.bounds.begin(), after.bounds.end(), after.leaves[range_index].begin(),
                               after.leaves[range_index].end()) &&
                 after.leaves[range_static].size() == view_checks.size();
    check(apart, "a static range's checks and a descriptor index's count apart in one function");
  }
  // MULLION_KEEP_OUTPUT: the outputs stay, for a pull request to attach
  if (std::getenv("MULLION_KEEP_OUTPUT"))
    std::printf("outputs in %s\n", dir.c_str());
  else
    fs::remove_all(dir);
  return failures ? 1 : 0;
}
