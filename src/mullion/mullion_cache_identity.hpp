#pragma once

// The shader cache's identity (docs/swtor/caches.md): the converter's codegen id (MULLION_CODEGEN_ID, which
// tools/codegen_id.py computes from its sources and toolchain) and the macOS build (kern.osversion). A converted
// shader's key starts with both; the cache's table, its pipeline archives and the pipeline libraries it writes are
// named by its tag, so another id's are dropped, and refused.
//
// Pure C++ (no Windows, no Metal): tests/host/cache_identity.cpp builds it natively.

#include "sha1/sha1_util.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace mullion::cache {

struct Identity {
  std::string codegen; // MULLION_CODEGEN_ID, 64 hex digits
  std::string build;   // the macOS build; "" when it couldn't be read
};

// a key's first fields
inline void
Hash(Sha1HashState &h, const Identity &identity) {
  for (std::string_view part : {std::string_view(identity.codegen), std::string_view(identity.build)}) {
    h.update(uint64_t(part.size()));
    h.update(part.data(), part.size());
  }
}

// the table cache_<tag as 16 hex digits>, the archive shards pipelines_<metal>_<tag>_<n>.metallib, and a pipeline
// library's version (its low 32 bits)
inline uint64_t
Tag(const Identity &identity) {
  Sha1HashState h;
  Hash(h, identity);
  auto digest = h.final();
  uint64_t tag;
  std::memcpy(&tag, digest.data, sizeof(tag));
  return tag;
}

} // namespace mullion::cache
