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
// 1, was asked for). A view's side is `view`: viewAddress (an address no live buffer holds, `limit` 0) or viewSize
// (`limit` what the buffer has from the address); WriteBufferImmediate's is `dest`, the same. `given` is the value the game gave. Every time it fires the call's counter goes up; with
// MULLION_STATS=1 the RANGE section after STATS says how often each call was dropped in that second:
//
//   RANGE CopyBufferRegion=3 CopyTextureRegion=1
//
// A view that runs past its buffer is bound as no view, which Direct3D reads as zeros; its line says dropped too. A
// root CBV, SRV or UAV is bound as given, and its shaders check it against what its buffer has from its address
// (RootViewBinding): one at an address no live buffer holds is bound as the null buffer of length 0, and its line says
// clamped.
//
// Pure C++ (no Windows, no Metal): tests/host/ranges.cpp builds it natively.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mullion::ranges {

// MULLION_D3D12_CHECK_RANGES, read once as d3d12core.dll loads (d3d12_command_list.cpp declares it). Every check
// outside that file tests this
extern const bool on;

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

// whether a view of `size` bytes at GPU address `address` lies in the live buffer that holds the address, which ends
// at `end`; `held` false when no live buffer holds it. a view of no size reads nothing, wherever it is
inline Overrun
View(uint64_t address, uint64_t size, bool held, uint64_t end) {
  if (!size)
    return {};
  if (!held)
    return {"Address", address, 0};
  if (size > end - address)
    return {"Size", size, end - address};
  return {};
}

// raised each time any buffer's addresses are registered or unregistered, by any device of the process, after the
// change. One counter for the process, never restarted: a device made where another was, at the same address, never
// reads a generation the other's ranges were kept at. This DLL's
inline std::atomic<uint64_t> buffer_generation{0};

// how a root CBV, SRV or UAV is bound under SM50_SHADER_FLAG_CHECK_ROOT_RANGES: the address its shaders read through,
// and the length they check every access against, what the live buffer holding `address` has from it up to the 32
// bits a buffer descriptor's length keeps. A view in no live buffer is bound as `null_buffer`, the device's zero-filled
// one, of length 0: every access reads 0 and writes nothing. (A load whose check fails reads its dropped value from
// the lock table the root arguments carry, not from the view: the converter's BufferDescriptor::Fallback)
struct RootView {
  uint64_t address, length;
};

inline RootView
RootViewBinding(uint64_t address, bool held, uint64_t end, uint64_t null_buffer) {
  if (!held)
    return {null_buffer, 0};
  return {address, std::min<uint64_t>(end - address, UINT32_MAX)};
}

// how many descriptors a shader-visible heap from `start` to `end`, `increment` bytes each, has from `base` on: the
// count a dynamic descriptor index through a table at `base` is clamped below (SM50_SHADER_FLAG_CHECK_DESCRIPTOR_INDICES).
// UINT32_MAX, no clamp, for a base in no heap the list has bound: what it holds is not Mullion's to know
inline uint64_t
DescriptorsLeft(uint64_t base, uint64_t start, uint64_t end, uint64_t increment) {
  if (!increment || base < start || base >= end)
    return UINT32_MAX;
  return std::min<uint64_t>((end - base) / increment, UINT32_MAX);
}

// the address ranges of live buffers a check found, newest first, kept while buffer_generation is the one they were
// found at, for the device they were found in. Numbers only, never a
// buffer: a range outlives nothing. Not thread-safe: each command list has its own, each thread making views its own
class HeldRanges {
public:
  // the end of a kept range that holds `address`, or 0 when none does. Another device or generation than the ranges
  // were kept at forgets them all first; read the generation before looking an address up, so a range found after
  // it may change is kept at the old one, and forgotten at the next call
  uint64_t
  end(const void *owner, uint64_t generation, uint64_t address) {
    if (owner != owner_ || generation != generation_) {
      ranges_ = {};
      owner_ = owner;
      generation_ = generation;
    }
    for (auto &range : ranges_)
      if (address >= range.start && address < range.end)
        return range.end;
    return 0;
  }

  // a live buffer's range, found since `end` was last asked; the oldest of four goes
  void
  keep(uint64_t start, uint64_t end) {
    std::move_backward(ranges_.begin(), ranges_.end() - 1, ranges_.end());
    ranges_[0] = {start, end};
  }

private:
  struct Range {
    uint64_t start = 0, end = 0;
  };
  std::array<Range, 4> ranges_{};
  const void *owner_ = nullptr;
  uint64_t generation_ = 0;
};

// the buffer a command list last found an address in, with the addresses that find it too, kept while
// buffer_generation is the one it was found at (private ticket 30; not behind MULLION_D3D12_CHECK_RANGES). A buffer
// released or made forgets it: the one it names may be gone, and another may have its addresses. Its addresses are
// numbers, so a lookup reads nothing of the buffer before the generation says it is still there. Not thread-safe:
// each command list has its own
template <typename Buffer> class FoundBuffer {
public:
  FoundBuffer(std::nullptr_t = nullptr) {}

  // the buffer kept for `address`, with the address's offset in it, or nullptr when none is. Read the generation
  // before looking an address up, as for HeldRanges
  Buffer *
  at(uint64_t generation, uint64_t address, uint64_t &offset) const {
    if (!buffer_ || generation != generation_ || address < start_ || address >= end_)
      return nullptr;
    offset = address - start_;
    return buffer_;
  }

  // `buffer` holds [start, end), found at `generation`
  void
  keep(Buffer *buffer, uint64_t generation, uint64_t start, uint64_t end) {
    buffer_ = buffer;
    generation_ = generation;
    start_ = start;
    end_ = end;
  }

private:
  Buffer *buffer_ = nullptr;
  uint64_t generation_ = 0, start_ = 0, end_ = 0;
};

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
enum class Call : unsigned {
  CopyBufferRegion,
  CopyTextureRegion,
  IASetIndexBuffer,
  IASetVertexBuffers,
  CreateConstantBufferView,
  WriteBufferImmediate,
  SetRootView,
  Count
};

inline const char *
CallName(Call call) {
  switch (call) {
  case Call::CopyBufferRegion:
    return "CopyBufferRegion";
  case Call::CopyTextureRegion:
    return "CopyTextureRegion";
  case Call::IASetIndexBuffer:
    return "IASetIndexBuffer";
  case Call::IASetVertexBuffers:
    return "IASetVertexBuffers";
  case Call::CreateConstantBufferView:
    return "CreateConstantBufferView";
  case Call::WriteBufferImmediate:
    return "WriteBufferImmediate";
  case Call::SetRootView:
    return "SetRootView";
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

// one place a check can fire: it writes its line the first time only. `action` is what it does: drops the call or the
// view, or clamps the bound a shader checks it against
struct Site {
  Call call;
  const char *side;
  const char *action = "dropped";
  std::atomic<bool> said{false};

  // a check here fired: counts it in `to`, and gives the line to write the first time, else ""
  std::string
  fired(const Overrun &overrun, Counts &to = counts) {
    to.add(call);
    return said.exchange(true, std::memory_order_relaxed) ? std::string() : Line(call, side, overrun, action);
  }
};

} // namespace mullion::ranges
