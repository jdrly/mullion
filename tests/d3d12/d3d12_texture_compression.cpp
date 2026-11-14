// jdrly/mullion. contract: with MULLION_TEX_MSAA_COMPRESSION=auto (the test's environment), a multisampled texture
// and a later single-sampled 2D texture of its type group and size, its resolve target, are made without Metal's
// lossless compression on a GPU before Apple10, as the bridge answers the family (MULLION_GPU_FAMILY too: run.py runs
// this as 9, 8 and 7), and with it on Apple10; a texture made before the multisampled one, or of another size or
// group, keeps it on every GPU. what the device asked the bridge for is read back through the device's own
// interface for D3D11On12 (GetResourceObjects); whether Metal then compresses is the host test's
// (tests/host/texture_compression.c). no rendering
#include "d3d12_test.hpp"
#include "../../src/dxgi/dxgi_interfaces.h"

static ComPtr<ID3D12Resource>
Target(ID3D12Device *device, DXGI_FORMAT format, UINT width, UINT height, UINT samples) {
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             nullptr, IID_PPV_ARGS(&resource))))
    resource = nullptr;
  return resource;
}

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<IMTLD3D11On12Source> source;
  CHECK(device->QueryInterface(IID_PPV_ARGS(&source)));
  bool apple10 = source->GetMTLDevice().supportsFamily(WMTGPUFamilyApple10);
  printf("apple10 %d: multisampled textures and resolve targets %s\n", apple10, apple10 ? "compressed" : "uncompressed");

  int failures = 0;
  // the field the device set for `resource`, -1 when it has no Metal texture
  auto uncompressed = [&](ID3D12Resource *resource) {
    MullionMetalResource info;
    if (!resource || FAILED(source->GetResourceObjects(resource, &info)) || !info.texture)
      return -1;
    NSObject_release(info.texture);
    return int(info.texture_info.uncompressed);
  };
  auto expect = [&](const char *what, ID3D12Resource *resource, bool want) {
    int got = uncompressed(resource);
    if (got != int(want)) {
      printf("failed: %s: uncompressed %d, want %d\n", what, got, want);
      failures++;
    }
  };

  auto before = Target(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1);
  expect("a target made before any multisampled texture", before.Get(), false);
  auto msaa = Target(device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 64, 64, 4);
  expect("a 4x texture", msaa.Get(), !apple10);
  auto resolve = Target(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 64, 64, 1);
  expect("a target of its group and size", resolve.Get(), !apple10);
  auto other_size = Target(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 64, 32, 1);
  expect("a target of another size", other_size.Get(), false);
  auto other_group = Target(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64, 1);
  expect("a target of another group", other_group.Get(), false);

  return failures ? 1 : 0;
}
