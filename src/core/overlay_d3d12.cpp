// GENERIC. D3D12 overlay. See overlay_d3d12.h for the contract.
#include "overlay_d3d12.h"

#include "log.h"
#include "shvi/shvi.h"

#include <MinHook.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <windows.h>

#include <cfloat>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shvi::overlay {
namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ------------------------------------------------------------------ hooked signatures
using PresentPfn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Pfn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersPfn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Pfn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                      const UINT*, IUnknown* const*);
using CreateSwapChainPfn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                       IDXGISwapChain**);
using CreateForHwndPfn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                     IDXGISwapChain1**);
using CreateForCoreWindowPfn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                           const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                           IDXGISwapChain1**);
using CreateForCompositionPfn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                            IDXGIOutput*, IDXGISwapChain1**);
using ExecutePfn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// ------------------------------------------------------------------ state
struct BufferState {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fenceValue = 0;
};

constexpr UINT kSrvDescriptors = 64;

// The one swap chain the overlay draws on, and everything it built for it.
struct Chain {
    IDXGISwapChain* sc = nullptr;  // identity only: no reference held
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    QueueSource source = QueueSource::None;

    // per-buffer state, rebuilt after a resize
    bool built = false;
    UINT width = 0, height = 0, bufferCount = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<BufferState> buffers;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT rtvSize = 0;
    ID3D12GraphicsCommandList* list = nullptr;

    // lives as long as the device
    ID3D12Fence* fence = nullptr;
    UINT64 fenceLast = 0;
    HANDLE event = nullptr;

    // Dear ImGui
    ImGuiContext* imgui = nullptr;
    ImFont* fonts[kFontCount] = {};  // regular, bold, display; null falls back to the regular face
    ID3D12DescriptorHeap* srvHeap = nullptr;
    UINT srvSize = 0;
    bool srvUsed[kSrvDescriptors] = {};
    bool backendInit = false;
    DXGI_FORMAT backendFormat = DXGI_FORMAT_UNKNOWN;
    UINT backendFrames = 0;
    DXGI_FORMAT formatLogged = DXGI_FORMAT_UNKNOWN;

    // FP16 (scRGB) swap chains: ImGui draws into an 8-bit offscreen target, a composite
    // pass blends it over the back buffer in linear light. The target is per-size state.
    ID3D12Resource* offscreen = nullptr;
    ID3D12DescriptorHeap* offscreenRtvHeap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE offscreenSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE offscreenSrvGpu{};
    bool offscreenSrv = false;
    ID3D12RootSignature* compositeRoot = nullptr;  // built once
    ID3D12PipelineState* compositePso = nullptr;
};

struct Global {
    std::recursive_mutex mu;  // present / resize / status / callbacks
    DrawList* list = nullptr;

    PresentPfn origPresent = nullptr;
    Present1Pfn origPresent1 = nullptr;
    ResizeBuffersPfn origResizeBuffers = nullptr;
    ResizeBuffers1Pfn origResizeBuffers1 = nullptr;
    CreateSwapChainPfn origCreateSwapChain = nullptr;
    CreateForHwndPfn origCreateForHwnd = nullptr;
    CreateForCoreWindowPfn origCreateForCoreWindow = nullptr;
    CreateForCompositionPfn origCreateForComposition = nullptr;
    ExecutePfn origExecute = nullptr;

    std::mutex mapMu;  // swap chain -> queue, from the creation hooks (AddRef'd queues)
    std::unordered_map<void*, ID3D12CommandQueue*> queues;

    bool haveChain = false;
    Chain chain;
    bool loggedOther = false;
    int unmatchedPresents = 0;  // late attach: presents with no queue found
    int othersSinceOurs = 0;    // presents of other swap chains since ours last presented
    bool fallbackGaveUp = false;
    bool disabled = false;
    std::uint64_t frames = 0;
    std::vector<std::pair<PresentFn, void*>> callbacks;
    bool testPattern = false;
    bool enabled = true;          // SetEnabled: false = pass straight through
    bool faultNextFrame = false;  // testing::FaultNextFrame
    // Stale-frame cut-off: the generation the present thread last saw and when it changed.
    std::uint64_t seenGeneration = 0;
    ULONGLONG generationSeenAt = 0;
};
Global* G = nullptr;
thread_local int t_depth = 0;  // >0 inside a hooked DXGI call: nested hooked calls pass straight through
// Late attach: the last DIRECT queue that executed on this thread since its last present
// (no reference: it is used, and cleared, by the present that follows on the same thread).
thread_local ID3D12CommandQueue* t_lastDirect = nullptr;
constexpr int kFallbackPresents = 120;
constexpr int kTakeoverPresents = 60;  // ~1 s at 60 fps with the attached swap chain silent

// ------------------------------------------------------------------ SEH
using StepFn = void (*)(void*);

