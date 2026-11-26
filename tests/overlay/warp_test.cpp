// D3D12 overlay test (T7.1). No game, no GPU: a WARP device, a direct queue and
// a flip swap chain on a hidden window. The overlay draws a red rect from its
// draw list; a present callback copies the back buffer to a readback buffer
// so the test can check pixels.
//
//   shvi_overlay_warp early   overlay installed before the swap chain exists (creation hooks)
//   shvi_overlay_warp late    swap chain created first (ExecuteCommandLists fallback)
//   shvi_overlay_warp fp16    R16G16B16A16_FLOAT swap chain (scRGB composite)
//   shvi_overlay_warp fault   a second present callback faults; it is dropped, drawing goes on
#include "core/overlay_d3d12.h"
#include "core/overlay_drawlist.h"
#include "shvi/shvi.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

namespace ov = shvi::overlay;

int g_failed = 0;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)
#define REQUIRE_HR(x)                                                                        \
    do {                                                                                     \
        HRESULT hr_ = (x);                                                                   \
        if (FAILED(hr_)) { std::printf("  FATAL %s:%d: %s = 0x%08lX\n", __FILE__, __LINE__, #x, \
                                       static_cast<unsigned long>(hr_)); return 1; }          \
    } while (0)

template <class T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct Gpu {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapChain = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT rtvSize = 0;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    HWND hwnd = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // readback, filled by the present callback
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackSize = 0;
};
Gpu g;

void WaitIdle() {
    g.queue->Signal(g.fence, ++g.fenceValue);
    if (g.fence->GetCompletedValue() < g.fenceValue) {
        g.fence->SetEventOnCompletion(g.fenceValue, g.event);
        WaitForSingleObject(g.event, INFINITE);
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

// Present callback: copy the back buffer (already drawn by the overlay) to the readback buffer.
void ReadbackCallback(const ov::PresentContext& ctx, void*) {
    ID3D12Resource* bb = nullptr;
    if (FAILED(ctx.swapChain->GetBuffer(ctx.backBufferIndex, IID_PPV_ARGS(&bb)))) return;
    D3D12_RESOURCE_DESC desc = bb->GetDesc();
    UINT64 total = 0;
    ctx.device->GetCopyableFootprints(&desc, 0, 1, 0, &g.footprint, nullptr, nullptr, &total);
    if (!g.readback || g.readbackSize < total) {
        SafeRelease(g.readback);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = total;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&g.readback)))) {
            bb->Release();
            return;
        }
        g.readbackSize = total;
    }
    auto b = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{g.readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = g.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ctx.list->ResourceBarrier(1, &b);
    bb->Release();
}

void FaultingCallback(const ov::PresentContext&, void*) { *static_cast<volatile int*>(nullptr) = 1; }

float HalfToFloat(std::uint16_t h) {
    std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    return s ? -v : v;
}

struct Pixel { float r, g, b, a; };  // 0..255 for UNORM, raw values for FP16

Pixel ReadPixel(UINT x, UINT y) {
    Pixel p{-1, -1, -1, -1};
    if (!g.readback) return p;
    void* mem = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(g.readbackSize)};
    if (FAILED(g.readback->Map(0, &range, &mem))) return p;
    const auto* row = static_cast<const std::uint8_t*>(mem) + g.footprint.Offset + y * g.footprint.Footprint.RowPitch;
    if (g.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t h[4];
        std::memcpy(h, row + x * 8, 8);
        p = {HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]), HalfToFloat(h[3])};
    } else {
        const std::uint8_t* c = row + x * 4;
        p = {float(c[0]), float(c[1]), float(c[2]), float(c[3])};
    }
    D3D12_RANGE none{0, 0};
    g.readback->Unmap(0, &none);
    return p;
}

bool IsColor(const Pixel& p, float r, float gg, float b, float a) {
    return std::fabs(p.r - r) < 1.5f && std::fabs(p.g - gg) < 1.5f && std::fabs(p.b - b) < 1.5f &&
           std::fabs(p.a - a) < 1.5f;
}

void PrintPixel(const char* what, const Pixel& p) {
    std::printf("  %s = (%.3f, %.3f, %.3f, %.3f)\n", what, p.r, p.g, p.b, p.a);
}

int MakeSwapChain(HWND hwnd, UINT w, UINT h, IDXGISwapChain3** out) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = g.format;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    REQUIRE_HR(g.factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &sc1));
    HRESULT hr = sc1->QueryInterface(IID_PPV_ARGS(out));
    sc1->Release();
    REQUIRE_HR(hr);
    return 0;
}

void ClosingCallback(const ov::PresentContext& ctx, void*) { ctx.list->Close(); }  // breaks the rule: never close

