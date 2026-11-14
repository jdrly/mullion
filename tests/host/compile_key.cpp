// jdrly/mullion: the shader cache's key changes with every compile flag, MULLION_D3D12_GUARD_STATIC_RANGES's among
// them, so a shader converted with the guard is never served to a run without it, and the other way round. the key's
// argument loop is CompileFunction's own (src/d3d12/d3d12_pipeline_graphics.cpp), copied at build time by
// compile_key.py; the rest of the key (the cache identity, the device, the name, the shaders) is the same for both
// runs, so only the loop can tell them apart. held to: the same arguments give the same key, and two argument chains
// that differ in SM50_SHADER_FLAG_GUARD_STATIC_RANGES alone give two. prints both keys.
#include "../../src/airconv/airconv_public.h"
#include "sha1/sha1_util.hpp"
#include <cstdio>
#include <optional>

// what the copied loop calls besides its two lambdas: a root signature's part and a log line. neither runs here, the
// chains have no root signature and no unknown argument
using UINT = unsigned;
#define FAILED(hr) ((hr) < 0)
#define ERR(...) std::fprintf(stderr, "unknown compile argument\n")
namespace microsoft {
inline long
DXBCGetRootSignature(const void *, const void **, UINT *) {
  return -1;
}
} // namespace microsoft

using namespace mullion;

static std::optional<Sha1Digest>
argument_key(SM50_SHADER_COMPILATION_ARGUMENT_DATA *arguments) {
  Sha1HashState h;
  auto hash = [&](const auto &...values) { (h.update(values), ...); };
  auto bytes = [&](const void *data, size_t size) {
    h.update(uint64_t(size));
    if (size)
      h.update(data, size);
  };
#include "compile_key_arguments.inc"
  return h.final();
}

int
main() {
  // a pixel shader's chain as a pipeline makes it: its pipeline state, then the common data with the device's flags
  SM50_SHADER_PSO_PIXEL_SHADER_DATA pixel{};
  pixel.type = SM50_SHADER_PSO_PIXEL_SHADER;
  SM50_SHADER_COMMON_DATA off{&pixel, SM50_SHADER_COMMON, SM50_SHADER_METAL_320,
                              SM50_SHADER_FLAG(SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64), 32};
  SM50_SHADER_COMMON_DATA on = off;
  on.flags = SM50_SHADER_FLAG(off.flags | SM50_SHADER_FLAG_GUARD_STATIC_RANGES);
  auto key_off = argument_key((SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&off);
  auto key_on = argument_key((SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&on);
  auto again = argument_key((SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&off);
  if (!key_off || !key_on || !again) {
    std::printf("FAILED: a chain has no key\n");
    return 1;
  }
  std::printf("key without the guard: %s\nkey with the guard:    %s\n", key_off->string().c_str(),
              key_on->string().c_str());
  int failures = 0;
  auto check = [&](bool ok, const char *what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
    failures += !ok;
  };
  check(*key_off == *again, "the same arguments give the same key");
  check(*key_off != *key_on, "the guard's flag alone gives another key");
  return failures ? 1 : 0;
}