int CaptureFault(EXCEPTION_POINTERS* ep, DWORD* code, void** addr) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *addr = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects here: __try cannot unwind them. Callers hold their locks outside.
bool SehRun(StepFn fn, void* arg, DWORD* code, void** addr) {
    __try {
        fn(arg);
        return true;
    } __except (CaptureFault(GetExceptionInformation(), code, addr)) {
        return false;
    }
}

std::string ModuleRva(const void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &m) || !m) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", addr);
        return buf;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = std::strrchr(path, '\\');
    base = base ? base + 1 : path;
    char buf[MAX_PATH + 32];
    std::snprintf(buf, sizeof(buf), "%s+0x%llX", base,
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr) -
                                                  reinterpret_cast<std::uintptr_t>(m)));
    return buf;
}

// ------------------------------------------------------------------ helpers
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void WaitFence(Chain& c, UINT64 value) {
    if (!c.fence || c.fence->GetCompletedValue() >= value) return;
    c.fence->SetEventOnCompletion(value, c.event);
    WaitForSingleObject(c.event, INFINITE);
}

void WaitAll(Chain& c) { WaitFence(c, c.fenceLast); }

// Per-buffer state only (and the FP16 offscreen target); ImGui, the fence, the
// composite pipeline and the device stay.
void ReleaseBuffers(Chain& c) {
    for (auto& b : c.buffers) SafeRelease(b.alloc);
    c.buffers.clear();
    SafeRelease(c.rtvHeap);
    SafeRelease(c.list);
    SafeRelease(c.offscreen);
    SafeRelease(c.offscreenRtvHeap);
    if (c.offscreenSrv) {
        SIZE_T i = (c.offscreenSrvCpu.ptr - c.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / c.srvSize;
        if (i < kSrvDescriptors) c.srvUsed[i] = false;
        c.offscreenSrv = false;
    }
    c.built = false;
}

void SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto* c = static_cast<Chain*>(info->UserData);
    for (UINT i = 0; i < kSrvDescriptors; ++i) {
        if (c->srvUsed[i]) continue;
        c->srvUsed[i] = true;
        *cpu = c->srvHeap->GetCPUDescriptorHandleForHeapStart();
        *gpu = c->srvHeap->GetGPUDescriptorHandleForHeapStart();
        cpu->ptr += SIZE_T(i) * c->srvSize;
        gpu->ptr += UINT64(i) * c->srvSize;
        return;
    }
    // Out of descriptors: hand out slot 0 again (wrong texture, never a crash).
    *cpu = c->srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = c->srvHeap->GetGPUDescriptorHandleForHeapStart();
}

void SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto* c = static_cast<Chain*>(info->UserData);
    SIZE_T i = (cpu.ptr - c->srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / c->srvSize;
    if (i < kSrvDescriptors) c->srvUsed[i] = false;
}

// How the overlay draws on a format. Rtv: ImGui straight into the back buffer.
// Composite: ImGui into an 8-bit offscreen target, blended over the back buffer in
// linear light (scRGB: sRGB -> linear, x kScRgbWhite).
enum class FormatPath { Rtv, Composite, Skip };

// 200 nits paper white / 80 nits (scRGB 1.0).
constexpr float kScRgbWhite = 2.5f;

FormatPath PathFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:  // SDR values written unchanged (dim under HDR10), logged once
            return FormatPath::Rtv;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return FormatPath::Composite;
        default:
            return FormatPath::Skip;
    }
}

// Fullscreen triangle; the offscreen target holds premultiplied sRGB-encoded colour
// (ImGui's alpha blend onto transparent black). Un-premultiply, decode, scale, re-premultiply.
constexpr char kCompositeHlsl[] = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 ToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float4 ps(V i) : SV_Target {
    float4 c = t0.Sample(s0, i.uv);
    if (c.a <= 0.0) return float4(0, 0, 0, 0);
    float3 lin = ToLinear(saturate(c.rgb / c.a)) * WHITE;
    return float4(lin * c.a, c.a);
}
)";

