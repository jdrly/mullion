// jdrly/mullion: what the host tests that run the native airconv share. shaders and root signatures are made token by
// token, so no compiler or binary fixture is needed, and put in a container signed with the hash Direct3D checks.
#pragma once
#include "../../libs/DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "../dxbc_hash.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using namespace microsoft;
using Words = std::vector<uint32_t>;

inline int failures = 0;

inline void
check(bool ok, const char *what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
  failures += !ok;
}

// an instruction: its opcode token, with its length, then its operands
inline void
instruction(Words &program, uint32_t opcode, std::initializer_list<Words> operands = {}, uint32_t controls = 0) {
  size_t at = program.size();
  program.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(opcode) | controls);
  for (auto &operand : operands)
    program.insert(program.end(), operand.begin(), operand.end());
  program[at] |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(program.size() - at);
}

// an operand of four components, by a mask (a destination) or a swizzle or selection (a source)
inline uint32_t
four(D3D10_SB_OPERAND_TYPE type, uint32_t selection, uint32_t mode, D3D10_SB_OPERAND_INDEX_DIMENSION dimension) {
  return ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
         ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(mode) | selection | ENCODE_D3D10_SB_OPERAND_TYPE(type) |
         ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(dimension);
}

inline Words
masked(D3D10_SB_OPERAND_TYPE type, uint32_t index, uint32_t mask) {
  return {four(type, mask, D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE, D3D10_SB_OPERAND_INDEX_1D), index};
}

inline Words
selected(D3D10_SB_OPERAND_TYPE type, uint32_t index, uint32_t component) {
  return {four(type, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(component), D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE,
               D3D10_SB_OPERAND_INDEX_1D),
          index};
}

inline Words
swizzled(D3D10_SB_OPERAND_TYPE type, uint32_t index, uint32_t swizzle) {
  return {four(type, swizzle, D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE, D3D10_SB_OPERAND_INDEX_1D), index};
}

inline Words
thread_x() {
  return {four(D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID, ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(0),
               D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE, D3D10_SB_OPERAND_INDEX_0D)};
}

inline Words
immediate(uint32_t value) {
  return {uint32_t(ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT) |
                   ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32)),
          value};
}

inline Words
empty(D3D10_SB_OPERAND_TYPE type, uint32_t index) {
  return {uint32_t(ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT) |
                   ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D)),
          index};
}

// a container of parts, each a name and its words, signed with the hash Direct3D checks
inline std::string
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

inline std::string
read(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