int CreateGpu() {
    REQUIRE_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory)));
    IDXGIAdapter* warp = nullptr;
    REQUIRE_HR(g.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    HRESULT hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device));
    warp->Release();
    REQUIRE_HR(hr);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    REQUIRE_HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)));
    REQUIRE_HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)));
    REQUIRE_HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list)));
    g.list->Close();
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    REQUIRE_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)));
    g.rtvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    REQUIRE_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"shvi_overlay_warp";
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"shvi overlay test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) { std::printf("  FATAL CreateWindowExW %lu\n", GetLastError()); return 1; }

    return MakeSwapChain(g.hwnd, 640, 360, &g.swapChain);
}

// A second window and swap chain on the same queue (mode recreate).
int SecondSwapChain(IDXGISwapChain3** out) {
    HWND w = CreateWindowExW(0, L"shvi_overlay_warp", L"second", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) return 1;
    return MakeSwapChain(w, 320, 200, out);
}

// The tick's part: re-publish the menu rect, as core_main's OnTick does every tick.
ov::DrawList* g_list = nullptr;
bool g_ticking = true;
void Tick() {
    if (!g_ticking || !g_list) return;
    g_list->AddRect(0.25f, 0.25f, 0.5f, 0.5f, 0xFF0000FFu);
    g_list->Publish();
}

