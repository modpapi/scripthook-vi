// GENERIC. D3D12 overlay (T7.1, docs/superpowers/specs/2026-11-24-d3d12-overlay-design.md §6.3).
//
// Hooks the code behind DXGI's Present/Present1/ResizeBuffers/ResizeBuffers1
// and the four IDXGIFactory2 swap-chain creation methods (MinHook inline
// detours, so every instance in the process is covered and other overlays
// chain). The first D3D12 swap chain that presents with a known queue gets the
// overlay: the published DrawList frame is drawn through Dear ImGui's DX12
// backend, then each RegisterPresent callback, then the original Present runs.
//
// It names no host and no game: the queue comes from the creation hooks (the
// `pDevice` argument of a D3D12 swap chain is its queue) or, for a swap chain
// created before Install, from ExecuteCommandLists on the presenting thread.
//
// Rules it keeps: no back buffer reference is held between presents (a host's
// ResizeBuffers keeps working); no CPU wait on the GPU in the common path
// except a per-buffer fence that has normally completed; a fault on the present
// thread disables the overlay for the session (logged as module+rva) and the
// original Present still runs; a faulting callback is unregistered.
//
// Known cost: Dear ImGui 1.93's DX12 backend uploads new glyphs (first use of a character at
// a new size) on the game's queue and waits for that upload on the present thread. That's a
// one-off hitch when the menu first shows text at a size, never a steady-state wait.
//
// Text size: pixel height = scale x back buffer height x kTextScale. 0.06 puts
// GTA's usual menu scale 0.35 at ~15 px on 720p, ~23 px on 1080p.
#pragma once
#include "overlay_drawlist.h"

#include <cstdint>

struct IDXGISwapChain3;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace shvi::overlay {

inline constexpr float kTextScale = 0.06f;

enum class QueueSource { None, Creation, ExecuteFallback };

struct Status {
    bool attached;
    QueueSource source;
    std::uint64_t frames;  // presents the overlay drew on
    std::uint32_t width, height, format;  // DXGI_FORMAT
    bool disabled;
};

// Once per process; false + log on failure. Creates and releases its own
// throwaway factory/device/swap chain first (WARP, hidden 1x1 window); these
// are never tracked.
bool Install(DrawList* list);
Status GetStatus();

struct PresentContext {
    std::uint32_t width, height, format, backBufferIndex;
    IDXGISwapChain3* swapChain;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12GraphicsCommandList* list;  // open, back buffer bound as RTV 0 in RENDER_TARGET state
};
using PresentFn = void (*)(const PresentContext&, void* user);
// SHVI_OK, or SHVI_E_INVALID_ARG (null fn, duplicate, or not registered).
int RegisterPresent(PresentFn fn, void* user);
int UnregisterPresent(PresentFn fn, void* user);

// Draws a fixed rect top-left on every present, independent of the list.
void SetTestPattern(bool on);

// Off = every hooked call passes straight through: no drawing, no GPU work, no callbacks.
// The core turns it off whenever it goes inert (signatures missing, tick hook failed, guard latched).
void SetEnabled(bool on);

// A published frame older than this is not drawn: when script ticks stop (loading, pause, hang)
// the menu vanishes instead of staying frozen. Every tick publishes, so a live menu never ages.
inline constexpr std::uint32_t kStaleFrameMs = 500;

namespace testing {
// Makes the next drawn frame fault inside the overlay's own present step (not in a callback).
void FaultNextFrame();
}  // namespace testing

// overlay_vtables.cpp: slot numbers from the SDK's C vtable structs.
struct Slots {
    int scPresent, scPresent1, scResizeBuffers, scResizeBuffers1, fCreateSwapChain, fCreateForHwnd,
        fCreateForCoreWindow, fCreateForComposition, qExecuteCommandLists;
};
Slots VtableSlots();

}  // namespace shvi::overlay
