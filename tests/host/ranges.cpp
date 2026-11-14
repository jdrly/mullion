// contract: MULLION_D3D12_CHECK_RANGES's arithmetic (src/mullion/mullion_ranges.hpp), and the length the bridge gives
// Metal 4 from an offset (src/winemetal/unix/buffer_range.h). A range of bytes fits its
// buffer only when offset and size both do, and no sum wraps to make a far range look near. A box fits a level only
// within its extent, which a block-compressed level counts in whole blocks. A placed footprint covers every row but the
// last one's padding. A reinterpret copy carries one source unit into one destination unit. A site writes its line once
// and counts every time; the RANGE section gives each window's counts
#include "../../src/mullion/mullion_ranges.hpp"
#include "../../src/winemetal/unix/buffer_range.h"
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

  // the ranges a check keeps: found once, held after, four at a time, the oldest going first; forgotten for another
  // device or generation
  {
    HeldRanges held;
    int device, other;
    check(held.end(&device, 1, 0x10100) == 0, "nothing is held at first");
    held.keep(0x10000, 0x10200);
    check(held.end(&device, 1, 0x10100) == 0x10200, "a kept range holds its addresses");
    check(held.end(&device, 1, 0x10200) == 0, "but not its end");
    for (uint64_t start : {0x20000, 0x30000, 0x40000})
      held.keep(start, start + 0x100);
    check(held.end(&device, 1, 0x10100) == 0x10200, "four ranges are held");
    held.keep(0x50000, 0x50100);
    check(held.end(&device, 1, 0x10100) == 0, "a fifth pushes the oldest out");
    check(held.end(&device, 1, 0x50080) == 0x50100, "and is held");
    check(held.end(&device, 2, 0x50080) == 0, "a buffer made or released forgets every range");
    held.keep(0x50000, 0x50100);
    check(held.end(&other, 2, 0x50080) == 0, "so does another device");
  }
  // a device made at the address of one destroyed, with as many buffers made and released: the process's counter has
  // moved past every generation the first one's ranges were kept at, so the second one's checks find none of them
  {
    HeldRanges held;
    int device;
    auto first = buffer_generation.load();
    check(held.end(&device, first, 0x10100) == 0, "the first device finds nothing kept");
    held.keep(0x10000, 0x10200);
    buffer_generation.fetch_add(1); // its buffer released, and the device with it
    buffer_generation.fetch_add(1); // the next device, at the same address, makes a buffer
    check(buffer_generation.load() != first, "the counter never comes back to a generation read before");
    check(held.end(&device, buffer_generation.load(), 0x10100) == 0,
          "a device at a destroyed one's address keeps none of its ranges");
  }
  // the buffer a command list last found an address in (private ticket 30): found for its addresses while no buffer is
  // made or released. a game releases buffer A while the list records, and its allocation ends; B is made at A's
  // address. the list has to look B up, not hand back A's ended allocation
  {
    struct Allocation {
      int id;
    } a{1}, b{2};
    FoundBuffer<Allocation> found;
    uint64_t offset = 0;
    auto generation = buffer_generation.load();
    check(!found.at(generation, 0x10100, offset), "no buffer is found at first");
    found.keep(&a, generation, 0x10000, 0x10200);
    check(found.at(generation, 0x10100, offset) == &a && offset == 0x100, "a found buffer holds its addresses");
    check(!found.at(generation, 0x10200, offset) && !found.at(generation, 0xff00, offset),
          "but not its end, nor an address before it");
    buffer_generation.fetch_add(1); // A released
    buffer_generation.fetch_add(1); // B made where A was
    generation = buffer_generation.load();
    check(!found.at(generation, 0x10100, offset), "a buffer released or made forgets the buffer found");
    found.keep(&b, generation, 0x10000, 0x10200);
    check(found.at(generation, 0x10100, offset) == &b, "the buffer found after it is held");
    found = nullptr;
    check(!found.at(generation, 0x10100, offset), "Reset and an aliasing barrier forget it");
  }

  // a root view's binding: its address and what its buffer has from there, at most 32 bits of it, however little that
  // is; the null buffer of length 0 in no buffer
  constexpr uint64_t null_buffer = 0x900000;
  auto bound = [&](uint64_t address, bool held, uint64_t end) {
    auto view = RootViewBinding(address, held, end, null_buffer);
    return std::to_string(view.address) + " " + std::to_string(view.length);
  };
  equal(bound(0x10100, true, 0x10200), "65792 256");
  check(RootViewBinding(0x10000, true, 0x10000 + (5ull << 32), null_buffer).length == UINT32_MAX,
        "a root view in a buffer of 5 GiB is bound at 4 GiB, not at what the low 32 bits of its length say");
  equal(bound(0x101e0, true, 0x10200), "66016 32");
  check(bound(0x101fc, true, 0x10200) == "66044 4", "a root view with 4 bytes left keeps its address and its 4 bytes");
  check(bound(0x20000, false, 0) == "9437184 0", "a root view in no buffer is the null buffer");
  check(bound(0, false, 0) == "9437184 0", "a root view at address 0 is the null buffer");
  Site root{Call::SetRootView, "view", "clamped"};
  {
    Counts root_counts;
    std::array<uint64_t, size_t(Call::Count)> root_last{};
    equal(root.fired(View(0x20000, 4, false, 0), root_counts),
          "MULLION-RANGE call=SetRootView what=viewAddress given=131072 limit=0 action=clamped");
    equal(root_counts.section(root_last), "SetRootView=1");
  }

  // the descriptors a heap has from a table's base: 4 of 32 bytes from the start, 1 from the last, none known outside it
  check(DescriptorsLeft(0x1000, 0x1000, 0x1080, 32) == 4, "a table at a heap's start has its 4 descriptors");
  check(DescriptorsLeft(0x1060, 0x1000, 0x1080, 32) == 1, "a table at a heap's last descriptor has 1");
  check(DescriptorsLeft(0x1080, 0x1000, 0x1080, 32) == UINT32_MAX, "a table at a heap's end is in no heap");
  check(DescriptorsLeft(0x0fe0, 0x1000, 0x1080, 32) == UINT32_MAX, "a table before a heap is in no heap");
  check(DescriptorsLeft(0x1000, 0, 0, 0) == UINT32_MAX, "with no heap bound, no count");

  // the bridge's Metal 4 lengths: what a buffer has from an offset, before, at and past its end
  check(buffer_remaining(256, 0) == 256, "a buffer's length from its start");
  check(buffer_remaining(256, 200) == 56, "what is left from an offset inside it");
  check(buffer_remaining(256, 256) == 0, "nothing from its end");
  check(buffer_remaining(256, 260) == 0, "nothing past its end, not a length that wraps");

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
  Site dest{Call::WriteBufferImmediate, "dest"}, cbv{Call::CreateConstantBufferView, "view"};
  equal(dest.fired(View(0x20000, 4, false, 0), counts),
        "MULLION-RANGE call=WriteBufferImmediate what=destAddress given=131072 limit=0 action=dropped");
  equal(cbv.fired(View(0x10000, 512, true, 0x10100), counts),
        "MULLION-RANGE call=CreateConstantBufferView what=viewSize given=512 limit=256 action=dropped");
  equal(counts.section(last), "CreateConstantBufferView=1 WriteBufferImmediate=1");

  if (failures)
    std::printf("%u failed\n", failures);
  return failures ? 1 : 0;
}
