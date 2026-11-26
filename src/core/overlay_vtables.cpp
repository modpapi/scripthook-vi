// GENERIC. Vtable slot numbers for the overlay's hooks, computed from the
// Windows SDK's C vtable structs (CINTERFACE), never hard-coded. This
// translation unit only does that; it sees COM interfaces as C structs.
#define CINTERFACE
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstddef>

#include "overlay_d3d12.h"

namespace shvi::overlay {

Slots VtableSlots() {
    constexpr auto slot = [](std::size_t offset) { return static_cast<int>(offset / sizeof(void*)); };
    Slots s{};
    s.scPresent = slot(offsetof(IDXGISwapChain3Vtbl, Present));
    s.scPresent1 = slot(offsetof(IDXGISwapChain3Vtbl, Present1));
    s.scResizeBuffers = slot(offsetof(IDXGISwapChain3Vtbl, ResizeBuffers));
    s.scResizeBuffers1 = slot(offsetof(IDXGISwapChain3Vtbl, ResizeBuffers1));
    s.fCreateSwapChain = slot(offsetof(IDXGIFactory2Vtbl, CreateSwapChain));
    s.fCreateForHwnd = slot(offsetof(IDXGIFactory2Vtbl, CreateSwapChainForHwnd));
    s.fCreateForCoreWindow = slot(offsetof(IDXGIFactory2Vtbl, CreateSwapChainForCoreWindow));
    s.fCreateForComposition = slot(offsetof(IDXGIFactory2Vtbl, CreateSwapChainForComposition));
    s.qExecuteCommandLists = slot(offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists));
    return s;
}

}  // namespace shvi::overlay
