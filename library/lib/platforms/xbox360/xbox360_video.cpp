#include <borealis/platforms/xbox360/xbox360_video.hpp>
#include <borealis/extern/nanovg/nanovg_d3d9_xbox360.h>
#include <borealis/core/logger.hpp>

namespace brls
{
Xbox360VideoContext::Xbox360VideoContext(uint32_t w, uint32_t h) : width(w), height(h)
{
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = width; pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT hr = Direct3D_CreateDevice(0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr) || !device)
    {
        brls::Logger::error("Xbox 360 D3D9 device creation failed: 0x{:08x}", static_cast<unsigned int>(hr));
        device = nullptr;
        return;
    }

    vg = nvgCreateD3D9(device, NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg)
        brls::Logger::error("Xbox 360 NanoVG D3D9 context creation failed");
}
Xbox360VideoContext::~Xbox360VideoContext() { if (vg) nvgDeleteD3D9(vg); if (device) device->Release(); }
void Xbox360VideoContext::clear(NVGcolor color) {
    if (!device) return;
    const DWORD r = (DWORD)(color.rgba[0] * 255.0f);
    const DWORD g = (DWORD)(color.rgba[1] * 255.0f);
    const DWORD b = (DWORD)(color.rgba[2] * 255.0f);
    const DWORD a = (DWORD)(color.rgba[3] * 255.0f);
    IDirect3DDevice9_Clear(device, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
        (a << 24) | (r << 16) | (g << 8) | b, 1.0f, 0);
}
void Xbox360VideoContext::beginFrame() {
    // Application::frame owns nvgBeginFrame/nvgEndFrame for every platform.
    // Keep this hook limited to backend preparation.
    if (device)
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}
void Xbox360VideoContext::endFrame() {
    if (!device)
        return;

    HRESULT hr = IDirect3DDevice9_Present(device, 0, 0, 0, 0);
    if (FAILED(hr))
        brls::Logger::error("Xbox 360 D3D9 Present failed: 0x{:08x}", static_cast<unsigned int>(hr));
}
void Xbox360VideoContext::resetState() { if (device) device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); }
double Xbox360VideoContext::getScaleFactor() { return 1.0; }
NVGcontext* Xbox360VideoContext::getNVGContext() { return vg; }
void Xbox360VideoContext::setSwapInterval(int) {}
IDirect3DDevice9* Xbox360VideoContext::getDevice() const { return device; }
}
