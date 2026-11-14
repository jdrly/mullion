// contract (jdrly/mullion, MULLION_D3D12_CHECK_RANGES=1, private ticket 20 part 5): a root CBV, SRV or UAV is checked
// against what its buffer has from its address. A row of a root constant buffer, or an element of a root SRV or UAV,
// past that buffer's end reads 0, and a store there writes nothing; one inside reads the buffer. Direct3D gives a root
// descriptor no size and leaves an access past its buffer undefined: Mullion's other buffer views read 0 there (D3D11.3
// 7.5, 22.4.12), and so do these with the switch.
// A root SRV with fewer bytes left in its buffer than one load reads keeps them: element 1 of one 16 bytes before its
// buffer's end reads the buffer, element 100 reads 0, and a uint4 element with 8 of its 16 bytes left reads 0 whole
// (22.4.12); the load a failed check drops reads the lock table, not past the buffer.
// With `indirect` (argv[2]), an ExecuteIndirect that sets a root SRV is followed by a Dispatch of the same root
// signature: Direct3D resets the view the command set, and the shader reads 0 through it. Under the control that read
// goes to address 0, so the control runs without `indirect`.
// Each buffer is 256 bytes, the least Mullion gives one, and each access past it lands 144 to 400 bytes beyond. Its
// control, tests/breaks/d3d12_root_view_ranges.patch, converts without the checks: the reads then return what lies behind
// the buffers, or Metal's validation (tests/run.py --validate) reports them. Run it on the rented M1 only (private
// ticket 15)
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint4 rows[64]; };
cbuffer I : register(b1) { uint row, element, past; };
StructuredBuffer<uint> s : register(t0);
StructuredBuffer<uint4> v : register(t2);
RWStructuredBuffer<uint> u : register(u0);
RWStructuredBuffer<uint> o : register(u1);
[numthreads(1, 1, 1)]
void cs() {
  o[0] = rows[row].x;
  o[1] = s[element];
  o[2] = u[element];
  u[past] = 0xdead;
  o[3] = rows[1].x;
  o[4] = s[1];
  o[5] = u[1];
  uint4 w = v[0];
  o[6] = w.x;
  o[7] = w.y;
  o[8] = w.z;
  o[9] = w.w;
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
  const bool indirect = argc > 2 && !strcmp(argv[2], "indirect");
  D3D12_ROOT_PARAMETER params[6] = {{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_SRV},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  params[3].Descriptor.ShaderRegister = 1;
  params[4].Constants = {1, 0, 3};
  params[5].Descriptor.ShaderRegister = 2;
  auto rs = root_signature(device.Get(), {6, params});
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

  // the constants, the SRV's and the UAV's buffers, word i holding i + 1, 100 + i and 200 + i; the output and its
  // readback
  const UINT bytes = 256, words = bytes / sizeof(UINT);
  auto constants = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto structured = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto unordered = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  // an ExecuteIndirect command: the root SRV, then a dispatch of one group
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(constants && structured && upload && unordered && output && back && arguments, "buffers"))
    return verdict();
  for (auto [resource, base] : {std::pair{constants.Get(), 1u}, std::pair{structured.Get(), 100u},
                                std::pair{upload.Get(), 200u}}) {
    UINT *data;
    CHECK(resource->Map(0, nullptr, (void **)&data));
    for (UINT i = 0; i < words; i++)
      data[i] = base + i;
    resource->Unmap(0, nullptr);
  }

  // row 20 of 16, element 100 of 64; the store goes to element 100 of the UAV's 64 too
  const UINT indices[3] = {20, 100, 100};
  CHECK(forget(back.Get()));
  list->CopyBufferRegion(unordered.Get(), 0, upload.Get(), 0, bytes);
  transition(list.Get(), unordered.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
  list->SetComputeRootShaderResourceView(1, structured->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, unordered->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress());
  list->SetComputeRoot32BitConstants(4, 3, indices, 0);
  // a uint4 element with 8 of its 16 bytes in the buffer
  list->SetComputeRootShaderResourceView(5, structured->GetGPUVirtualAddress() + bytes - 8);
  list->Dispatch(1, 1, 1);
  // again, the SRV 16 bytes before its buffer's end and the output 64 bytes in
  list->SetComputeRootShaderResourceView(1, structured->GetGPUVirtualAddress() + bytes - 16);
  list->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress() + 64);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), unordered.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, bytes);
  list->CopyBufferRegion(back.Get(), bytes, unordered.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const UINT *got;
  CHECK(back->Map(0, nullptr, (void **)&got));
  const struct {
    const char *what;
    UINT want;
  } outputs[] = {{"row 20 of a root CBV of 16 rows", 0},     {"element 100 of a root SRV of 64", 0},
                 {"element 100 of a root UAV of 64", 0},     {"row 1 of the root CBV", 5},
                 {"element 1 of the root SRV", 101},         {"element 1 of the root UAV", 201}};
  for (UINT i = 0; i < std::size(outputs); i++)
    expect(got[i] == outputs[i].want, "%s reads %#x, want %#x", outputs[i].what, got[i], outputs[i].want);
  // the second dispatch: the SRV with 16 bytes left reads element 1, which is inside them, and 0 at element 100
  expect(got[16 + 4] == 161, "element 1 of a root SRV 16 bytes before its buffer's end reads %#x, want 0xa1", got[16 + 4]);
  expect(got[16 + 1] == 0, "element 100 of a root SRV 16 bytes before its buffer's end reads %#x, want 0", got[16 + 1]);
  expect(got[16 + 3] == 5, "the second dispatch's row 1 of the root CBV reads %#x, want 5", got[16 + 3]);
  // both dispatches: the uint4 element half past its buffer reads 0 whole
  for (UINT at : {6u, 7u, 8u, 9u, 16u + 6, 16u + 7, 16u + 8, 16u + 9})
    expect(got[at] == 0, "a uint4 element with 8 of its bytes left reads %#x in word %u, want 0", got[at], at);
  // the UAV's own words are as they were: the store past its end went nowhere it can see
  for (UINT i = 0; i < words; i++)
    if (!expect(got[words + i] == 200 + i, "word %u of the root UAV's buffer is %#x, want %#x", i, got[words + i],
                200 + i))
      break;
  back->Unmap(0, nullptr);
  if (!indirect)
    return verdict();

  // ExecuteIndirect sets the root SRV and dispatches; a Dispatch after it reads through the view Direct3D reset
  step("a Dispatch after an ExecuteIndirect that set its root SRV");
  D3D12_INDIRECT_ARGUMENT_DESC set[2] = {{D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW},
                                         {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
  set[0].ShaderResourceView.RootParameterIndex = 1;
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(UINT64) + sizeof(D3D12_DISPATCH_ARGUMENTS), 2, set};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, rs.Get(), IID_PPV_ARGS(&signature)));
  UINT8 *command;
  CHECK(arguments->Map(0, nullptr, (void **)&command));
  const UINT64 view = structured->GetGPUVirtualAddress();
  const D3D12_DISPATCH_ARGUMENTS one{1, 1, 1};
  memcpy(command, &view, sizeof(view));
  memcpy(command + sizeof(view), &one, sizeof(one));
  arguments->Unmap(0, nullptr);
  CHECK(forget(back.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), pso.Get()));
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list.Get(), unordered.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, unordered->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress());
  // element 1, not 100: the view the command sets goes unchecked (its length is 0xffffffff), and it has to stay inside
  const UINT inside[3] = {20, 1, 100};
  list->SetComputeRoot32BitConstants(4, 3, inside, 0);
  list->SetComputeRootShaderResourceView(5, structured->GetGPUVirtualAddress());
  list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
  list->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress() + 128);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(back->Map(0, nullptr, (void **)&got));
  // the indirect dispatch read the view its command set; the one after reads 0 through the reset view
  expect(got[4] == 101, "the indirect dispatch's element 1 of the root SRV reads %#x, want 0x65", got[4]);
  expect(got[32 + 4] == 0, "element 1 of the root SRV after ExecuteIndirect reset it reads %#x, want 0", got[32 + 4]);
  expect(got[32 + 3] == 5, "row 1 of the root CBV after ExecuteIndirect reads %#x, want 5", got[32 + 3]);
  back->Unmap(0, nullptr);
  return verdict();
}