bool BuildCompositePipeline(Chain& c) {
    if (c.compositePso) return true;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{1, &param, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    SafeRelease(errors);
    if (FAILED(hr)) return false;
    hr = c.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                       IID_PPV_ARGS(&c.compositeRoot));
    SafeRelease(blob);
    if (FAILED(hr)) return false;

    char white[32];
    std::snprintf(white, sizeof(white), "%.4f", kScRgbWhite);
    const D3D_SHADER_MACRO defines[] = {{"WHITE", white}, {nullptr, nullptr}};
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kCompositeHlsl, sizeof(kCompositeHlsl) - 1, nullptr, defines, nullptr, "vs",
                                   "vs_5_0", 0, 0, &vs, nullptr)) &&
              SUCCEEDED(D3DCompile(kCompositeHlsl, sizeof(kCompositeHlsl) - 1, nullptr, defines, nullptr, "ps",
                                   "ps_5_0", 0, 0, &ps, &errors));
    if (errors) {
        SHVI_LOG("overlay: composite shader: %s", static_cast<const char*>(errors->GetBufferPointer()));
        SafeRelease(errors);
    }
    if (ok) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = c.compositeRoot;
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        auto& bl = pd.BlendState.RenderTarget[0];
        bl.BlendEnable = TRUE;
        bl.SrcBlend = D3D12_BLEND_ONE;
        bl.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = D3D12_BLEND_ONE;
        bl.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.compositePso)));
    }
    SafeRelease(vs);
    SafeRelease(ps);
    return ok;
}

bool BuildOffscreen(Chain& c) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = c.width;
    rd.Height = c.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}};
    if (FAILED(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                                                 IID_PPV_ARGS(&c.offscreen))))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.offscreenRtvHeap)))) return false;
    c.device->CreateRenderTargetView(c.offscreen, nullptr, c.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart());
    ImGui_ImplDX12_InitInfo info;
    info.UserData = &c;
    SrvAlloc(&info, &c.offscreenSrvCpu, &c.offscreenSrvGpu);
    c.offscreenSrv = true;
    c.device->CreateShaderResourceView(c.offscreen, nullptr, c.offscreenSrvCpu);
    return true;
}

// Builds the per-buffer state (and, the first time, ImGui) for desc d.
// Menu faces from the system font folder: Segoe UI, Segoe UI Bold, Bahnschrift (a narrow display face for
// titles). A missing file falls back to the built-in vector font, so text always draws. The SDK's
// ui::TextWidth uses these faces' advance widths to fit labels into a panel.
void LoadFonts(Chain& c, ImGuiIO& io) {
    char dir[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    std::string fonts = (n > 0 && n < MAX_PATH) ? std::string(dir) + "\\Fonts\\" : std::string("C:\\Windows\\Fonts\\");
    static const char* const kFiles[kFontCount] = {"segoeui.ttf", "segoeuib.ttf", "bahnschrift.ttf"};
    for (int i = 0; i < kFontCount; ++i) {
        std::string path = fonts + kFiles[i];
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        c.fonts[i] = io.Fonts->AddFontFromFileTTF(path.c_str(), 0.0f, &cfg);
    }
    if (!c.fonts[kFontRegular]) c.fonts[kFontRegular] = io.Fonts->AddFontDefaultVector();
    SHVI_LOG("overlay: fonts %s / %s / %s", c.fonts[0] ? "regular" : "-", c.fonts[1] ? "bold" : "-", c.fonts[2] ? "display" : "-");
}

bool Build(Chain& c, const DXGI_SWAP_CHAIN_DESC1& d) {
    c.width = d.Width;
    c.height = d.Height;
    c.format = d.Format;
    c.bufferCount = d.BufferCount;
    if (!c.fence) {
        if (FAILED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return false;
        c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, d.BufferCount};
    if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.rtvHeap)))) return false;
    c.rtvSize = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    c.buffers.resize(d.BufferCount);
    for (auto& b : c.buffers) {
        if (FAILED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.alloc))))
            return false;
        b.fenceValue = 0;
    }
    if (FAILED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.buffers[0].alloc, nullptr,
                                           IID_PPV_ARGS(&c.list))))
        return false;
    c.list->Close();

    if (!c.imgui) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        c.imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(c.imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        LoadFonts(c, io);
        ImGui::SetCurrentContext(prev);
    }
    if (!c.srvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvDescriptors,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (FAILED(c.device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&c.srvHeap)))) return false;
        c.srvSize = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    const bool composite = PathFor(d.Format) == FormatPath::Composite;
    if (composite && (!BuildCompositePipeline(c) || !BuildOffscreen(c))) return false;
    DXGI_FORMAT rtvFormat = composite ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format;
    ImGui::SetCurrentContext(c.imgui);
    if (c.backendInit && (c.backendFormat != rtvFormat || c.backendFrames != d.BufferCount)) {
        ImGui_ImplDX12_Shutdown();  // the GPU is idle: WaitAll ran before the rebuild
        c.backendInit = false;
    }
    if (!c.backendInit) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = c.device;
        info.CommandQueue = c.queue;
        info.NumFramesInFlight = static_cast<int>(d.BufferCount);
        info.RTVFormat = rtvFormat;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.UserData = &c;
        info.SrvDescriptorHeap = c.srvHeap;
        info.SrvDescriptorAllocFn = &SrvAlloc;
        info.SrvDescriptorFreeFn = &SrvFree;
        if (!ImGui_ImplDX12_Init(&info)) return false;
        c.backendInit = true;
        c.backendFormat = rtvFormat;
        c.backendFrames = d.BufferCount;
    }
    c.built = true;
    return true;
}

