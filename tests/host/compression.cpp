// contract: which Direct3D 12 textures lose Metal's lossless compression (src/mullion/mullion_compression.hpp).
// MULLION_TEX_MSAA_COMPRESSION unset is Metal's default on every GPU; `auto` turns compression off for multisampled
// textures and resolve targets on a GPU before Apple10, which is what MULLION_GPU_FAMILY=9 makes the bridge answer,
// and leaves it on Apple10; `0` turns it off everywhere. a resolve target is a single-sampled 2D texture of the type
// group and size of a multisampled texture made before it. a rule's first texture is logged once, and the lines
// have the fields docs/swtor/README.md gives
#include "../../src/mullion/mullion_compression.hpp"
#include <cstdio>

using namespace mullion::compression;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

static void
equal(const std::string &got, const std::string &want) {
  check(got == want, "got\n  " + got + "\nwant\n  " + want);
}

// DXGI_FORMAT_R8G8B8A8_TYPELESS, the group of R8G8B8A8_UNORM (28) and its sRGB form; R16G16B16A16_TYPELESS; and
// R24G8_TYPELESS, the group of D24_UNORM_S8_UINT (45)
constexpr uint32_t kRGBA8 = 27, kRGBA16 = 9, kD24S8 = 44;

int
main() {
  // the grammar: 0, 1 and auto; anything else, and unset, is the default, Metal's on every GPU
  for (auto value : {"0", "1", "auto"})
    equal(MsaaSwitch(value, kMsaaDefault), value);
  for (auto value : {"", "2", "AUTO", "off", " 0"})
    equal(MsaaSwitch(value, kMsaaDefault), kMsaaDefault);
  for (bool apple10 : {false, true}) {
    check(!MsaaUncompressed(MsaaSwitch("", kMsaaDefault), apple10), "unset keeps Metal's default");
    check(!MsaaUncompressed("1", apple10), "1 keeps Metal's default");
    check(MsaaUncompressed("0", apple10), "0 turns it off on every GPU");
  }
  // auto follows the family the bridge answers as: Apple9 (an M4, or MULLION_GPU_FAMILY=9) off, Apple10 on
  check(MsaaUncompressed("auto", false), "auto turns it off before Apple10");
  check(!MsaaUncompressed("auto", true), "auto keeps it on Apple10");

  Shape msaa{kRGBA8, 1920, 1080, 4, true}, target{kRGBA8, 1920, 1080, 1, true};

  // off: nothing is uncompressed, nothing is remembered and nothing logged, so every texture's field stays 0
  {
    Textures textures(false);
    check(textures.rule(msaa) == Rule::None, "off: a multisampled texture keeps compression");
    check(textures.made(msaa, false) == Rule::None, "off: nothing to log");
    check(textures.rule(target) == Rule::None, "off: a texture of its size keeps compression");
  }

  // on
  Textures textures(true);
  check(textures.rule(msaa) == Rule::Msaa, "a multisampled texture is uncompressed");
  check(textures.rule(Shape{kD24S8, 1920, 1080, 2, true}) == Rule::Msaa, "so is a multisampled depth texture");
  check(textures.rule(target) == Rule::None, "a texture made before any multisampled one keeps compression");
  check(textures.made(msaa, true) == Rule::Msaa, "the first multisampled texture is logged");
  check(textures.made(Shape{kRGBA8, 1280, 720, 4, true}, true) == Rule::None, "the second isn't");
  check(textures.rule(target) == Rule::Resolve, "a single-sampled 2D texture of its group and size is a resolve target");
  check(textures.rule(Shape{kRGBA8, 1280, 720, 1, true}) == Rule::Resolve, "so is one of the second's size");
  check(textures.rule(Shape{kRGBA8, 1920, 1088, 1, true}) == Rule::None, "another height keeps compression");
  check(textures.rule(Shape{kRGBA8, 1921, 1080, 1, true}) == Rule::None, "another width keeps compression");
  check(textures.rule(Shape{kRGBA16, 1920, 1080, 1, true}) == Rule::None, "another group keeps compression");
  check(textures.rule(Shape{kRGBA8, 1920, 1080, 1, false}) == Rule::None, "a 3D texture keeps compression");
  check(textures.made(target, true) == Rule::Resolve, "the first resolve target is logged");
  check(textures.made(target, true) == Rule::None, "the second isn't");
  check(textures.made(Shape{kRGBA16, 64, 64, 1, true}, false) == Rule::None, "a compressed texture logs nothing");
  check(textures.rule(Shape{kD24S8, 1920, 1080, 1, true}) == Rule::None,
        "a depth texture of a size only a color multisampled texture has keeps compression");
  textures.made(Shape{kD24S8, 1920, 1080, 4, true}, true);
  check(textures.rule(Shape{kD24S8, 1920, 1080, 1, true}) == Rule::Resolve, "and is a resolve target after a depth one");

  // the lines
  equal(SettingsLine(false, false, "1", true), "Mullion: textures compression=metal msaa=metal msaaSwitch=1 apple10=yes");
  equal(SettingsLine(false, true, "auto", false), "Mullion: textures compression=metal msaa=off msaaSwitch=auto apple10=no");
  equal(SettingsLine(true, false, "1", true), "Mullion: textures compression=off msaa=off msaaSwitch=1 apple10=yes");
  equal(UncompressedLine(Rule::Msaa, msaa, 28), "Mullion: texture uncompressed rule=msaa samples=4 size=1920x1080 format=28");
  equal(UncompressedLine(Rule::Resolve, target, 29),
        "Mullion: texture uncompressed rule=resolve samples=1 size=1920x1080 format=29");
  // the longest either can be stays under 900 bytes, so Wine never splits it
  Shape largest{~0u, ~0ull, ~0u, ~0u, true};
  check(UncompressedLine(Rule::Resolve, largest, ~0u).size() < 900, "the uncompressed line under 900 bytes");
  check(SettingsLine(true, true, "auto", false).size() < 900, "the settings line under 900 bytes");

  if (failures)
    std::printf("%u failed\n", failures);
  return failures ? 1 : 0;
}
