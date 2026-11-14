// contract (jdrly/mullion, private ticket 33): with MULLION_D3D12_CHECK_RANGES=1 and MULLION_D3D12_GUARD_STATIC_RANGES=1
// a check a shader makes itself, which the CPU doesn't see, counts where it fails, and the device reads the counts back:
// at each STATS line and once when it ends, `MULLION-RANGE call=Shader what=<kind> given=<count> action=checked` the
// first time a kind counts in the process. One thread reads an element past a static range's view, an element past a
// root SRV's buffer, and through an unbounded table an index past the heap, which is clamped: each kind fails once, so
// each counts 1. It reads 0, 0 and the table's last descriptor's first word. Its control,
// tests/breaks/d3d12_shader_check_counts.patch, converts shaders without the counting, which leaves no line. Run it on
// the rented M1 only (private ticket 15)
#include "d3d12_test.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> fixed : register(t0);
StructuredBuffer<uint> root : register(t1);
Buffer<uint> views[] : register(t2);
cbuffer I : register(b0) { uint at, past; };
RWStructuredBuffer<uint> o : register(u0);
[numthreads(1, 1, 1)]
void cs() {
  o[0] = fixed[at];
  o[1] = root[at];
  o[2] = views[NonUniformResourceIndex(past)][0];
}
)hlsl";

int
main(int argc, char **argv) {
  char temporary[MAX_PATH], directory[MAX_PATH];
  auto length = GetTempPathA(sizeof(temporary), temporary);
  CHECK(length && length < sizeof(temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "chk", directory) ? S_OK : E_FAIL);
  CHECK(SetEnvironmentVariableA("MULLION_LOG_PATH", directory) ? S_OK : E_FAIL);
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::string cs;
  if (compiler.dxc)
    cs = compiler.compile(hlsl, "cs", "cs");
  else {
    // D3DCOMPILE Constants: "Directs the compiler to enable unbounded descriptor tables" (as d3d12_root_ranges)
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "cs", "cs_5_1",
                            D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code, &errors);
    if (FAILED(hr) && errors)
      printf("cs: %.*s\n", int(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer()));
    if (SUCCEEDED(hr))
      cs = kept(std::string(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize()), "cs");
  }
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  const UINT bytes = 256, values = 42;
  {
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    // t0 through a static range, t1 a root SRV, t2 on through a volatile unbounded range, two constants, u0 a root UAV
    D3D12_DESCRIPTOR_RANGE1 fixed{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE},
        unbounded{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT(-1), 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE};
    D3D12_ROOT_PARAMETER1 params[5] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                       {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                       {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
    params[0].DescriptorTable = {1, &fixed};
    params[1].Descriptor = {1, 0};
    params[2].DescriptorTable = {1, &unbounded};
    params[3].Constants = {0, 0, 2};
    params[4].Descriptor = {0, 0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc{D3D_ROOT_SIGNATURE_VERSION_1_1};
    rs_desc.Desc_1_1 = {(UINT)std::size(params), params};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D12RootSignature> rs;
    CHECK(D3D12SerializeVersionedRootSignature(&rs_desc, &blob, nullptr));
    CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
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

    // a heap of two: a structured view of four elements, and a typed view whose first word is 42
    auto data = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(data && output && back, "buffers"))
      return verdict();
    UINT *words;
    CHECK(data->Map(0, nullptr, (void **)&words));
    for (UINT i = 0; i < bytes / sizeof(UINT); i++)
      words[i] = i ? 1 : values;
    data->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER,
                                               D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    structured.Buffer = {0, 4, sizeof(UINT), D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(data.Get(), &structured, heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC typed{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER,
                                          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    typed.Buffer = {0, bytes / sizeof(UINT), 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(data.Get(), &typed, {heap->GetCPUDescriptorHandleForHeapStart().ptr + increment});

    // element 100000 past the static view's 4 and past the root SRV's buffer, whose Metal allocation may be longer than
    // its 256 bytes; index 5 past the one descriptor the heap has from the unbounded range's start, clamped to it
    step("one thread past a static range, a root SRV and the heap");
    const UINT indices[2] = {100000, 5};
    CHECK(forget(back.Get()));
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootShaderResourceView(1, data->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, {heap->GetGPUDescriptorHandleForHeapStart().ptr + increment});
    list->SetComputeRoot32BitConstants(3, 2, indices, 0);
    list->SetComputeRootUnorderedAccessView(4, output->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, bytes);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const UINT *got;
    CHECK(back->Map(0, nullptr, (void **)&got));
    expect(got[0] == 0, "an element past the static range's view reads %u, want 0", got[0]);
    expect(got[1] == 0, "an element past the root SRV's buffer reads %u, want 0", got[1]);
    expect(got[2] == values, "an index past the heap reads %u, want %u (the table's last descriptor)", got[2], values);
    back->Unmap(0, nullptr);
  }

  // the device's end reads its words a last time: one line per kind, each counted once
  step("the device's end reads the counts back");
  const std::string path = std::string(directory) + "\\d3d12_shader_check_counts_d3d12.log";
  const char *const want[] = {"MULLION-RANGE call=Shader what=staticRange given=1 action=checked",
                              "MULLION-RANGE call=Shader what=rootView given=1 action=checked",
                              "MULLION-RANGE call=Shader what=descriptorIndex given=1 action=checked"};
  std::string lines;
  // the device may end a little after its last reference: its queue's thread lets it go
  for (int tries = 0; tries < 100; tries++, Sleep(100)) {
    std::ifstream log(path);
    lines.assign(std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>());
    if (std::all_of(std::begin(want), std::end(want), [&](auto line) { return lines.find(line) != std::string::npos; }))
      break;
  }
  for (auto line : want) {
    auto at = lines.find(line);
    expect(at != std::string::npos && lines.find(line, at + 1) == std::string::npos, "once in the log: %s", line);
  }
  unsigned said = 0;
  for (size_t at = 0; (at = lines.find("call=Shader", at)) != std::string::npos; at++)
    said++;
  expect(said == std::size(want), "%u lines of shaders' checks, want %zu", said, std::size(want));
  printf("test log: %s\n", path.c_str());
  return verdict();
}