ImU32 Col(std::uint32_t rgba) {
    return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
}

void DrawItems(const Chain& c, ImDrawList* dl, const Frame& frame, float W, float H) {
    ImFont* fallback = c.fonts[kFontRegular] ? c.fonts[kFontRegular] : ImGui::GetFont();
    for (const DrawItem& it : frame) {
        ImFont* font = it.font >= 0 && it.font < kFontCount && c.fonts[it.font] ? c.fonts[it.font] : fallback;
        ImU32 col = Col(it.rgba);
        if (it.kind == DrawItem::Kind::Rect) {
            dl->AddRectFilled(ImVec2(it.x * W, it.y * H), ImVec2((it.x + it.w) * W, (it.y + it.h) * H), col);
            continue;
        }
        float size = it.scale * H * kTextScale;
        if (!(size >= 1.0f) || it.text.empty()) continue;
        const char* b = it.text.data();
        const char* e = b + it.text.size();
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e);
        float px = it.x * W;
        if (it.align == 1) px -= ts.x * 0.5f;
        else if (it.align == 2) px -= ts.x;
        dl->AddText(font, size, ImVec2(px, it.y * H), col, b, e);
    }
}

// ------------------------------------------------------------------ the present step
// Everything the step acquires lives here, outside the SEH frame: after a fault OnPresent
// still releases the back buffer (a leaked reference would fail the host's ResizeBuffers).
struct FrameArgs {
    IDXGISwapChain* sc = nullptr;
    IDXGISwapChain3* sc3 = nullptr;
    ID3D12Resource* bb = nullptr;
    std::shared_ptr<const Frame> frame;
};

struct CallbackArgs {
    PresentFn fn;
    void* user;
    const PresentContext* ctx;
};

void RunCallback(void* p) {
    auto* a = static_cast<CallbackArgs*>(p);
    a->fn(*a->ctx, a->user);
}

// Each callback under its own SEH guard: a fault unregisters that callback only.
void RunCallbacks(const PresentContext& ctx) {
    auto callbacks = G->callbacks;  // a callback may (un)register
    for (auto& [fn, user] : callbacks) {
        CallbackArgs a{fn, user, &ctx};
        DWORD code = 0;
        void* addr = nullptr;
        if (SehRun(&RunCallback, &a, &code, &addr)) continue;
        SHVI_LOG("overlay: present callback %s faulted (0x%08lX at %s); unregistered",
                 ModuleRva(reinterpret_cast<const void*>(fn)).c_str(), static_cast<unsigned long>(code),
                 ModuleRva(addr).c_str());
        UnregisterPresent(fn, user);
    }
}

