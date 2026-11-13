// contract: a converted shader's cache key (CompileFunction, src/d3d12/d3d12_pipeline_graphics.cpp) and the cache's
// tag (its table, its archives' names, a pipeline library's version) follow the cache's identity
// (src/mullion/mullion_cache_identity.hpp): the same inputs give the same key, another codegen id or another macOS
// build gives another key and another tag
#include "../../src/mullion/mullion_cache_identity.hpp"
#include <cstdio>
#include <string>

using namespace mullion;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

// the key's head as CompileFunction hashes it, then the same rest
static Sha1Digest
Key(const cache::Identity &identity) {
  Sha1HashState h;
  cache::Hash(h, identity);
  h.update(uint64_t(0x1234)); // the device's registryID
  h.update(uint64_t(2));      // the shaders
  h.update("vs_main", 7);
  return h.final();
}

int
main() {
  const std::string a(64, 'a'), b = std::string(63, 'a') + "b";
  cache::Identity sample{a, "25B78"};
  check(Key(sample) == Key(cache::Identity{a, "25B78"}), "the same inputs, the same key");
  check(cache::Tag(sample) == cache::Tag(cache::Identity{a, "25B78"}), "the same inputs, the same tag");
  check(Key(sample) != Key(cache::Identity{b, "25B78"}), "another codegen id, another key");
  check(cache::Tag(sample) != cache::Tag(cache::Identity{b, "25B78"}), "another codegen id, another tag");
  check(Key(sample) != Key(cache::Identity{a, "25C56"}), "another macOS build, another key");
  check(cache::Tag(sample) != cache::Tag(cache::Identity{a, "25C56"}), "another macOS build, another tag");
  check(Key(sample) != Key(cache::Identity{a, ""}), "an unknown build isn't a known one");
  // each part keeps its length, so no two identities run together into one
  check(Key(cache::Identity{a + "2", "5B78"}) != Key(sample), "the parts don't run together");
  check(uint32_t(cache::Tag(sample)) != uint32_t(cache::Tag(cache::Identity{b, "25B78"})),
        "a pipeline library's version (the tag's low 32 bits) follows the codegen id");
  check(uint32_t(cache::Tag(sample)) != uint32_t(cache::Tag(cache::Identity{a, "25C56"})),
        "and the macOS build");
  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
