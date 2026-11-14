// contract (jdrly/mullion, MULLION_D3D12_RESET_WAITS=1): a command allocator reset while a list recorded from it is
// still on the GPU returns only once that list has completed, and the list runs as recorded. Direct3D 12 forbids the
// reset ("the app must ensure the GPU is no longer executing any command lists that are associated with the command
// allocator", ID3D12CommandAllocator::Reset) and games make it anyway; upstream's Reset gives the list's recording
// back at once, for the next list to record over while the queue still encodes it and the GPU still reads it.
// here the first list's first dispatch runs for milliseconds, and its other 63 come after a UAV barrier, so they read
// their arguments late; the allocator is reset right after ExecuteCommandLists and a second list recorded from it with
// other arguments. every word of the first list's output is there when the Reset returns, read through a CPU-visible
// buffer before anything else runs, and both lists' outputs are as recorded after the second.
#include "d3d12_test.hpp"
#include <vector>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> dst : register(u0);
cbuffer C : register(b0) { uint value; uint spin; };
[numthreads(64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
  uint v = value + tid.x;
  for (uint i = 0; i < spin; i++)
    v = v * 1664525u + 1013904223u;
  dst[tid.x] = v;
}
)hlsl";

constexpr UINT kThreads = 64, kDispatches = 64, kWords = kThreads * kDispatches;
// the first dispatch's steps, each a multiply-add on the one before: milliseconds on an Apple GPU
constexpr UINT kSpin = 1u << 22;

static UINT
spun(UINT v, UINT spin) {
  for (UINT i = 0; i < spin; i++)
    v = v * 1664525u + 1013904223u;
  return v;
}

// what dispatch k of list `n` writes: its value, and only the first list's first dispatch spins
static UINT
value(UINT round, UINT n, UINT k) {
  return (n + 1) << 24 | round << 16 | k << 8;
}

// the words of list `n`'s output that aren't what it recorded, the first few printed
static unsigned
mismatches(const UINT *got, const std::vector<UINT> &first_dispatch, UINT round, UINT n, const char *when) {
  unsigned count = 0;
  for (UINT k = 0; k < kDispatches; k++)
    for (UINT t = 0; t < kThreads; t++) {
      UINT want = n == 0 && k == 0 ? first_dispatch[t] : value(round, n, k) + t;
      if (got[k * kThreads + t] != want && count++ < 4)
        printf("  %s: list %u, dispatch %u, thread %u is %#x, want %#x\n", when, n, k, t, got[k * kThreads + t], want);
    }
  return count;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (code.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {2, params});
  if (!expect(rs != nullptr, "root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  // the outputs, which the CPU reads as the GPU left them
  D3D12_HEAP_PROPERTIES visible{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, kWords * sizeof(UINT), 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> outputs[2];
  for (auto &output : outputs)
    CHECK(device->CreateCommittedResource(&visible, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  UINT64 signal = 0;

  // list `n` of the round into its output: 64 dispatches of a thread group each, the first spinning for list 0
  auto record = [&](UINT round, UINT n) {
    list->SetComputeRootSignature(rs.Get());
    auto address = outputs[n]->GetGPUVirtualAddress();
    for (UINT k = 0; k < kDispatches; k++) {
      UINT constants[2] = {value(round, n, k), n == 0 && k == 0 ? kSpin : 0};
      list->SetComputeRootUnorderedAccessView(0, address + UINT64(k) * kThreads * sizeof(UINT));
      list->SetComputeRoot32BitConstants(1, 2, constants, 0);
      list->Dispatch(1, 1, 1);
      // the dispatches after the first start, and read their arguments, once it has ended
      if (k == 0) {
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        barrier.UAV.pResource = outputs[n].Get();
        list->ResourceBarrier(1, &barrier);
      }
    }
  };

  std::vector<UINT> first_dispatch(kThreads);
  for (UINT round = 0; round < 4; round++) {
    step("round %u", round);
    for (UINT t = 0; t < kThreads; t++)
      first_dispatch[t] = spun(value(round, 0, 0) + t, kSpin);
    for (auto &output : outputs)
      CHECK(forget(output.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    record(round, 0);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    // at once, while the first dispatch runs: the reset Direct3D forbids
    CHECK(allocator->Reset());
    // the first list has completed by now, and wrote what it recorded
    const UINT *got;
    CHECK(outputs[0]->Map(0, nullptr, (void **)&got));
    expect(!mismatches(got, first_dispatch, round, 0, "at the reset"),
           "round %u: the first list completed before the reset returned", round);
    outputs[0]->Unmap(0, nullptr);

    // the second list, recorded into what the first had, with other arguments
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    record(round, 1);
    CHECK(list->Close());
    queue->ExecuteCommandLists(1, lists);
    CHECK(queue->Signal(fence.Get(), ++signal));
    CHECK(fence->SetEventOnCompletion(signal, event));
    WaitForSingleObject(event, INFINITE);
    CHECK(device->GetDeviceRemovedReason());
    for (UINT n = 0; n < 2; n++) {
      CHECK(outputs[n]->Map(0, nullptr, (void **)&got));
      expect(!mismatches(got, first_dispatch, round, n, "after both"), "round %u: list %u ran as recorded", round, n);
      outputs[n]->Unmap(0, nullptr);
    }
  }
  CloseHandle(event);
  return verdict();
}
