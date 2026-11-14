// jdrly/mullion: MULLION_D3D12_GUARD_STATIC_RANGES (SM50_SHADER_FLAG_GUARD_STATIC_RANGES) in the converter's output,
// without a GPU. a compute shader reads a constant buffer by a dynamic index, loads from a structured SRV, stores to a
// structured UAV and adds to it atomically, all through one descriptor table. the native airconv writes it as LLVM
// assembly for that table's ranges marked static and marked volatile, each with and without -guard-static-ranges.
// held to:
// - volatile, with or without the flag: one output. the flag changes nothing upstream already guards;
// - static without the flag differs from volatile: upstream drops the checks there (6e4b44d), so this test can tell;
// - static with the flag is volatile's output, byte for byte: every check a volatile range has is back.
//
//   host_guard_static_ranges <airconv>
//
// the shader and the root signature are made here, token by token, so no compiler or binary fixture is needed.
#include "airconv_host.hpp"
#include <cstdio>
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

// a version 1.1 root signature (RTS0): one descriptor table of b0, t0 and u0, one descriptor each, its ranges' flags
// `flags` (D3D12_DESCRIPTOR_RANGE_FLAGS: 0 static, 1 volatile descriptors)
static Words
root_signature(uint32_t flags) {
  constexpr uint32_t version_1_1 = 2, table = 0, all = 0, srv = 0, uav = 1, cbv = 2, append = 0xffffffff;
  // the signature (6 words), its parameter (3), the table (2), its ranges (6 each)
  return {version_1_1, 1, 24, 0, 0, 0,
          table, all, 36,
          3, 44,
          cbv, 1, 0, 0, flags, append,
          srv, 1, 0, 0, flags, append,
          uav, 1, 0, 0, flags, append};
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <airconv>\n", argv[0]);
    return 2;
  }
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / ("host_guard_static_ranges." + std::to_string(::getpid()));
  fs::create_directories(dir);
  auto write = [&](const char *name, const std::string &bytes) {
    std::ofstream(dir / name, std::ios::binary) << bytes;
  };
  write("shader.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"SHEX", program()}}));
  write("static.rs", container({{"RTS0", root_signature(0)}}));
  write("volatile.rs", container({{"RTS0", root_signature(1)}}));

  // the converter's LLVM assembly for the table's ranges `ranges`, with or without the flag
  auto convert = [&](const char *ranges, bool guard) {
    std::string out = std::string(ranges) + (guard ? ".guarded.ll" : ".ll");
    std::string command = "\"" + std::string(argv[1]) + "\" -S" + (guard ? " -guard-static-ranges" : "") +
                          " -root-signature \"" + (dir / (std::string(ranges) + ".rs")).string() + "\" -o \"" +
                          (dir / out).string() + "\" \"" + (dir / "shader.dxbc").string() + "\"";
    if (std::system(command.c_str()) != 0) {
      std::printf("FAILED: %s\n", command.c_str());
      failures++;
      return std::string();
    }
    return read(dir / out);
  };
  auto unguarded = convert("static", false), guarded = convert("static", true);
  auto volatile_ = convert("volatile", false), volatile_flagged = convert("volatile", true);
  if (!failures) {
    check(!volatile_.empty() && volatile_ == volatile_flagged, "volatile ranges convert the same with the flag");
    check(unguarded != volatile_, "static ranges without the flag convert without volatile ranges' checks");
    check(guarded == volatile_, "static ranges with the flag convert as volatile ranges do");
  }
  // MULLION_KEEP_OUTPUT: the four outputs stay, for a pull request to attach
  if (std::getenv("MULLION_KEEP_OUTPUT"))
    std::printf("outputs in %s\n", dir.c_str());
  else
    fs::remove_all(dir);
  return failures ? 1 : 0;
}