void DrawFrame(void* p) {
    auto* a = static_cast<FrameArgs*>(p);
    Chain& c = G->chain;
    IDXGISwapChain3*& sc3 = a->sc3;  // held in FrameArgs: released by OnPresent even after a fault
    if (FAILED(a->sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    DXGI_SWAP_CHAIN_DESC1 d{};
    sc3->GetDesc1(&d);
    if (c.built && (d.Width != c.width || d.Height != c.height || d.Format != c.format ||
                    d.BufferCount != c.bufferCount)) {
        WaitAll(c);  // resized behind our back (no resize hook ran): rebuild
        ReleaseBuffers(c);
    }
    const FormatPath path = PathFor(d.Format);
    if (c.formatLogged != d.Format) {
        c.formatLogged = d.Format;
        if (path == FormatPath::Skip)
            SHVI_LOG("overlay: swap chain format %u not supported; not drawing", static_cast<unsigned>(d.Format));
        else if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
            SHVI_LOG("overlay: R10G10B10A2 swap chain: SDR values written unchanged (dim if the output is HDR10)");
        else if (path == FormatPath::Composite)
            SHVI_LOG("overlay: FP16 swap chain: composited as scRGB, paper white %.1f", kScRgbWhite);
    }
    if (path == FormatPath::Skip) {
        c.width = d.Width, c.height = d.Height, c.format = d.Format;
        return;
    }
    if (!c.built && !Build(c, d)) {
        SHVI_LOG("overlay: building D3D12 state failed; overlay off");
        G->disabled = true;
        ReleaseBuffers(c);
        return;
    }

    UINT i = sc3->GetCurrentBackBufferIndex();
    if (i >= c.buffers.size()) {
        return;
    }
    BufferState& b = c.buffers[i];
    WaitFence(c, b.fenceValue);  // normally already complete
    ID3D12Resource*& bb = a->bb;
    if (FAILED(sc3->GetBuffer(i, IID_PPV_ARGS(&bb)))) {
        return;
    }
    if (G->faultNextFrame) {  // testing::FaultNextFrame: fault with the back buffer held
        G->faultNextFrame = false;
        *static_cast<volatile int*>(nullptr) = 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(i) * c.rtvSize;
    c.device->CreateRenderTargetView(bb, nullptr, rtv);
    b.alloc->Reset();
    c.list->Reset(b.alloc, nullptr);
    const bool composite = path == FormatPath::Composite;
    D3D12_RESOURCE_BARRIER barriers[2] = {
        Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(c.offscreen, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    c.list->ResourceBarrier(composite ? 2 : 1, barriers);
    D3D12_CPU_DESCRIPTOR_HANDLE imguiRtv = rtv;
    if (composite) {
        imguiRtv = c.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
        const float transparent[4] = {0, 0, 0, 0};
        c.list->ClearRenderTargetView(imguiRtv, transparent, 0, nullptr);
    }
    c.list->OMSetRenderTargets(1, &imguiRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(d.Width), float(d.Height), 0, 1};
    D3D12_RECT sr{0, 0, LONG(d.Width), LONG(d.Height)};
    c.list->RSSetViewports(1, &vp);
    c.list->RSSetScissorRects(1, &sr);
    c.list->SetDescriptorHeaps(1, &c.srvHeap);

    ImGui::SetCurrentContext(c.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(d.Width), float(d.Height));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    std::shared_ptr<const Frame>& frame = a->frame;
    frame = G->list ? G->list->Front() : nullptr;
    // Stale cut-off: ticks publish every frame, so a generation that stops moving means ticks stopped.
    const ULONGLONG now = GetTickCount64();
    const std::uint64_t gen = G->list ? G->list->Generation() : 0;
    if (gen != G->seenGeneration || G->generationSeenAt == 0) {
        G->seenGeneration = gen;
        G->generationSeenAt = now;
    }
    const bool stale = now - G->generationSeenAt > kStaleFrameMs;
    if (frame && !stale) DrawItems(c, dl, *frame, float(d.Width), float(d.Height));
    if (G->testPattern) dl->AddRectFilled(ImVec2(8, 8), ImVec2(72, 40), IM_COL32(255, 0, 255, 255));
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), c.list);

    if (composite) {
        auto toSrv = Transition(c.offscreen, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.list->ResourceBarrier(1, &toSrv);
        c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        c.list->RSSetViewports(1, &vp);
        c.list->RSSetScissorRects(1, &sr);
        c.list->SetGraphicsRootSignature(c.compositeRoot);
        c.list->SetPipelineState(c.compositePso);
        c.list->SetDescriptorHeaps(1, &c.srvHeap);
        c.list->SetGraphicsRootDescriptorTable(0, c.offscreenSrvGpu);
        c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c.list->DrawInstanced(3, 1, 0, 0);
    }

    // Back buffer bound as RTV 0 for the callbacks (ImGui left its own state behind).
    c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    c.list->RSSetViewports(1, &vp);
    c.list->RSSetScissorRects(1, &sr);
    PresentContext ctx{d.Width, d.Height, static_cast<std::uint32_t>(d.Format), i, sc3, c.device, c.queue, c.list};
    RunCallbacks(ctx);

    auto barrier = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    c.list->ResourceBarrier(1, &barrier);
    if (HRESULT hr = c.list->Close(); FAILED(hr)) {
        // A callback broke the list (closed it, recorded something invalid). Executing it could
        // remove the game's device; drop the frame and turn the overlay off instead.
        G->disabled = true;
        SHVI_LOG("overlay: command list Close failed (0x%08lX) after the present callbacks; overlay off for "
                 "this session", static_cast<unsigned long>(hr));
        return;
    }
    ID3D12CommandList* lists[] = {c.list};
    c.queue->ExecuteCommandLists(1, lists);
    c.queue->Signal(c.fence, ++c.fenceLast);
    b.fenceValue = c.fenceLast;
    ++G->frames;
}

// The swap chain's queue from the creation hooks (AddRef'd), or null.
ID3D12CommandQueue* TakeCreationQueue(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(G->mapMu);
    auto it = G->queues.find(sc);
    if (it == G->queues.end()) return nullptr;
    it->second->AddRef();
    return it->second;
}

bool Attach(IDXGISwapChain* sc, ID3D12CommandQueue* queue, QueueSource source) {
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
        queue->Release();
        return false;
    }
    Chain& c = G->chain;
    if (c.device) {  // re-attach after a re-created swap chain: drop everything bound to the old device/queue
        WaitAll(c);
        ReleaseBuffers(c);
        if (c.backendInit) {
            ImGui::SetCurrentContext(c.imgui);
            ImGui_ImplDX12_Shutdown();
            c.backendInit = false;
        }
        SafeRelease(c.srvHeap);
        std::memset(c.srvUsed, 0, sizeof(c.srvUsed));
        SafeRelease(c.compositePso);
        SafeRelease(c.compositeRoot);
        SafeRelease(c.fence);
        c.fenceLast = 0;
        SafeRelease(c.queue);
        SafeRelease(c.device);
    }
    c.sc = sc;
    c.queue = queue;  // keeps the reference
    c.device = device;
    c.source = source;
    G->haveChain = true;
    SHVI_LOG("overlay: attached (queue via %s) to swap chain %p",
             source == QueueSource::Creation ? "creation hook" : "ExecuteCommandLists", static_cast<void*>(sc));
    return true;
}

// True when sc is the swap chain the overlay draws on (attaching it if it is the first).
bool SelectChain(IDXGISwapChain* sc, ID3D12CommandQueue* lastDirect) {
    if (G->haveChain) {
        if (G->chain.sc == sc) {
            G->othersSinceOurs = 0;
            return true;
        }
        if (!G->loggedOther) {
            G->loggedOther = true;
            SHVI_LOG("overlay: another swap chain %p presents; passed through", static_cast<void*>(sc));
        }
        // Takeover: the game replaced its swap chain (fullscreen/HDR toggle, device recovery) and the
        // old one stopped presenting. A D3D12 swap chain seen by the creation hooks takes its place.
        if (++G->othersSinceOurs < kTakeoverPresents) return false;
        ID3D12CommandQueue* q = TakeCreationQueue(sc);
        if (!q) return false;
        IDXGISwapChain* old = G->chain.sc;
        SHVI_LOG("overlay: swap chain %p stopped presenting; %p takes over", static_cast<void*>(old),
                 static_cast<void*>(sc));
        {
            std::lock_guard<std::mutex> lock(G->mapMu);
            if (auto it = G->queues.find(old); it != G->queues.end()) {
                it->second->Release();
                G->queues.erase(it);
            }
        }
        G->othersSinceOurs = 0;
        G->loggedOther = false;
        return Attach(sc, q, QueueSource::Creation);
    }
    if (ID3D12CommandQueue* q = TakeCreationQueue(sc)) return Attach(sc, q, QueueSource::Creation);

    // Late attach: a swap chain created before Install. Use the last DIRECT queue that
    // executed on this thread, if it belongs to the swap chain's device.
    if (G->fallbackGaveUp) return false;
    if (lastDirect) {
        ID3D12Device* scDevice = nullptr;
        ID3D12Device* qDevice = nullptr;
        bool match = SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&scDevice))) &&
                     SUCCEEDED(lastDirect->GetDevice(IID_PPV_ARGS(&qDevice))) && scDevice == qDevice;
        SafeRelease(scDevice);
        SafeRelease(qDevice);
        if (match) {
            lastDirect->AddRef();
            return Attach(sc, lastDirect, QueueSource::ExecuteFallback);
        }
    }
    if (++G->unmatchedPresents >= kFallbackPresents) {
        G->fallbackGaveUp = true;
        SHVI_LOG("overlay: no D3D12 queue found for the presenting swap chain after %d presents; "
                 "only swap chains created from now on can attach", kFallbackPresents);
    }
    return false;
}

