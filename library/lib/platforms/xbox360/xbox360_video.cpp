#include <borealis/platforms/xbox360/xbox360_video.hpp>
#include <borealis/platforms/xbox360/xbox360_yuv.hpp>
#if X360_XAV
#include <borealis/platforms/xbox360/xbox360_xav.hpp>
#endif
#include <borealis/core/application.hpp>
#include <borealis/extern/nanovg/nanovg_d3d9_xbox360.h>
#include <borealis/core/logger.hpp>
#include <xbox.h>

namespace brls
{
Xbox360VideoContext::Xbox360VideoContext(uint32_t w, uint32_t h) : width(w), height(h)
{
    XVIDEO_MODE videoMode{};
    XGetVideoMode(&videoMode);
    width = videoMode.fIsHiDef ? 1280 : 640;
    height = videoMode.fIsHiDef ? 720 : 480;

    Application::setWindowSize(width, height);
    brls::Logger::info("Xbox video mode: {}x{} (HD={}, widescreen={}, {:.2f} Hz); backbuffer: {}x{}",
                       videoMode.dwDisplayWidth, videoMode.dwDisplayHeight,
                       videoMode.fIsHiDef, videoMode.fIsWideScreen, videoMode.RefreshRate,
                       width, height);

    pp = D3DPRESENT_PARAMETERS{};
    pp.BackBufferWidth = width; pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = FALSE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
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
Xbox360VideoContext::~Xbox360VideoContext() {
    Xbox360YUVRenderer::release(device);
    if (vg) nvgDeleteD3D9(vg);
    vg = nullptr;
#if !defined(PLATFORM_XBOX360)
    if (device) device->Release();
#else
    // XDK caveat: calling the final IDirect3DDevice9::Release() during title
    // teardown routes through D3DDevice_Release -> ... -> D3DDevice_SetStreamSource,
    // which dereferences the device's internal stream-binding table. Those slots
    // are no longer in a sane state at exit (observed as 0xC0000005 writing
    // 0x43D6D088 from inside D3DDevice_SetStreamSource). On Xbox 360 a title is
    // destroyed wholesale: the XEX loader / HAL takes back the GPU device and all
    // video memory when the title unloads, and OXDK reference samples exit without
    // releasing the device. Skipping the device release here is intentional and
    // required; individual D3D resources above are still released normally.
    device = nullptr;
#endif
}
void Xbox360VideoContext::clear(NVGcolor color) {
    if (!device) return;
    const DWORD r = (DWORD)(color.rgba[0] * 255.0f);
    const DWORD g = (DWORD)(color.rgba[1] * 255.0f);
    const DWORD b = (DWORD)(color.rgba[2] * 255.0f);
    const DWORD a = (DWORD)(color.rgba[3] * 255.0f);
    // Stencil is cleared because NanoVG runs with NVG_STENCIL_STROKES; the
    // depth buffer is not: every drawer this context hosts (NanoVG's flush and
    // the YUV video quad) sets D3DRS_ZENABLE to FALSE, so the Z clear only cost
    // a full-screen write per frame.
    IDirect3DDevice9_Clear(device, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_STENCIL,
        (a << 24) | (r << 16) | (g << 8) | b, 1.0f, 0);
}
void Xbox360VideoContext::beginFrame() {
    // Application::frame owns nvgBeginFrame/nvgEndFrame for every platform.
    // Keep this hook limited to backend preparation.
    if (!device)
        return;

    D3DVIEWPORT9 viewport{};
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    const HRESULT hr = IDirect3DDevice9_SetViewport(device, &viewport);
    if (FAILED(hr))
        brls::Logger::error("Xbox 360 D3D9 SetViewport failed: 0x{:08x}", static_cast<unsigned int>(hr));

    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}
void Xbox360VideoContext::endFrame() {
    if (!device)
        return;

    // XAV render contract: PostRender right before Swap, PreRender right
    // after. Both are no-ops until XAVInitialize has run (i.e. until the
    // experimental hardware-decode path is actually in use).
#if X360_XAV
    const bool xav = Xbox360Xav::instance().initialized();
    if (xav) Xbox360Xav::instance().postRender();
#endif

    HRESULT hr = IDirect3DDevice9_Present(device, 0, 0, 0, 0);
    if (FAILED(hr))
        brls::Logger::error("Xbox 360 D3D9 Present failed: 0x{:08x}", static_cast<unsigned int>(hr));

#if X360_XAV
    if (xav) Xbox360Xav::instance().preRender();
#endif
}
void Xbox360VideoContext::resetState() { if (device) device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); }
double Xbox360VideoContext::getScaleFactor() { return 1.0; }
NVGcontext* Xbox360VideoContext::getNVGContext() { return vg; }
void Xbox360VideoContext::setSwapInterval(int) {}
IDirect3DDevice9* Xbox360VideoContext::getDevice() const { return device; }
const D3DPRESENT_PARAMETERS* Xbox360VideoContext::getPresentParameters() const { return &pp; }
}
