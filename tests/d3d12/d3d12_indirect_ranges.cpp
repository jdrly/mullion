// contract (jdrly/mullion, MULLION_D3D12_CHECK_RANGES=1, private ticket 20 part 4): an ExecuteIndirect whose commands
// run past its argument buffer, MaxCommandCount of the signature's stride from ArgumentBufferOffset with the last taking
// only its arguments' bytes, or whose count runs past its count buffer, is dropped whole; one that ends exactly at its
// buffer's width runs. Direct3D leaves reading past either buffer undefined.
// Each case dispatches groups that each add 1 to a word of their own, through a signature of one DISPATCH argument
// (12 bytes) and a stride of 16. Its control, tests/breaks/d3d12_indirect_ranges.patch, runs every call as given: the
// first command of a call whose second runs past its buffer then adds 1. Run it on the rented M1 only (private ticket
// 15)
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer o : register(u0);
[numthreads(1, 1, 1)]
void cs() {
  o.InterlockedAdd(0, 1);
}
)hlsl";

// an upload buffer of `width` bytes holding `words`, where indirect arguments can be read
static ComPtr<ID3D12Resource>
words(ID3D12Device *device, UINT64 width, std::initializer_list<UINT> values) {
  auto res = buffer(device, D3D12_HEAP_TYPE_UPLOAD, width, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *data;
  if (!res || FAILED(res->Map(0, nullptr, (void **)&data)))
    return nullptr;
  memset(data, 0, width);
  std::copy(values.begin(), values.end(), data);
  res->Unmap(0, nullptr);
  return res;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::string cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  D3D12_INDIRECT_ARGUMENT_DESC dispatch{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{16, 1, &dispatch};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));

  // two commands of one group each in 28 bytes, the second's arguments ending at the width; and one command with a
  // fourth word of 1 in 16, which holds one command but not two, and serves as a count of 1 at offset 12
  auto two = words(device.Get(), 28, {1, 1, 1, 0, 1, 1, 1});
  auto one = words(device.Get(), 16, {1, 1, 1, 1});
  const UINT bytes = 256;
  auto zeros = words(device.Get(), bytes, {});
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(two && one && zeros && output && back, "buffers"))
    return verdict();

  const struct {
    const char *what;
    ID3D12Resource *args;
    UINT64 offset;
    UINT count;
    ID3D12Resource *counts;
    UINT64 count_offset;
    UINT want;
  } cases[] = {
      {"two commands ending at the width", two.Get(), 0, 2, nullptr, 0, 2},
      {"two commands, the second past the width", one.Get(), 0, 2, nullptr, 0, 0},
      {"one command from offset 4, ending at the width", one.Get(), 4, 1, nullptr, 0, 1},
      {"one command at the width", one.Get(), 16, 1, nullptr, 0, 0},
      {"a count of 1 ending at its width", two.Get(), 0, 2, one.Get(), 12, 1},
      {"a count at its width", two.Get(), 0, 2, one.Get(), 16, 0},
  };
  CHECK(forget(back.Get()));
  list->CopyBufferRegion(output.Get(), 0, zeros.Get(), 0, bytes);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetComputeRootSignature(rs.Get());
  for (UINT i = 0; i < std::size(cases); i++) {
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + i * 16);
    list->ExecuteIndirect(signature.Get(), cases[i].count, cases[i].args, cases[i].offset, cases[i].counts,
                          cases[i].count_offset);
  }
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT *got;
  CHECK(back->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < std::size(cases); i++)
    expect(got[4 * i] == cases[i].want, "%s: %u groups ran, want %u", cases[i].what, got[4 * i], cases[i].want);
  back->Unmap(0, nullptr);
  return verdict();
}