void OnPresent(IDXGISwapChain* sc, ID3D12CommandQueue* lastDirect) {
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    if (!G->enabled || G->disabled || !SelectChain(sc, lastDirect)) return;
    FrameArgs a;
    a.sc = sc;
    DWORD code = 0;
    void* addr = nullptr;
    if (!SehRun(&DrawFrame, &a, &code, &addr)) {
        G->disabled = true;
        SHVI_LOG("overlay: fault 0x%08lX at %s on the present thread; overlay off for this session",
                 static_cast<unsigned long>(code), ModuleRva(addr).c_str());
    }
    // Success or fault: nothing the step acquired outlives the present.
    SafeRelease(a.bb);
    SafeRelease(a.sc3);
}

void OnResize(IDXGISwapChain* sc) {
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    if (!G->haveChain || G->chain.sc != sc) return;
    WaitAll(G->chain);
    ReleaseBuffers(G->chain);
}

// ------------------------------------------------------------------ hooks
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ID3D12CommandQueue* lastDirect = std::exchange(t_lastDirect, nullptr);
    if (t_depth == 0 && !(flags & DXGI_PRESENT_TEST)) OnPresent(sc, lastDirect);
    ++t_depth;
    HRESULT hr = G->origPresent(sc, sync, flags);
    --t_depth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    ID3D12CommandQueue* lastDirect = std::exchange(t_lastDirect, nullptr);
    if (t_depth == 0 && !(flags & DXGI_PRESENT_TEST)) OnPresent(sc, lastDirect);
    ++t_depth;
    HRESULT hr = G->origPresent1(sc, sync, flags, params);
    --t_depth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT f,
                                            UINT flags) {
    if (t_depth == 0) OnResize(sc);
    ++t_depth;
    HRESULT hr = G->origResizeBuffers(sc, count, w, h, f, flags);
    --t_depth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT f,
                                             UINT flags, const UINT* nodeMasks, IUnknown* const* queues) {
    if (t_depth == 0) OnResize(sc);
    ++t_depth;
    HRESULT hr = G->origResizeBuffers1(sc, count, w, h, f, flags, nodeMasks, queues);
    --t_depth;
    return hr;
}

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_lastDirect = q;
    G->origExecute(q, n, lists);
}

