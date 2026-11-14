// contract (jdrly/mullion, MULLION_D3D12_CHECK_RANGES=1, private ticket 20 part 1): a CopyBufferRegion or
// CopyTextureRegion whose offset, size, box, region or footprint runs past its resource is dropped whole, and the list
// goes on: the destination keeps every byte it had, and an in-range copy after it lands. Direct3D calls such a copy
// undefined, and its debug layer rejects it (CopyBufferRegion, CopyTextureRegion, Remarks). A texture given to
// CopyBufferRegion is dropped too.
// Each copy reaches at most 272 bytes past a buffer or 4 texels past a texture. Its control,
// tests/breaks/d3d12_copy_ranges.patch, hands them to Metal: under Metal's validation (tests/run.py --validate) Metal
// reports them, without it the destination changes or the texture case crashes, and the test fails either way. Run it
// on the rented M1 only (private ticket 15)
#include "d3d12_test.hpp"
#include <functional>
#include <tuple>

static UINT8
value(UINT64 i) {
  return UINT8(i * 7 + 1);
}

int
main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the copies' sources hold value(i), the seeds ~value(i); 1024 bytes hold a 4x4 RGBA8 footprint at 0 or 128
  const UINT64 bytes = 256, upload_bytes = 1024, pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto seed = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto target = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto small = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, upload_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ComPtr<ID3D12Resource> texture;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&texture));
  if (!expect(source && seed && target && small && back && texture, "resources"))
    return verdict();
  for (auto [resource, size, flip] : {std::tuple{source.Get(), upload_bytes, false},
                                      std::tuple{small.Get(), bytes, false}, std::tuple{seed.Get(), upload_bytes, true}}) {
    UINT8 *data;
    CHECK(resource->Map(0, nullptr, (void **)&data));
    for (UINT64 i = 0; i < size; i++)
      data[i] = flip ? UINT8(~value(i)) : value(i);
    resource->Unmap(0, nullptr);
  }

  // records one case between the seed and the readback, runs it and gives the readback's bytes
  auto run = [&](const std::function<void()> &record, std::vector<UINT8> &got) -> HRESULT {
    HRESULT hr;
    if (FAILED(hr = forget(back.Get())) || FAILED(hr = allocator->Reset()) ||
        FAILED(hr = list->Reset(allocator.Get(), nullptr)))
      return hr;
    record();
    if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())))
      return hr;
    UINT8 *data;
    if (FAILED(hr = back->Map(0, nullptr, (void **)&data)))
      return hr;
    got.assign(data, data + upload_bytes);
    back->Unmap(0, nullptr);
    return S_OK;
  };
  std::vector<UINT8> got;

  // CopyBufferRegion into a 256-byte buffer from a 256-byte one; the last case is in range and lands
  struct BufferCase {
    const char *what;
    UINT64 dst, src, count;
    bool lands;
  };
  for (auto c : {BufferCase{"the source past its end", 0, 192, 128, false},
                 BufferCase{"the destination past its end", 200, 0, 64, false},
                 BufferCase{"an offset past the source", 0, 260, 4, false}, BufferCase{"in range", 16, 32, 64, true}}) {
    step("CopyBufferRegion: %s", c.what);
    CHECK(run(
        [&] {
          list->CopyBufferRegion(target.Get(), 0, seed.Get(), 0, bytes);
          list->CopyBufferRegion(target.Get(), c.dst, small.Get(), c.src, c.count);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(back.Get(), 0, target.Get(), 0, bytes);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        },
        got
    ));
    for (UINT64 b = 0; b < bytes; b++) {
      UINT8 want = c.lands && b >= c.dst && b - c.dst < c.count ? value(c.src + b - c.dst) : UINT8(~value(b));
      if (!expect(got[b] == want, "%s: byte %llu is %#x, want %#x", c.what, b, got[b], want))
        break;
    }
  }

  // CopyTextureRegion with a 4x4 RGBA8 texture, seeded from a footprint of ~value(i) and read back through one
  auto footprint = [&](ID3D12Resource *resource, UINT64 offset) {
    D3D12_TEXTURE_COPY_LOCATION location{resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    location.PlacedFootprint = {offset, {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, UINT(pitch)}};
    return location;
  };
  D3D12_TEXTURE_COPY_LOCATION level{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  level.SubresourceIndex = 0;
  auto from_seed = footprint(seed.Get(), 0), to_back = footprint(back.Get(), 0);
  // the byte of texel (x, y) at channel `c` in a footprint at offset 0
  auto at = [&](UINT x, UINT y, UINT c) { return y * pitch + x * 4 + c; };
  struct TextureCase {
    const char *what;
    std::function<void()> record;
    // whether texel (x, y) holds the source's texel at the same place less (dx, dy) after the case
    std::function<bool(UINT, UINT)> copied;
    UINT dx, dy;
  };
  auto into_texture = [&](std::function<void()> copy) {
    return [&, copy] {
      list->CopyTextureRegion(&level, 0, 0, 0, &from_seed, nullptr);
      copy();
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyTextureRegion(&to_back, 0, 0, 0, &level, nullptr);
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    };
  };
  auto none = [](UINT, UINT) { return false; };
  // a footprint at 512 of a 1024-byte buffer covers 3 pitches and a row, 784 bytes: 272 past its end
  auto past_buffer = footprint(source.Get(), 512), whole = footprint(source.Get(), 0);
  D3D12_BOX two{0, 0, 0, 2, 2, 1};
  std::vector<TextureCase> cases;
  cases.push_back({"a footprint past its buffer",
                   into_texture([&] { list->CopyTextureRegion(&level, 0, 0, 0, &past_buffer, nullptr); }), none});
  cases.push_back({"a region past the texture",
                   into_texture([&] { list->CopyTextureRegion(&level, 2, 0, 0, &whole, nullptr); }), none});
  cases.push_back({"in range", into_texture([&] { list->CopyTextureRegion(&level, 1, 1, 0, &whole, &two); }),
                   [](UINT x, UINT y) { return x >= 1 && x < 3 && y >= 1 && y < 3; }, 1, 1});
  for (auto &c : cases) {
    step("CopyTextureRegion: %s", c.what);
    CHECK(run(c.record, got));
    bool same = true;
    for (UINT y = 0; y < 4 && same; y++)
      for (UINT x = 0; x < 4 && same; x++)
        for (UINT ch = 0; ch < 4 && same; ch++) {
          UINT8 want = c.copied(x, y) ? value(at(x - c.dx, y - c.dy, ch)) : UINT8(~value(at(x, y, ch)));
          same = expect(got[at(x, y, ch)] == want, "%s: texel (%u, %u) channel %u is %#x, want %#x", c.what, x, y, ch,
                        got[at(x, y, ch)], want);
        }
  }

  // a source box past the texture, into the readback: nothing is written there, which forget filled with 0xff
  step("CopyTextureRegion: a source box past the texture");
  D3D12_BOX wide{0, 0, 0, 8, 4, 1};
  CHECK(run(
      [&] {
        list->CopyTextureRegion(&level, 0, 0, 0, &from_seed, nullptr);
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&to_back, 0, 0, 0, &level, &wide);
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      },
      got
  ));
  for (UINT64 b = 0; b < upload_bytes; b++)
    if (!expect(got[b] == 0xff, "a source box past the texture: byte %llu is %#x, want 0xff", b, got[b]))
      break;

  // a texture where CopyBufferRegion wants a buffer: dropped, and the buffer keeps its seed. Last, as without the
  // check it is a null dereference
  step("CopyBufferRegion: a texture as its source");
  CHECK(run(
      [&] {
        list->CopyBufferRegion(target.Get(), 0, seed.Get(), 0, bytes);
        list->CopyBufferRegion(target.Get(), 0, texture.Get(), 0, 16);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(back.Get(), 0, target.Get(), 0, bytes);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      },
      got
  ));
  for (UINT64 b = 0; b < bytes; b++)
    if (!expect(got[b] == UINT8(~value(b)), "a texture as its source: byte %llu is %#x, want %#x", b, got[b],
                UINT8(~value(b))))
      break;
  return verdict();
}
