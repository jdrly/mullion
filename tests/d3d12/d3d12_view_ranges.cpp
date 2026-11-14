// contract (jdrly/mullion, MULLION_D3D12_CHECK_RANGES=1, private ticket 20 part 2): an index or vertex buffer view
// that runs past the buffer its address lies in, or whose address lies in no live buffer, is bound as no view. A draw
// then reads zero through it, as through no view: every index is 0, and every vertex has no position, so nothing is
// drawn ("0 GPUVA or NULL pDesc means nothing bound", DirectX-Specs, Resource Binding; D3D11.3 8.19.2). A view inside
// its buffer draws as before.
// The model is d3d12_fetch: vertex i is a point on pixel i of a 15 by 15 target with the value i + 1, drawn through
// indices last to first. Each view runs at most 24 bytes past a buffer of its own memory. Its control,
// tests/breaks/d3d12_view_ranges.patch, binds the views as given: under Metal's validation (tests/run.py --validate)
// Metal reports the index buffers, without it the draws read the bytes behind the buffers, and the test fails either
// way. Run it on the rented M1 only (private ticket 15)
#include "d3d12_test.hpp"
#include <tuple>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
V vs(float4 pos : POSITION, uint value : VALUE) { V v; v.pos = pos; v.value = value; return v; }
uint ps(V v) : SV_Target { return v.value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  struct Vertex {
    float x, y;
    UINT value;
  };
  const UINT side = 15, count = 32, clear = 0xc1ea;
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const D3D12_INPUT_ELEMENT_DESC elements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x)},
                                               {"VALUE", 0, DXGI_FORMAT_R32_UINT, 0, offsetof(Vertex, value)}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.InputLayout = {elements, (UINT)std::size(elements)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the vertices and their indices, last to first, each in an upload buffer of its own. A view's buffer ends where
  // its memory does, and Mullion gives a buffer at least 256 bytes (BufferAllocation): the vertices' 384 bytes are
  // exactly theirs, the indices' 128 lie in a buffer of 256, all of it theirs
  const UINT vertex_bytes = count * sizeof(Vertex), index_bytes = count * sizeof(UINT), index_room = 256;
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertex_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, index_room, D3D12_RESOURCE_STATE_GENERIC_READ);
  // a buffer released before the draws: its address is no live buffer's
  auto released = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, index_room, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(readback && vertices && indices && released, "buffers"))
    return verdict();
  const D3D12_GPU_VIRTUAL_ADDRESS gone = released->GetGPUVirtualAddress();
  released.Reset();
  Vertex *v;
  CHECK(vertices->Map(0, nullptr, (void **)&v));
  for (UINT i = 0; i < count; i++)
    v[i] = {(i % side + 0.5f) * 2 / side - 1, 1 - (i / side + 0.5f) * 2 / side, i + 1};
  vertices->Unmap(0, nullptr);
  UINT *index;
  CHECK(indices->Map(0, nullptr, (void **)&index));
  for (UINT i = 0; i < index_room / sizeof(UINT); i++)
    index[i] = i < count ? count - 1 - i : 0;
  indices->Unmap(0, nullptr);

  enum class Drawn { All, First, None };
  auto draw = [&](const char *what, D3D12_VERTEX_BUFFER_VIEW vbv, D3D12_INDEX_BUFFER_VIEW ibv, Drawn drawn) {
    step("%s", what);
    // every vertex; vertex 0 alone, as every index read 0; or nothing
    std::vector<UINT> want(side * side, clear);
    for (UINT i = 0; i < count; i++)
      if (drawn == Drawn::All || (drawn == Drawn::First && !i))
        want[i] = i + 1;
    if (FAILED(forget(readback.Get())) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pso.Get())))
      return (void)expect(false, "the list could not be begun");
    const float cleared[4] = {(float)clear};
    D3D12_VIEWPORT viewport{0, 0, (float)side, (float)side, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)side, (LONG)side};
    list->ClearRenderTargetView(rtv, cleared, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->IASetVertexBuffers(0, 1, &vbv);
    list->IASetIndexBuffer(&ibv);
    list->DrawIndexedInstanced(count, 1, 0, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
    const char *out;
    if (!expect(ran == S_OK, "%s: the draw's list: %08lx", what, ran) || FAILED(readback->Map(0, nullptr, (void **)&out)))
      return;
    unsigned wrong = 0;
    for (UINT pixel = 0; pixel < side * side; pixel++) {
      UINT got = ((const UINT *)(out + pixel / side * footprint.Footprint.RowPitch))[pixel % side];
      if (got != want[pixel] && wrong++ < 4)
        expect(false, "%s: pixel %u,%u holds %#x, want %#x", what, pixel % side, pixel / side, got, want[pixel]);
    }
    expect(wrong <= 4, "%s: and %u more pixels", what, wrong - 4);
    readback->Unmap(0, nullptr);
  };

  const UINT stride = sizeof(Vertex);
  const D3D12_VERTEX_BUFFER_VIEW whole{vertices->GetGPUVirtualAddress(), vertex_bytes, stride},
      past_vertices{vertices->GetGPUVirtualAddress(), vertex_bytes + 2 * stride, stride};
  const D3D12_INDEX_BUFFER_VIEW all{indices->GetGPUVirtualAddress(), index_bytes, DXGI_FORMAT_R32_UINT},
      past_indices{indices->GetGPUVirtualAddress(), index_room + 8, DXGI_FORMAT_R32_UINT},
      released_indices{gone, index_bytes, DXGI_FORMAT_R32_UINT};
  draw("both views inside their buffers: every vertex", whole, all, Drawn::All);
  draw("an index view 8 bytes past its buffer: no view, every index 0", whole, past_indices, Drawn::First);
  draw("an index view at a released buffer's address: no view, every index 0", whole, released_indices, Drawn::First);
  draw("a vertex view two vertices past its buffer: no view, nothing drawn", past_vertices, all, Drawn::None);
  // a view checked once is checked again when bound again, and an in-range one after it still draws
  draw("both views inside their buffers again", whole, all, Drawn::All);
  return verdict();
}