// A new swap chain: remember its queue if it is a D3D12 one.
void Created(IUnknown* device, IDXGISwapChain* sc) {
    if (!device || !sc) return;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&q)))) return;  // D3D11 and others: not ours
    {
        std::lock_guard<std::mutex> lock(G->mapMu);
        auto& slot = G->queues[sc];
        if (slot) slot->Release();
        slot = q;
    }
    // Same address as the swap chain we draw on: the old one is gone, start over on the next present.
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    if (G->haveChain && G->chain.sc == sc) {
        Chain& c = G->chain;
        WaitAll(c);
        ReleaseBuffers(c);
        c.sc = nullptr;
        G->haveChain = false;
        G->loggedOther = false;
        SHVI_LOG("overlay: swap chain %p was re-created; re-attaching on its next present", static_cast<void*>(sc));
    }
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    ++t_depth;
    HRESULT hr = G->origCreateSwapChain(f, device, desc, out);
    --t_depth;
    if (SUCCEEDED(hr) && out && *out) Created(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateForHwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                            IDXGISwapChain1** out) {
    ++t_depth;
    HRESULT hr = G->origCreateForHwnd(f, device, hwnd, desc, fs, output, out);
    --t_depth;
    if (SUCCEEDED(hr) && out && *out) Created(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateForCoreWindow(IDXGIFactory2* f, IUnknown* device, IUnknown* window,
                                                  const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                  IDXGISwapChain1** out) {
    ++t_depth;
    HRESULT hr = G->origCreateForCoreWindow(f, device, window, desc, output, out);
    --t_depth;
    if (SUCCEEDED(hr) && out && *out) Created(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateForComposition(IDXGIFactory2* f, IUnknown* device,
                                                   const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                   IDXGISwapChain1** out) {
    ++t_depth;
    HRESULT hr = G->origCreateForComposition(f, device, desc, output, out);
    --t_depth;
    if (SUCCEEDED(hr) && out && *out) Created(device, *out);
    return hr;
}

// ------------------------------------------------------------------ install
struct Targets {
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize = nullptr;
    void* resize1 = nullptr;
    void* createSwapChain = nullptr;
    void* createForHwnd = nullptr;
    void* createForCoreWindow = nullptr;
    void* createForComposition = nullptr;
    void* execute = nullptr;
};

void* Slot(void* obj, int slot) { return (*static_cast<void***>(obj))[slot]; }

// Throwaway factory, WARP device, queue, hidden 1x1 window and swap chain: read the code
// addresses from their vtables, then release everything.
bool FindTargets(Targets* t, std::string* err) {
    const Slots s = VtableSlots();
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* sc = nullptr;
    HWND hwnd = nullptr;
    const wchar_t* cls = L"shvi_overlay_probe";
    HINSTANCE inst = GetModuleHandleW(nullptr);
    bool ok = false;
    do {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { *err = "CreateDXGIFactory2"; break; }
        t->createSwapChain = Slot(factory, s.fCreateSwapChain);
        t->createForHwnd = Slot(factory, s.fCreateForHwnd);
        t->createForCoreWindow = Slot(factory, s.fCreateForCoreWindow);
        t->createForComposition = Slot(factory, s.fCreateForComposition);
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) { *err = "EnumWarpAdapter"; break; }
        if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            *err = "D3D12CreateDevice(WARP)";
            break;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { *err = "CreateCommandQueue"; break; }
        t->execute = Slot(queue, s.qExecuteCommandLists);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, cls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (!hwnd) { *err = "CreateWindowExW"; break; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 1;
        sd.Height = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc))) {
            *err = "CreateSwapChainForHwnd";
            break;
        }
        t->present = Slot(sc, s.scPresent);
        t->present1 = Slot(sc, s.scPresent1);
        t->resize = Slot(sc, s.scResizeBuffers);
        t->resize1 = Slot(sc, s.scResizeBuffers1);
        ok = true;
    } while (false);
    SafeRelease(sc);
    SafeRelease(queue);
    SafeRelease(device);
    SafeRelease(warp);
    SafeRelease(factory);
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(cls, inst);
    return ok;
}

struct HookSpec {
    const char* name;
    void* target;
    void* detour;
    void** orig;
};

bool InstallOnce(DrawList* list) {
    G = new Global;  // lives for the process: hooks may run until exit
    G->list = list;
    Targets t;
    std::string err;
    if (!FindTargets(&t, &err)) {
        SHVI_LOG("overlay: probe failed at %s; overlay off", err.c_str());
        return false;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        SHVI_LOG("overlay: MH_Initialize failed (%s); overlay off", MH_StatusToString(st));
        return false;
    }
    const HookSpec specs[] = {
        {"Present", t.present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&G->origPresent)},
        {"Present1", t.present1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&G->origPresent1)},
        {"ResizeBuffers", t.resize, reinterpret_cast<void*>(&HookResizeBuffers),
         reinterpret_cast<void**>(&G->origResizeBuffers)},
        {"ResizeBuffers1", t.resize1, reinterpret_cast<void*>(&HookResizeBuffers1),
         reinterpret_cast<void**>(&G->origResizeBuffers1)},
        {"CreateSwapChain", t.createSwapChain, reinterpret_cast<void*>(&HookCreateSwapChain),
         reinterpret_cast<void**>(&G->origCreateSwapChain)},
        {"CreateSwapChainForHwnd", t.createForHwnd, reinterpret_cast<void*>(&HookCreateForHwnd),
         reinterpret_cast<void**>(&G->origCreateForHwnd)},
        {"CreateSwapChainForCoreWindow", t.createForCoreWindow, reinterpret_cast<void*>(&HookCreateForCoreWindow),
         reinterpret_cast<void**>(&G->origCreateForCoreWindow)},
        {"CreateSwapChainForComposition", t.createForComposition, reinterpret_cast<void*>(&HookCreateForComposition),
         reinterpret_cast<void**>(&G->origCreateForComposition)},
        {"ExecuteCommandLists", t.execute, reinterpret_cast<void*>(&HookExecute),
         reinterpret_cast<void**>(&G->origExecute)},
    };
    std::vector<void*> created;
    for (const HookSpec& h : specs) {
        st = MH_CreateHook(h.target, h.detour, h.orig);
        if (st != MH_OK) {
            SHVI_LOG("overlay: hook %s at %s failed (%s); overlay off", h.name, ModuleRva(h.target).c_str(),
                     MH_StatusToString(st));
            for (void* c : created) MH_RemoveHook(c);
            return false;
        }
        created.push_back(h.target);
    }
    for (void* c : created) {
        st = MH_EnableHook(c);
        if (st != MH_OK) {
            SHVI_LOG("overlay: enabling hook at %s failed (%s); overlay off", ModuleRva(c).c_str(),
                     MH_StatusToString(st));
            for (void* r : created) MH_RemoveHook(r);
            return false;
        }
    }
    SHVI_LOG("overlay: DXGI hooks in (Present %s, ResizeBuffers %s, CreateSwapChainForHwnd %s)",
             ModuleRva(t.present).c_str(), ModuleRva(t.resize).c_str(), ModuleRva(t.createForHwnd).c_str());
    return true;
}

}  // namespace

