#include "mullion_shader_cache.hpp"
#include "log/log.hpp"
#include "mullion_session.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <cstdio>
#include <mullion_codegen.h>

static_assert(sizeof(MULLION_CODEGEN_ID) == 65, "tools/codegen_id.py writes 64 hex digits");

namespace mullion {

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static mullion::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<mullion::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

std::string
ShaderCacheDirectory() {
  if (env::getEnvVar("MULLION_SHADER_CACHE") == "0")
    return {};
  if (auto path = env::getEnvVar("MULLION_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/"))
    return path.ends_with('/') ? path : path + "/";
  // Mullion's shaders differ from stock DXMT's at the same AIRCONV_VERSION, so the caches never share a directory
  return str::format("mullion/", env::getExeName(), "/");
}

const cache::Identity &
ShaderCacheIdentity() {
  static const cache::Identity identity = [] {
    WMTSessionFacts facts;
    WMTGetSessionFacts(0, nullptr, &facts);
    return cache::Identity{MULLION_CODEGEN_ID, facts.macos_build};
  }();
  return identity;
}

uint64_t
ShaderCacheTag() {
  static const uint64_t tag = cache::Tag(ShaderCacheIdentity());
  return tag;
}

std::string
ShaderCacheArchivePrefix(uint32_t metal_version) {
  char tag[17];
  snprintf(tag, sizeof(tag), "%016llx", (unsigned long long)ShaderCacheTag());
  return str::format("pipelines_", metal_version, "_", tag, "_");
}

// opens the table of this identity, after the bridge has dropped other identities' tables and archives
// (WMTCachePrepare), and says what it found in a cacheOpen line (docs/swtor/README.md)
ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  session::CacheOpen open;
  open.directory = ShaderCacheDirectory();
  if (open.directory.empty())
    return;
  open.file = str::format("shaders_", (unsigned int)metal_version, ".db");
  auto path = open.directory + open.file;
  auto family = str::format("pipelines_", (unsigned int)metal_version, "_");
  WMTCachePrepared prepared;
  WMTCachePrepare(path.c_str(), ShaderCacheTag(), family.c_str(), ShaderCacheArchivePrefix(metal_version).c_str(),
                  &prepared);
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), ShaderCacheTag());
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), ShaderCacheTag());

  if (prepared.entries >= 0)
    entries_ = open.entries = prepared.entries;
  if (prepared.bytes >= 0)
    open.bytes = prepared.bytes;
  open.codegen = ShaderCacheIdentity().codegen;
  open.build = ShaderCacheIdentity().build;
  open.dropped_tables = prepared.dropped_tables;
  open.dropped_shards = prepared.removed_shards;
  open.dropped_bytes = prepared.dropped_bytes + prepared.removed_bytes;
  open.env = ProcessEnvironment();
  for (auto &line : session::CacheOpenLines(open))
    Logger::info(line);
}

} // namespace mullion