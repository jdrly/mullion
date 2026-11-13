// contract: a command buffer the GPU failed removes the device with DXGI_ERROR_DEVICE_HUNG, which
// GetDeviceRemovedReason then answers ("the GPU will not accept any more commands", Microsoft Learn,
// ID3D12Device::GetDeviceRemovedReason), and writes one MULLION-GPU-ERROR line (docs/swtor/README.md).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getdeviceremovedreason
// The failure is the bridge's test hook, in test builds only: MULLION_TEST_GPU_ERROR=3 makes every command buffer that
// completes read as failed with MTLCommandBufferErrorPageFault, so no GPU faults. Without it the copy completes and the
// device stays. The line itself is checked in the test's log, by hand or by tests/run.py, which counts it as a fault.
#include "d3d12_test.hpp"

int
main(int, char **) {
  char hook[16] = {};
  const bool failing = GetEnvironmentVariableA("MULLION_TEST_GPU_ERROR", hook, sizeof(hook)) && atoi(hook);
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 4096, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 4096, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(source && readback, "the buffers are refused"))
    return verdict();
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(readback.Get(), 0, source.Get(), 0, 4096);
  const HRESULT reason = submit(device.Get(), queue.Get(), list.Get());
  if (failing)
    expect(reason == DXGI_ERROR_DEVICE_HUNG, "MULLION_TEST_GPU_ERROR=%s: the removal reason is 0x%08lx, not "
           "DXGI_ERROR_DEVICE_HUNG (a build without -Denable_tests=true has no hook)", hook, reason);
  else
    expect(reason == S_OK, "without MULLION_TEST_GPU_ERROR the device is removed: 0x%08lx", reason);
  return verdict();
}
