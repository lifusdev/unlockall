#include "lifus/unlockall/gui/D3d.h"
#include <algorithm>
#include <iterator>

namespace uwu {
DWORD gfx::wait_frame(HWND game) {
    const auto current_monitor = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    if (monitor != current_monitor) {
        monitor = current_monitor;
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const unsigned hz = GetMonitorInfoW(monitor, &info) &&
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1
            ? std::clamp<unsigned>(mode.dmDisplayFrequency, 30, 240)
            : 60;
        interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(hz)));
    }
    const auto remaining = next_frame - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero()) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(
            1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (!SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE))
            return WAIT_FAILED;
        const HANDLE handle = timer.get();
        const auto result_value =
            MsgWaitForMultipleObjectsEx(1, &handle, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result_value != WAIT_OBJECT_0)
            return result_value;
    }
    return WAIT_OBJECT_0;
}

bool gfx::create_target() {
    com_ptr<ID3D11Texture2D> buffer;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf()))) ||
        FAILED(device->CreateRenderTargetView(buffer.Get(), nullptr, target.GetAddressOf())))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    target_width = desc.Width;
    target_height = desc.Height;
    return true;
}

bool gfx::resize(UINT w, UINT h) {
    if (target && target_width == w && target_height == h)
        return true;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    target.Reset();
    return SUCCEEDED(swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)) && create_target();
}

bool gfx::init(HWND window) {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const auto create = [&](D3D_DRIVER_TYPE driver) {
        return D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr,
            context.ReleaseAndGetAddressOf());
    };
    HRESULT result_value = create(D3D_DRIVER_TYPE_HARDWARE);
    if (result_value == DXGI_ERROR_UNSUPPORTED)
        result_value = create(D3D_DRIVER_TYPE_WARP);
    if (FAILED(result_value))
        return false;
    com_ptr<IDXGIDevice1> dxgi;
    com_ptr<IDXGIAdapter> adapter;
    com_ptr<IDXGIFactory2> factory;
    if (FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
        return false;
    dxgi->SetMaximumFrameLatency(2);
    RECT client{};
    if (!GetClientRect(window, &client))
        return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    desc.Height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(
            factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, swap_chain.GetAddressOf())))
        return false;
    timer.reset(CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE));
    if (!timer)
        timer.reset(CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE));
    if (!timer || FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(composition.GetAddressOf()))) ||
        FAILED(composition->CreateTargetForHwnd(window, TRUE, composition_target.GetAddressOf())) ||
        FAILED(composition->CreateVisual(visual.GetAddressOf())) ||
        FAILED(visual->SetContent(swap_chain.Get())) || FAILED(composition_target->SetRoot(visual.Get())))
        return false;
    return SUCCEEDED(composition->Commit()) && create_target();
}
}