// The test's own frame: clear the back buffer to blue, then Present (the overlay draws on top).
int Frame() {
    Tick();
    UINT i = g.swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    REQUIRE_HR(g.swapChain->GetBuffer(i, IID_PPV_ARGS(&bb)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(i) * g.rtvSize;
    g.device->CreateRenderTargetView(bb, nullptr, rtv);
    REQUIRE_HR(g.alloc->Reset());
    REQUIRE_HR(g.list->Reset(g.alloc, nullptr));
    auto b = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.list->ResourceBarrier(1, &b);
    const float blue[4] = {0, 0, 1, 1};
    g.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
    b = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &b);
    REQUIRE_HR(g.list->Close());
    ID3D12CommandList* lists[] = {g.list};
    g.queue->ExecuteCommandLists(1, lists);
    bb->Release();
    REQUIRE_HR(g.swapChain->Present(0, 0));
    WaitIdle();
    return 0;
}

int Frames(int n) {
    for (int k = 0; k < n; ++k)
        if (int rc = Frame()) return rc;
    return 0;
}

// After a fault or a refused list the overlay is off; the game must keep presenting and resizing.
void CheckGameKeepsGoing() {
    CHECK(Frames(5) == 0);
    HRESULT hr = g.swapChain->ResizeBuffers(0, 320, 200, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) std::printf("  ResizeBuffers after the fault = 0x%08lX\n", static_cast<unsigned long>(hr));
    CHECK(hr == S_OK);
    CHECK(Frames(5) == 0);
}

// The review-fix modes. Runs after the first 10 good frames; returns -1 when the mode is done
// (success decided by CHECKs), 0 to continue with the common checks, >0 on a fatal error.
int ExtraMode(const std::string& mode) {
    if (mode == "drawfault") {  // a fault inside the overlay's own step, not in a callback
        ov::testing::FaultNextFrame();
        if (int rc = Frames(1)) return rc;
        CHECK(ov::GetStatus().disabled);
        CheckGameKeepsGoing();
        return -1;
    }
    if (mode == "closefault") {  // a callback closes the open list: the overlay must not execute it
        CHECK(ov::RegisterPresent(&ClosingCallback, nullptr) == SHVI_OK);
        if (int rc = Frames(1)) return rc;
        CHECK(ov::GetStatus().disabled);
        CheckGameKeepsGoing();
        return -1;
    }
    if (mode == "disable") {  // the core went inert: no drawing, no GPU work, until enabled again
        ov::SetEnabled(false);
        std::uint64_t before = ov::GetStatus().frames;
        if (int rc = Frames(5)) return rc;
        CHECK(ov::GetStatus().frames == before);
        // The readback callback doesn't run while disabled either, so clear the evidence and re-enable.
        ov::SetEnabled(true);
        if (int rc = Frames(3)) return rc;
        CHECK(IsColor(ReadPixel(320, 180), 255, 0, 0, 255));
        CHECK(ov::GetStatus().frames == before + 3);
        return -1;
    }
    if (mode == "stale") {  // ticks stop: the last frame must not stay on screen
        g_ticking = false;
        Sleep(static_cast<DWORD>(ov::kStaleFrameMs) + 200);
        if (int rc = Frames(2)) return rc;
        Pixel c = ReadPixel(320, 180);
        if (!IsColor(c, 0, 0, 255, 255)) PrintPixel("centre with ticks stopped", c);
        CHECK(IsColor(c, 0, 0, 255, 255));
        g_ticking = true;
        if (int rc = Frames(2)) return rc;
        CHECK(IsColor(ReadPixel(320, 180), 255, 0, 0, 255));
        return -1;
    }
    if (mode == "recreate") {  // the game replaces its swap chain (new address); the old one stops presenting
        IDXGISwapChain3* second = nullptr;
        if (int rc = SecondSwapChain(&second)) return rc;
        IDXGISwapChain3* first = g.swapChain;
        g.swapChain = second;
        if (int rc = Frames(70)) return rc;
        ov::Status st = ov::GetStatus();
        CHECK(st.attached && !st.disabled && st.width == 320 && st.height == 200);
        Pixel c = ReadPixel(160, 100);
        if (!IsColor(c, 255, 0, 0, 255)) PrintPixel("centre on the new swap chain", c);
        CHECK(IsColor(c, 255, 0, 0, 255));
        first->Release();
        return -1;
    }
    return 0;
}

int Run(const std::string& mode) {
    ov::DrawList list;
    const bool late = mode == "late";
    if (mode == "fp16") g.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ov::Slots slots = ov::VtableSlots();
    CHECK(slots.scPresent == 8);
    CHECK(slots.scResizeBuffers == 13);

    if (late) {
        if (int rc = CreateGpu()) return rc;
    }
    if (!ov::Install(&list)) { std::printf("  FATAL Install failed\n"); return 1; }
    if (!late) {
        if (int rc = CreateGpu()) return rc;
    }

    g_list = &list;
    if (mode == "fault") CHECK(ov::RegisterPresent(&FaultingCallback, nullptr) == SHVI_OK);
    CHECK(ov::RegisterPresent(&ReadbackCallback, nullptr) == SHVI_OK);

    if (int rc = Frames(10)) return rc;
    if (int rc = ExtraMode(mode)) return rc < 0 ? 0 : rc;
    ov::Status st = ov::GetStatus();
    CHECK(st.attached);
    CHECK(!st.disabled);
    CHECK(st.source == (late ? ov::QueueSource::ExecuteFallback : ov::QueueSource::Creation));
    CHECK(st.width == 640 && st.height == 360 && st.format == static_cast<std::uint32_t>(g.format));
    CHECK(st.frames >= 10);

    Pixel centre = ReadPixel(320, 180), corner = ReadPixel(5, 5);
    if (g.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        PrintPixel("fp16 centre", centre);
        CHECK(std::fabs(centre.r - 2.5f) < 0.05f && std::fabs(centre.g) < 0.05f && std::fabs(centre.b) < 0.05f);
        CHECK(std::fabs(corner.r) < 0.05f && std::fabs(corner.b - 1.0f) < 0.05f);
    } else {
        if (!IsColor(centre, 255, 0, 0, 255)) PrintPixel("centre", centre);
        if (!IsColor(corner, 0, 0, 255, 255)) PrintPixel("corner", corner);
        CHECK(IsColor(centre, 255, 0, 0, 255));
        CHECK(IsColor(corner, 0, 0, 255, 255));
    }

    if (mode == "fault") {
        CHECK(ov::UnregisterPresent(&FaultingCallback, nullptr) == SHVI_E_INVALID_ARG);  // already dropped
        return 0;
    }

    // Keep count and format, as games do. Fails if anyone still holds a back buffer reference.
    HRESULT hr = g.swapChain->ResizeBuffers(0, 320, 200, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) std::printf("  ResizeBuffers = 0x%08lX\n", static_cast<unsigned long>(hr));
    CHECK(hr == S_OK);
    if (SUCCEEDED(hr)) {
        if (int rc = Frames(10)) return rc;
        st = ov::GetStatus();
        CHECK(st.width == 320 && st.height == 200 && !st.disabled);
        centre = ReadPixel(160, 100);
        if (g.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            CHECK(std::fabs(centre.r - 2.5f) < 0.05f);
        } else {
            if (!IsColor(centre, 255, 0, 0, 255)) PrintPixel("centre after resize", centre);
            CHECK(IsColor(centre, 255, 0, 0, 255));
        }
    }
    CHECK(ov::UnregisterPresent(&ReadbackCallback, nullptr) == SHVI_OK);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "early";
    static const char* modes[] = {"early", "late", "fp16", "fault", "drawfault", "closefault", "disable", "stale",
                                  "recreate"};
    if (std::find(std::begin(modes), std::end(modes), mode) == std::end(modes)) {
        std::printf("usage: shvi_overlay_warp early|late|fp16|fault|drawfault|closefault|disable|stale|recreate\n");
        return 2;
    }
    int rc = Run(mode);
    std::printf("%s: %s (%d failed checks)\n", mode.c_str(), rc || g_failed ? "FAIL" : "PASS", g_failed);
    return rc || g_failed ? 1 : 0;
}