bool Install(DrawList* list) {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [list] { ok = InstallOnce(list); });
    return ok;
}

Status GetStatus() {
    Status s{};
    if (!G) return s;
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    s.attached = G->haveChain;
    s.source = G->haveChain ? G->chain.source : QueueSource::None;
    s.frames = G->frames;
    s.width = G->chain.width;
    s.height = G->chain.height;
    s.format = static_cast<std::uint32_t>(G->chain.format);
    s.disabled = G->disabled;
    return s;
}

int RegisterPresent(PresentFn fn, void* user) {
    if (!fn || !G) return SHVI_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    for (auto& cb : G->callbacks)
        if (cb.first == fn && cb.second == user) return SHVI_E_INVALID_ARG;
    G->callbacks.emplace_back(fn, user);
    return SHVI_OK;
}

int UnregisterPresent(PresentFn fn, void* user) {
    if (!fn || !G) return SHVI_E_INVALID_ARG;
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    for (auto it = G->callbacks.begin(); it != G->callbacks.end(); ++it) {
        if (it->first == fn && it->second == user) {
            G->callbacks.erase(it);
            return SHVI_OK;
        }
    }
    return SHVI_E_INVALID_ARG;
}

void SetEnabled(bool on) {
    if (!G) return;
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    if (G->enabled != on) SHVI_LOG("overlay: %s", on ? "enabled" : "disabled by the core (inert)");
    G->enabled = on;
}
namespace testing {
void FaultNextFrame() {
    if (!G) return;
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    G->faultNextFrame = true;
}
}  // namespace testing

void SetTestPattern(bool on) {
    if (!G) return;
    std::lock_guard<std::recursive_mutex> lock(G->mu);
    G->testPattern = on;
}

}  // namespace shvi::overlay
