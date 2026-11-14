// contract (jdrly/mullion, MULLION_D3D12_CHECK_RANGES=1, private ticket 20 part 6): a dynamic descriptor index through a
// table is clamped below the descriptors the heap has from the table's start, so that an index past the heap reads the
// heap's last descriptor instead of memory past the heap's buffer (note 7 § 1.2: "an unbounded array"). Direct3D
// leaves an index past the heap undefined. An index inside reads its own descriptor.
// A heap of four typed views, each of a buffer whose first word is 10, 20, 30 or 40, is read through an unbounded
// range at the heap's start and at its third descriptor. It has no GPU control: without the clamp the index past the
// heap reads a descriptor from past the heap's buffer, a GPU address no one made. host_check_root_ranges' controls
// stand for it in the converter (tests/breaks/host_check_root_ranges.{8,9,10,11}.patch). Run it on the rented M1
// only (private ticket 15)
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
Buffer<uint> views[] : register(t0);
cbuffer I : register(b0) { uint inside, past; };
RWStructuredBuffer<uint> o : register(u0);
[numthreads(1, 1, 1)]
void cs() {
  o[0] = views[NonUniformResourceIndex(inside)][0];
  o[1] = views[NonUniformResourceIndex(past)][0];
}
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
  D3D12_DESCRIPTOR_RANGE unbounded{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT(-1), 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &unbounded};
  params[1].Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {3, params});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));

  // four buffers of one value each, a typed view of each in a heap of exactly four
  const UINT views = 4, bytes = 256;
  ComPtr<ID3D12Resource> values[views];
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, views,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (UINT i = 0; i < views; i++) {
    values[i] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(bool(values[i]), "buffer %u", i))
      return verdict();
    UINT *data;
    CHECK(values[i]->Map(0, nullptr, (void **)&data));
    data[0] = 10 * (i + 1);
    values[i]->Unmap(0, nullptr);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER,
                                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer = {0, bytes / sizeof(UINT), 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(values[i].Get(), &srv,
                                     {heap->GetCPUDescriptorHandleForHeapStart().ptr + i * increment});
  }
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(output && back, "buffers"))
    return verdict();

  // the table at the heap's start: index 2 inside, 10 past (clamped to 3); at its third descriptor: index 1 inside
  // (the fourth descriptor), 5 past (clamped to 1, the fourth descriptor too)
  const struct {
    UINT start, inside, past, want_inside, want_past;
  } cases[] = {{0, 2, 10, 30, 40}, {2, 1, 5, 40, 40}};
  CHECK(forget(back.Get()));
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  for (UINT i = 0; i < std::size(cases); i++) {
    const UINT indices[2] = {cases[i].inside, cases[i].past};
    list->SetComputeRootDescriptorTable(0, {heap->GetGPUDescriptorHandleForHeapStart().ptr + cases[i].start * increment});
    list->SetComputeRoot32BitConstants(1, 2, indices, 0);
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress() + i * 16);
    list->Dispatch(1, 1, 1);
  }
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT *got;
  CHECK(back->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < std::size(cases); i++) {
    expect(got[4 * i] == cases[i].want_inside, "a table at descriptor %u: index %u reads %u, want %u", cases[i].start,
           cases[i].inside, got[4 * i], cases[i].want_inside);
    expect(got[4 * i + 1] == cases[i].want_past, "a table at descriptor %u: index %u past the heap reads %u, want %u",
           cases[i].start, cases[i].past, got[4 * i + 1], cases[i].want_past);
  }
  back->Unmap(0, nullptr);
  return verdict();
}
