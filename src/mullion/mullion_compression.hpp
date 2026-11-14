#pragma once

// jdrly/mullion: which Direct3D 12 textures are made without Metal's lossless compression (docs/swtor/README.md
// § Texture compression). On an M4 (Apple9) d9mt saw 4x MSAA with compression draw flickering lines, and its fix,
// which a player confirmed, turns compression off for multisampled textures and the textures their resolves write
// on GPUs before Apple10 (d9mt 3145b30, driver/mtl/mtl_pfv.h). MULLION_TEX_MSAA_COMPRESSION is that rule here:
//
//   0      multisampled textures and resolve targets are made uncompressed, on every GPU
//   1      Metal's default for them (compressed); the default until the GPU gate turns `auto` on
//   auto   0 on a GPU before Apple10 (the family the bridge answers as, under MULLION_GPU_FAMILY too), else 1
//
// Direct3D 12 doesn't say at creation which texture a resolve will write. A resolve target here is a single-sampled
// 2D texture of the type group (D3D11.3 19.1.4), width and height of a multisampled texture the device has made
// before it. MULLION_TEX_NO_COMPRESSION=1, every texture uncompressed, is the bridge's (texture_compression.h).
//
// The lines, at info, from d3d12's device: one as each device is made, with the switch read then,
//
//   Mullion: textures compression=metal msaa=off msaaSwitch=auto apple10=no
//
// and one for each rule, at the first texture it makes uncompressed on the device,
//
//   Mullion: texture uncompressed rule=msaa samples=4 size=1920x1080 format=27
//
// `format` is the texture's DXGI_FORMAT, `samples` the count Metal makes it with (4 for the paired 8x of an Apple9).
//
// Pure C++ (no Windows, no Metal): tests/host/compression.cpp builds it natively.

#include <array>
#include <cstdint>
#include <set>
#include <string>

namespace mullion::compression {

// MULLION_TEX_MSAA_COMPRESSION unset (d3d12_device.cpp declares it with this): Metal's default on every GPU
constexpr const char *kMsaaDefault = "1";

// MULLION_TEX_MSAA_COMPRESSION's grammar: "0", "1" or "auto"; anything else is the fallback
inline std::string
MsaaSwitch(const std::string &raw, const std::string &fallback) {
  return raw == "0" || raw == "1" || raw == "auto" ? raw : fallback;
}

// whether multisampled textures and resolve targets are made uncompressed, by the switch's value and whether the GPU
// is Apple10 or newer
inline bool
MsaaUncompressed(const std::string &value, bool apple10) {
  return value == "0" || (value == "auto" && !apple10);
}

enum class Rule { None, Msaa, Resolve };

inline const char *
RuleName(Rule rule) {
  return rule == Rule::Msaa ? "msaa" : rule == Rule::Resolve ? "resolve" : "none";
}

// a texture as the rule sees it: its DXGI type group, size and the sample count Metal makes it with
struct Shape {
  uint32_t group;
  uint64_t width;
  uint32_t height;
  uint32_t samples;
  bool texture2D; // D3D12_RESOURCE_DIMENSION_TEXTURE2D
};

// the multisampled textures' shapes a device has made, and the rule's answer for a texture. not thread-safe: the
// device holds a lock around it
class Textures {
public:
  explicit Textures(bool msaa_uncompressed) : msaa_uncompressed_(msaa_uncompressed) {}

  bool
  msaaUncompressed() const {
    return msaa_uncompressed_;
  }

  // which rule makes a texture of `shape` uncompressed, None when it keeps Metal's default
  Rule
  rule(const Shape &shape) const {
    if (!msaa_uncompressed_)
      return Rule::None;
    if (shape.samples > 1)
      return Rule::Msaa;
    if (shape.texture2D && multisampled_.count(key(shape)))
      return Rule::Resolve;
    return Rule::None;
  }

  // a texture of `shape` was made, `uncompressed` or not: a multisampled one's size and group make later textures
  // resolve targets. the rule whose first texture it is, which the device logs; None after the first
  Rule
  made(const Shape &shape, bool uncompressed) {
    if (msaa_uncompressed_ && shape.samples > 1)
      multisampled_.insert(key(shape));
    if (!uncompressed)
      return Rule::None;
    Rule rule = shape.samples > 1 ? Rule::Msaa : Rule::Resolve;
    bool &logged = rule == Rule::Msaa ? msaa_logged_ : resolve_logged_;
    if (logged)
      return Rule::None;
    logged = true;
    return rule;
  }

private:
  static std::array<uint64_t, 3>
  key(const Shape &shape) {
    return {shape.group, shape.width, shape.height};
  }

  bool msaa_uncompressed_;
  bool msaa_logged_ = false, resolve_logged_ = false;
  std::set<std::array<uint64_t, 3>> multisampled_;
};

// "Mullion: textures …", as a device is made. `all`: MULLION_TEX_NO_COMPRESSION=1
inline std::string
SettingsLine(bool all, bool msaa_uncompressed, const std::string &msaa_switch, bool apple10) {
  return std::string("Mullion: textures compression=") + (all ? "off" : "metal") +
         " msaa=" + (all || msaa_uncompressed ? "off" : "metal") + " msaaSwitch=" + msaa_switch +
         " apple10=" + (apple10 ? "yes" : "no");
}

// "Mullion: texture uncompressed …", once per device for each rule (Textures::made). `format`: the DXGI_FORMAT
inline std::string
UncompressedLine(Rule rule, const Shape &shape, uint32_t format) {
  return std::string("Mullion: texture uncompressed rule=") + RuleName(rule) +
         " samples=" + std::to_string(shape.samples) + " size=" + std::to_string(shape.width) + "x" +
         std::to_string(shape.height) + " format=" + std::to_string(format);
}

} // namespace mullion::compression
