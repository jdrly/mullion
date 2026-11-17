/* present_pace.exe: a Direct3D 12 present loop for MULLION_SYNC_INTERVAL's Mac check (docs/swtor/README.md § Present
 * pacing). Not part of the build or of any suite: built by hand with MinGW, run by hand under Wine with the build to
 * check, in the Mac's logged-in GUI session (the test suite's window tests can't run there: docs/swtor/gate.md § 1).
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall -o present_pace.exe present_pace.c -ld3d12 -ldxgi
 *   present_pace.exe [seconds=15] [latency=2] [interval=0]
 *
 * Each frame: wait in 2 ms slices until the fence of frame n - latency is reached, clear a 1280x720 back buffer, execute,
 * Signal(fence, n), Present(interval, 0), as fast as the interval lets it. Output:
 *   PACE start seconds=<s> latency=<n> interval=<n>
 *   PACE s <second> frames=<presents in it>      once a second
 *   PACE end frames=<n> longest_wait=<ms> longest_present=<ms>
 * Exit 0 done, 2 setup failed, 3 a fence wait over 30 s. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdint.h>

#define NBUF 3
static LARGE_INTEGER qf, q0;
static double
ms_now(void) {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return (q.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
}
static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE)
    return 0;
  return DefWindowProcW(h, m, w, l);
}
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    HRESULT hr_ = (x);                                                                                                 \
    if (FAILED(hr_)) {                                                                                                 \
      printf("PACE setup failed: %s = 0x%08lx\n", #x, (unsigned long)hr_);                                            \
      return 2;                                                                                                        \
    }                                                                                                                  \
  } while (0)

int
main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  double seconds = argc > 1 ? atof(argv[1]) : 15;
  UINT64 latency = argc > 2 ? _strtoui64(argv[2], NULL, 10) : 2;
  UINT interval = argc > 3 ? (UINT)atoi(argv[3]) : 0;
  QueryPerformanceFrequency(&qf);
  QueryPerformanceCounter(&q0);

  WNDCLASSW wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"present_pace";
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  RegisterClassW(&wc);
  RECT r = {0, 0, 1280, 720};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowW(L"present_pace", L"present_pace", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                            r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    printf("PACE setup failed: CreateWindow\n");
    return 2;
  }

  IDXGIFactory4 *factory;
  ID3D12Device *dev;
  ID3D12CommandQueue *q;
  IDXGISwapChain1 *sc1;
  IDXGISwapChain3 *sc;
  CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
  CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
  DXGI_SWAP_CHAIN_DESC1 sd = {0};
  sd.Width = 1280;
  sd.Height = 720;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = NBUF;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1));
  CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NBUF, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ID3D12DescriptorHeap *heap;
  CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
  UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE base = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
  ID3D12Resource *bufs[NBUF];
  ID3D12CommandAllocator *alloc[NBUF];
  ID3D12GraphicsCommandList *list;
  for (UINT i = 0; i < NBUF; i++) {
    CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i]));
    D3D12_CPU_DESCRIPTOR_HANDLE h = {base.ptr + i * inc};
    ID3D12Device_CreateRenderTargetView(dev, bufs[i], NULL, h);
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                              (void **)&alloc[i]));
  }
  CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], NULL,
                                       &IID_ID3D12GraphicsCommandList, (void **)&list));
  ID3D12GraphicsCommandList_Close(list);
  ID3D12Fence *fence;
  CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
  HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
  printf("PACE start seconds=%.0f latency=%llu interval=%u\n", seconds, (unsigned long long)latency, interval);

  UINT64 n = 0;
  long total = 0, second_frames = 0, second = 0;
  double longest_wait = 0, longest_present = 0, next_second = 1000;
  while (ms_now() < seconds * 1000) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (n >= latency && ID3D12Fence_GetCompletedValue(fence) < n - latency + 1) {
      double a = ms_now();
      ID3D12Fence_SetEventOnCompletion(fence, n - latency + 1, event);
      while (WaitForSingleObject(event, 2) == WAIT_TIMEOUT)
        if (ms_now() - a > 30000) {
          printf("PACE hung: fence %llu waited 30 s\n", (unsigned long long)(n - latency + 1));
          return 3;
        }
      double w = ms_now() - a;
      if (w > longest_wait)
        longest_wait = w;
    }
    UINT bi = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
    ID3D12CommandAllocator_Reset(alloc[bi]);
    ID3D12GraphicsCommandList_Reset(list, alloc[bi], NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bufs[bi];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    float c = (float)((total % 120) / 120.0);
    float color[4] = {c, 0.3f, 1.0f - c, 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE h = {base.ptr + bi * inc};
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, color, 0, NULL);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(q, fence, ++n);
    double a = ms_now();
    IDXGISwapChain3_Present(sc, interval, 0);
    double e = ms_now() - a;
    if (e > longest_present)
      longest_present = e;
    total++, second_frames++;
    if (ms_now() >= next_second) {
      printf("PACE s %ld frames=%ld\n", ++second, second_frames);
      second_frames = 0, next_second += 1000;
    }
  }
  printf("PACE end frames=%ld longest_wait=%.0f longest_present=%.0f\n", total, longest_wait, longest_present);
  return 0;
}
