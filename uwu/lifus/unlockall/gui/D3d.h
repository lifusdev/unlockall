#pragma once
#include "lifus/unlockall/mem/Handle.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <chrono>

namespace uwu {
template <class t_value> using com_ptr = Microsoft::WRL::ComPtr<t_value>;

struct gfx {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain1> swap_chain;
    com_ptr<ID3D11RenderTargetView> target;
    com_ptr<IDCompositionDevice> composition;
    com_ptr<IDCompositionTarget> composition_target;
    com_ptr<IDCompositionVisual> visual;
    unique_handle timer;
    HMONITOR monitor{};
    UINT target_width{}, target_height{};
    std::chrono::steady_clock::duration interval = std::chrono::milliseconds(16);
    std::chrono::steady_clock::time_point next_frame{};

    DWORD wait_frame(HWND game);
    bool create_target();
    bool resize(UINT w, UINT h);
    bool init(HWND window);
};

}
