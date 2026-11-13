#include "mullion_session.hpp"
#include "Metal.hpp"
#include "airconv_public.h"
#include "log/log.hpp"
#include "mullion_shader_cache.hpp"
#include "util_string.hpp"
#include "util_switch.hpp"
#include <atomic>
#include <version.h>

namespace mullion {

// MULLION_SESSION_HEADER=1 writes the header; off by default until the Mac run (docs/swtor/README.md), and SWTOR on
// Mac's DX12 dev build turns it on. Off: no header, and no bridge call for it
static sw::Switch<bool> kSessionHeader{"MULLION_SESSION_HEADER", false, sw::exactlyOne};

void
WriteSessionHeader(uint64_t handle, const char *api, uint32_t feature_level, bool metal4, uint32_t shader_metal) {
  static std::atomic_flag written;
  // MULLION_SESSION_HEADER off or MULLION_LOG_LEVEL above info: nothing written, nothing read
  if (Logger::logLevel() > LogLevel::Info || !kSessionHeader() || written.test_and_set())
    return;
  WMT::Device device;
  device.handle = handle;
  session::Header header;
  header.version = MULLION_VERSION;
  header.api = api;
  header.feature_level = feature_level;
  header.metal4 = !device.supportsFamily(WMTGPUFamilyMetal4) ? "unsupported" : metal4 ? "on" : "off";
  header.shader_metal = shader_metal;
  header.airconv = AIRCONV_VERSION;

  WMTGetOSVersion(&header.macos[0], &header.macos[1], &header.macos[2]);
  char name[256] = {};
  device.name().getCString(name, sizeof(name), WMTUTF8StringEncoding);
  header.gpu = name;
  header.working_set = device.recommendedMaxWorkingSetSize();
  header.placement_sparse = device.supportsPlacementSparse();

  // the file ShaderCache opens for this Metal version
  header.cache_file = str::format("shaders_", shader_metal, ".db");
  if (auto directory = ShaderCacheDirectory(); !directory.empty())
    header.cache_directory = directory;
  auto cache_path = header.cache_directory ? *header.cache_directory + header.cache_file : std::string();

  WMTSessionFacts facts;
  WMTGetSessionFacts(handle, header.cache_directory ? cache_path.c_str() : nullptr, kMullionShaderCacheVersion, &facts);
  if (facts.macos_build[0])
    header.build = std::string(facts.macos_build);
  if (facts.gpu_family)
    header.family = facts.gpu_family;
  header.masked = facts.masked_family;
  if (facts.physical_memory)
    header.ram = facts.physical_memory;
#if defined(__arm64ec__)
  header.arch = "arm64ec";
#elif defined(__x86_64__)
  header.arch = "x86_64";
#else
  header.arch = "i386";
#endif
  if (facts.translated)
    header.arch += "-rosetta";
  if (facts.cache_entries >= 0)
    header.cache_entries = facts.cache_entries;
  if (facts.cache_bytes >= 0)
    header.cache_bytes = facts.cache_bytes;

  header.switches = sw::processRegistry().list();
  if (auto block = GetEnvironmentStringsW()) {
    for (auto entry = block; *entry; entry += wcslen(entry) + 1) {
      std::string text = str::fromws(entry);
      // "=C:=C:\..." and the like: a drive's directory, no variable
      if (auto equals = text.find('=', 1); equals != std::string::npos)
        header.env.emplace_back(text.substr(0, equals), text.substr(equals + 1));
    }
    FreeEnvironmentStringsW(block);
  }

  for (auto &line : session::Format(header))
    Logger::info(line);
}

} // namespace mullion
