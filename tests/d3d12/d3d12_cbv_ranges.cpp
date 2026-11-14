// contract (jdrly/mullion, MULLION_D3D12_CHECK_RANGES=1, private ticket 20 part 3): a constant buffer view whose bytes
// run past the buffer its address lies in is made a null view, which a shader reads as zeros ("BufferLocation ... NULL
// is allowed", D3D12_CONSTANT_BUFFER_VIEW_DESC; D3D11.3 7.5); one inside its buffer reads the buffer. A
// WriteBufferImmediate whose destination lies in no live buffer is dropped, a MARKER_IN one too, and a write inside its
// buffer after them lands, with every other byte of the buffer as it was.
// The view reads only its first 16 bytes and the writes go to a released buffer's address. Its control,
// tests/breaks/d3d12_cbv_ranges.patch, takes both as given: the view then reads the buffer's values, and under Metal's
// validation (tests/run.py --validate) the writes are reported, without it they go past another buffer's end. Run it
// on the rented M1 only (private ticket 15)
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint4 v; };
RWStructuredBuffer<uint4> o : register(u0);
[numthreads(1, 1, 1)]
void cs() { o[0] = v; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE cbvs{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].DescriptorTable = {1, &cbvs};
  auto rs = root_signature(device.Get(), {2, params});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList2> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // 256 bytes of constants, the least Mullion gives a buffer, so a view of 512 runs past its memory; the output, its
  // readback, a buffer for the immediate writes and its seed, and a buffer released before anything runs, whose
  // address is no live buffer's
  const UINT bytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  auto constants = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto seed = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto released = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(constants && output && back && written && seed && released, "buffers"))
    return verdict();
  const D3D12_GPU_VIRTUAL_ADDRESS gone = released->GetGPUVirtualAddress();
  released.Reset();
  UINT *data;
  CHECK(constants->Map(0, nullptr, (void **)&data));
  for (UINT i = 0; i < bytes / sizeof(UINT); i++)
    data[i] = i + 1;
  constants->Unmap(0, nullptr);
  CHECK(seed->Map(0, nullptr, (void **)&data));
  memset(data, 0x5a, bytes);
  seed->Unmap(0, nullptr);

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CONSTANT_BUFFER_VIEW_DESC inside{constants->GetGPUVirtualAddress(), bytes},
      past{constants->GetGPUVirtualAddress(), 2 * bytes};
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateConstantBufferView(&inside, cpu);
  device->CreateConstantBufferView(&past, {cpu.ptr + increment});

  // each view's first 16 bytes, through the shader
  for (UINT slot : {0u, 1u}) {
    const char *what = slot ? "a view of 512 bytes over 256" : "a view inside its buffer";
    step("constant buffer view: %s", what);
    CHECK(forget(back.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, {heap->GetGPUDescriptorHandleForHeapStart().ptr + slot * increment});
    list->Dispatch(1, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, 16);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(back->Map(0, nullptr, (void **)&data));
    for (UINT i = 0; i < 4; i++) {
      UINT want = slot ? 0 : i + 1;
      if (!expect(data[i] == want, "%s: v[%u] is %#x, want %#x", what, i, data[i], want))
        break;
    }
    back->Unmap(0, nullptr);
  }

  // immediate writes: two at the released buffer's address, one DEFAULT and one MARKER_IN, then one inside the buffer
  step("WriteBufferImmediate: to a released buffer's address, then inside a buffer");
  CHECK(forget(back.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  list->CopyBufferRegion(written.Get(), 0, seed.Get(), 0, bytes);
  const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER writes[] = {
      {gone, 0xdead0001}, {gone + 4, 0xdead0002}, {written->GetGPUVirtualAddress() + 8, 0x12345678}
  };
  const D3D12_WRITEBUFFERIMMEDIATE_MODE modes[] = {
      D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
      D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT
  };
  list->WriteBufferImmediate(3, writes, modes);
  transition(list.Get(), written.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(back.Get(), 0, written.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(back->Map(0, nullptr, (void **)&data));
  for (UINT i = 0; i < bytes / sizeof(UINT); i++) {
    UINT want = i == 2 ? 0x12345678 : 0x5a5a5a5a;
    if (!expect(data[i] == want, "word %u of the written buffer is %#x, want %#x", i, data[i], want))
      break;
  }
  back->Unmap(0, nullptr);
  return verdict();
}
