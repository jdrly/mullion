#pragma once

// jdrly/mullion: MULLION_D3D12_CHECK_RANGES=1 checks the offsets, sizes and regions a game hands Direct3D 12 against
// the resource they point into, and drops a call whose numbers run past it (private ticket 20, docs/swtor/README.md
// § Ranges). Direct3D calls such a call undefined; Metal may then read or write past a buffer or texture, which on a
// Mac can restart WindowServer. The checks are arithmetic on numbers the call already has, and this file is that
// arithmetic, the line a check writes and its counters.
//
// The first time a check fires at its site, at warn:
//
//   MULLION-RANGE call=CopyBufferRegion what=srcSize given=4096 limit=256 action=dropped
//
// `what` is the side, src or dst, and the number: Offset or Size (a range of bytes, `limit` the buffer's width or
// what it has past the offset), X, Y or Z (a box's or region's far edge, `limit` the level's), Footprint (a placed
// footprint's offset or bytes, as Offset and Size), Resource (`given` its D3D12_RESOURCE_DIMENSION where a buffer,
// 1, was asked for). `given` is the value the game gave. Every time it fires the call's counter goes up; with
// MULLION_STATS=1 the RANGE section after STATS says how often each call was dropped in that second:
//
//   RANGE CopyBufferRegion=3 CopyTextureRegion=1
//
// Pure C++ (no Windows, no Metal): tests/host/ranges.cpp builds it natively.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace mullion::ranges {

// a number that runs past its resource: which part of it, the value given and the most the resource takes there.
// false when it fits
struct Overrun {
  const char *part = nullptr;
  uint64_t given = 0, limit = 0;

  explicit operator bool() const {
    return part;
  }
};

// whether `size` bytes from `offset` lie within `limit` bytes; the sum never wraps
inline Overrun
Bytes(uint64_t offset, uint64_t size, uint64_t limit) {
  if (offset > limit)
    return {"Offset", offset, limit};
  if (size > limit - offset)
    return {"Size", size, limit - offset};
  return {};
}

// a box from (left, top, front) to (right, bottom, back), right, bottom and back excluded
struct Box {
  uint64_t left = 0, top = 0, front = 0, right = 0, bottom = 0, back = 0;
};

// whether `box` lies within an extent that starts at 0: the edge past it, x, y or z first
inline Overrun
Within(const Box &box, const Box &extent) {
  if (box.right > extent.right)
    return {"X", box.right, extent.right};
  if (box.bottom > extent.bottom)
    return {"Y", box.bottom, extent.bottom};
  if (box.back > extent.back)
    return {"Z", box.back, extent.back};
  return {};
}

// whole blocks: a partial one counts. never wraps
inline uint64_t
BlocksOf(uint64_t texels, uint64_t block) {
  return texels / block + (texels % block != 0);
}

// a level's extent in whole blocks: a block-compressed region counts full 4-texel blocks, including in mips smaller
// than a block (D3D11.3 19.5.1, as d3d12_texture.cpp's WriteToSubresource reads it)
inline Box
InBlocks(Box extent, uint64_t block) {
  extent.right = BlocksOf(extent.right, block) * block;
  extent.bottom = BlocksOf(extent.bottom, block) * block;
  return extent;
}

// the texels a copy of `texels` source texels covers in the destination: a copy between a block-compressed format and
// another carries one unit, a block or a texel, into one unit (CopyResource, Remarks)
inline uint64_t
InDestination(uint64_t texels, uint64_t src_block, uint64_t dst_block) {
  return BlocksOf(texels, src_block) * dst_block;
}

// the bytes a placed footprint covers from its offset: every row of every slice but the last row, whose padding to
// RowPitch it doesn't need. a number too large for 64 bits is the largest there is, which no buffer holds
inline uint64_t
FootprintBytes(uint64_t width, uint64_t height, uint64_t depth, uint64_t row_pitch, uint64_t block,
               uint64_t bytes_per_block) {
  if (!width || !height || !depth)
    return 0;
  uint64_t rows = BlocksOf(height, block), row = 0, slices = 0, total = 0;
  if (__builtin_mul_overflow(BlocksOf(width, block), bytes_per_block, &row) ||
      __builtin_mul_overflow(rows, depth, &slices) || __builtin_mul_overflow(slices - 1, row_pitch, &total) ||
      __builtin_add_overflow(total, row, &total))
    return UINT64_MAX;
  return total;
}

// the calls whose numbers are checked
enum class Call : unsigned { CopyBufferRegion, CopyTextureRegion, Count };

inline const char *
CallName(Call call) {
  switch (call) {
  case Call::CopyBufferRegion:
    return "CopyBufferRegion";
  case Call::CopyTextureRegion:
    return "CopyTextureRegion";
  default:
    return "?";
  }
}

// "MULLION-RANGE …", the first time a site's check fires. `what` is the side ("src", "dst") and the overrun's part
inline std::string
Line(Call call, const char *side, const Overrun &overrun, const char *action = "dropped") {
  return std::string("MULLION-RANGE call=") + CallName(call) + " what=" + side + overrun.part +
         " given=" + std::to_string(overrun.given) + " limit=" + std::to_string(overrun.limit) + " action=" + action;
}

// how often each call was dropped, since the process started; any thread adds
struct Counts {
  std::array<std::atomic<uint64_t>, size_t(Call::Count)> calls{};

  void
  add(Call call) {
    calls[size_t(call)].fetch_add(1, std::memory_order_relaxed);
  }

  // the RANGE section's text for the window that ends: the calls dropped since `last`, the counts at the window
  // before; "" when there were none
  std::string
  section(std::array<uint64_t, size_t(Call::Count)> &last) const {
    std::string text;
    for (size_t i = 0; i < calls.size(); i++) {
      uint64_t now = calls[i].load(std::memory_order_relaxed), window = now - last[i];
      last[i] = now;
      if (window)
        text += (text.empty() ? "" : " ") + std::string(CallName(Call(i))) + "=" + std::to_string(window);
    }
    return text;
  }
};

// this DLL's
inline Counts counts;

// one place a check can fire: it writes its line the first time only
struct Site {
  Call call;
  const char *side;
  std::atomic<bool> said{false};

  // a check here fired: counts it in `to`, and gives the line to write the first time, else ""
  std::string
  fired(const Overrun &overrun, Counts &to = counts) {
    to.add(call);
    return said.exchange(true, std::memory_order_relaxed) ? std::string() : Line(call, side, overrun);
  }
};

} // namespace mullion::ranges
