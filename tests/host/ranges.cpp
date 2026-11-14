// contract: MULLION_D3D12_CHECK_RANGES's arithmetic (src/mullion/mullion_ranges.hpp). A range of bytes fits its
// buffer only when offset and size both do, and no sum wraps to make a far range look near. A box fits a level only
// within its extent, which a block-compressed level counts in whole blocks. A placed footprint covers every row but the
// last one's padding. A reinterpret copy carries one source unit into one destination unit. A site writes its line once
// and counts every time; the RANGE section gives each window's counts
#include "../../src/mullion/mullion_ranges.hpp"
#include <cstdio>

using namespace mullion::ranges;

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

static std::string
said(const Overrun &overrun) {
  return overrun ? std::string(overrun.part) + " " + std::to_string(overrun.given) + " " +
                       std::to_string(overrun.limit)
                 : "fits";
}

int
main() {
  // bytes: up to the end fits, one past doesn't, and the part that runs past is named with what was left for it
  equal(said(Bytes(0, 256, 256)), "fits");
  equal(said(Bytes(256, 0, 256)), "fits");
  equal(said(Bytes(255, 1, 256)), "fits");
  equal(said(Bytes(0, 257, 256)), "Size 257 256");
  equal(said(Bytes(200, 57, 256)), "Size 57 56");
  equal(said(Bytes(257, 0, 256)), "Offset 257 256");
  // an offset and a size whose sum wraps past 2^64 to a small number: a check of offset + size <= limit passes it
  equal(said(Bytes(16, UINT64_MAX - 7, 256)), "Size 18446744073709551608 240");
  equal(said(Bytes(UINT64_MAX, 2, 256)), "Offset 18446744073709551615 256");

  // boxes: the far edge may be the extent's, not past it; x, then y, then z
  Box level{0, 0, 0, 64, 32, 1};
  equal(said(Within({0, 0, 0, 64, 32, 1}, level)), "fits");
  equal(said(Within({0, 0, 0, 65, 32, 1}, level)), "X 65 64");
  equal(said(Within({8, 0, 0, 16, 33, 1}, level)), "Y 33 32");
  equal(said(Within({0, 0, 0, 1, 1, 2}, level)), "Z 2 1");
  // a block-compressed 2x2 mip is a 4x4 block: a copy of the whole block fits, a texel more doesn't
  Box mip = InBlocks({0, 0, 0, 2, 2, 1}, 4);
  equal(said(Within({0, 0, 0, 4, 4, 1}, mip)), "fits");
  equal(said(Within({0, 0, 0, 5, 4, 1}, mip)), "X 5 4");
  equal(said(Within({0, 0, 0, 3, 3, 1}, InBlocks({0, 0, 0, 3, 3, 1}, 1))), "fits");

  // views: inside their buffer, or of no size anywhere, they fit; an address no live buffer holds, or a size past the
  // buffer's end from the address, doesn't
  equal(said(View(0x10100, 256, true, 0x10200)), "fits");
  equal(said(View(0x10100, 0, false, 0)), "fits");
  equal(said(View(0x10100, 257, true, 0x10200)), "Size 257 256");
  equal(said(View(0x20000, 4, false, 0)), "Address 131072 0");
  // a size whose end wraps past 2^64 from the address
  equal(said(View(0x10100, UINT64_MAX - 0xff, true, 0x10200)), "Size 18446744073709551360 256");

  // a reinterpret copy: 8 BC texels are two blocks, two texels of an uncompressed destination, and back
  check(InDestination(8, 4, 1) == 2, "8 BC texels are 2 destination texels");
  check(InDestination(2, 4, 1) == 1, "a partial block is one destination texel");
  check(InDestination(2, 1, 4) == 8, "2 texels are 2 BC blocks, 8 destination texels");
  check(InDestination(7, 1, 1) == 7, "same units: as given");

  // footprints: 4 rows of 16 RGBA8 texels at a pitch of 256 cover three pitches and the last row's 64 bytes
  check(FootprintBytes(16, 4, 1, 256, 1, 4) == 3 * 256 + 64, "a 2D footprint");
  // two slices of those rows: seven pitches and a row
  check(FootprintBytes(16, 4, 2, 256, 1, 4) == 7 * 256 + 64, "a 3D footprint");
  // BC1, 8 bytes a block: 16x8 texels are 4x2 blocks, one pitch and a row of 32 bytes
  check(FootprintBytes(16, 8, 1, 256, 4, 8) == 256 + 32, "a BC footprint");
  check(FootprintBytes(0, 4, 1, 256, 1, 4) == 0, "an empty footprint");
  // numbers no buffer holds don't wrap to a small size
  check(FootprintBytes(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1, 16) == UINT64_MAX, "a footprint too large");
  // nor does a width whose rounding up to whole blocks would pass 2^64
  check(FootprintBytes(UINT64_MAX, 1, 1, 256, 4, 8) == UINT64_MAX, "a footprint whose blocks pass 2^64");
  check(BlocksOf(UINT64_MAX, 4) == (UINT64_MAX >> 2) + 1, "whole blocks of the largest width");
  // the footprint check as CopyTextureRegion makes it: at offset 192 of a 1024-byte buffer it ends at the end, at 512 it
  // runs past
  equal(said(Bytes(192, FootprintBytes(16, 4, 1, 256, 1, 4), 1024)), "fits");
  equal(said(Bytes(512, FootprintBytes(16, 4, 1, 256, 1, 4), 1024)), "Size 832 512");

  // a site: its line the first time, nothing after, and a count each time
  Counts counts;
  Site src{Call::CopyBufferRegion, "src"}, dst{Call::CopyTextureRegion, "dst"};
  equal(src.fired(Bytes(0, 4096, 256), counts),
        "MULLION-RANGE call=CopyBufferRegion what=srcSize given=4096 limit=256 action=dropped");
  equal(src.fired(Bytes(512, 1, 256), counts), "");
  equal(dst.fired(Within({0, 0, 0, 65, 1, 1}, level), counts),
        "MULLION-RANGE call=CopyTextureRegion what=dstX given=65 limit=64 action=dropped");

  // the RANGE section: each window's counts, nothing for a window without any
  std::array<uint64_t, size_t(Call::Count)> last{};
  equal(counts.section(last), "CopyBufferRegion=2 CopyTextureRegion=1");
  equal(counts.section(last), "");
  src.fired(Bytes(0, 4096, 256), counts);
  equal(counts.section(last), "CopyBufferRegion=1");
  Site view{Call::IASetIndexBuffer, "view"};
  equal(view.fired(View(0x20000, 4, false, 0), counts),
        "MULLION-RANGE call=IASetIndexBuffer what=viewAddress given=131072 limit=0 action=dropped");
  equal(counts.section(last), "IASetIndexBuffer=1");

  if (failures)
    std::printf("%u failed\n", failures);
  return failures ? 1 : 0;
}
