//
// Xbox 360 GPU video renderer: planar YUV420 -> RGB in a pixel shader.
//
// The frame comes from libmpv's software renderer (sw_format "yuv420p"),
// already sitting in a contiguous I420 CPU buffer.  All this file does is copy
// the three planes into L8 textures and draw one quad, so the only per-frame
// CPU work is three memcpys instead of a full colour conversion.
//

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/platform.hpp>
#include <borealis/platforms/xbox360/xbox360_video.hpp>
#include <borealis/platforms/xbox360/xbox360_yuv.hpp>
#include <borealis/platforms/xbox360/xbox360_perf.hpp>

#if defined(PLATFORM_XBOX360)

#include <nanovg.h>
#include <windows.h>
#include <xbox.h> // XMemCpyStreaming_WriteCombined (xapilib.lib)
#include <d3d9.h>
#include <d3dx9shader.h>

#include <cstring>
#include <string>
#include <cstdint>

namespace brls
{

namespace
{

// Per-frame cost breakdown for the on-device performance logs.  Nothing here
// runs in the hot path more than once per video frame.  Timing goes through
// xbox360perf (QueryPerformanceCounter): system_clock only has millisecond
// resolution on this platform, which is why the state/draw segment used to
// read back as 0.00 ms.
struct DrawProbe
{
    double uploadMs  = 0;
    int uploadFrames = 0;
    double uploadBytes = 0;
    int lockFails    = 0;
    double submitMs  = 0;
    int frames       = 0;
    // Cumulative over the whole run: surface churn must stay visible even in
    // windows where no draw happened at all.
    int texCreated   = 0;
    int texReleased  = 0;
    std::uint64_t lastFlush = 0;
    xbox360perf::Ring uploadRing;
    xbox360perf::Ring submitRing;
    void flush()
    {
#if !X360_RUNTIME_PROBES
        return;
#endif
        if (frames == 0) return;
        // Time gate as well as the 60-sample gate: at low fps the sample
        // count alone delayed this line past the interesting playback window.
        const std::uint64_t now = xbox360perf::nowUsec();
        if (lastFlush == 0)
        {
            lastFlush = now;
            return;
        }
        if (now - lastFlush < 2000000 && frames < 60) return;
        const double uploadAvg   = uploadFrames ? uploadMs / uploadFrames : 0.0;
        const double kibPerFrame = uploadFrames ? uploadBytes / uploadFrames / 1024.0 : 0.0;
        brls::Logger::info(
            "[x360-perf] yuv plane upload: {:.2f} ms avg, p95 {:.2f}, max {:.2f} "
            "({} new frames, {:.0f} KiB/frame, {} lock fails)",
            uploadAvg, uploadRing.quantile(0.95f), uploadRing.maxMs(), uploadFrames, kibPerFrame, lockFails);
        brls::Logger::info(
            "[x360-perf] state+quad draw: {:.2f} ms avg, p95 {:.2f}, max {:.2f} "
            "({} draws, surfaces created {} released {})",
            submitMs / frames, submitRing.quantile(0.95f), submitRing.maxMs(), frames, texCreated, texReleased);
        uploadMs     = 0;
        uploadFrames = 0;
        uploadBytes  = 0;
        lockFails    = 0;
        submitMs     = 0;
        frames       = 0;
        lastFlush    = now;
        uploadRing.reset();
        submitRing.reset();
    }
};
DrawProbe g_probe;

struct YUVVertex
{
    float position[3];
    float uv[2];
};

// Pass-through vertex shader: the quad is built in clip space already and the
// viewport (not a transform matrix) does the placement.
const char* YUV_VS_SOURCE =
    "struct VS_IN { float4 position : POSITION; float2 uv : TEXCOORD0; };"
    "struct VS_OUT { float4 position : POSITION; float2 uv : TEXCOORD0; };"
    "VS_OUT main(VS_IN input) {"
    "  VS_OUT o; o.position = input.position; o.uv = input.uv; return o;"
    "}";

// c0 = (alpha, lumaScale, vToR, uToG)
// c1 = (vToG, uToB, 0, 0)
// Coefficients are the standard limited-range matrices; mpv's software
// renderer guesses the colour space from the surface size exactly the way
// mp_csp_guess_colorspace() does, so we pick the matching matrix here.
const char* YUV_PS_SOURCE =
    "sampler2D texY : register(s0);"
    "sampler2D texU : register(s1);"
    "sampler2D texV : register(s2);"
    "float4 c0 : register(c0);"
    "float4 c1 : register(c1);"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {"
    "  float y = (tex2D(texY, uv).r - 0.0625) * c0.y;"
    "  float u = tex2D(texU, uv).r - 0.5;"
    "  float v = tex2D(texV, uv).r - 0.5;"
    "  float r = saturate(y + c0.z * v);"
    "  float g = saturate(y + c0.w * u + c1.x * v);"
    "  float b = saturate(y + c1.y * u);"
    "  return float4(r * c0.x, g * c0.x, b * c0.x, c0.x);"
    "}";

// Passthrough quad for an already-decoded RGB texture (the XAV hardware-decode
// path).  c0.x carries the alpha so the quad blends like the YUV one.
const char* RGB_PS_SOURCE =
    "sampler2D texRGB : register(s0);"
    "float4 c0 : register(c0);"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {"
    "  float3 c = tex2D(texRGB, uv).rgb;"
    "  return float4(c * c0.x, c0.x);"
    "}";

struct RendererState
{
    IDirect3DDevice9* device             = nullptr;
    // Triple-buffered plane sets.  Each upload targets its own slot and the
    // slot rotates every frame, so LockRect(D3DLOCK_NOOVERWRITE) can return
    // immediately without waiting for the GPU to finish sampling previous
    // frames.  XDK does not support D3DLOCK_DISCARD/DONOTWAIT; NOOVERWRITE
    // paired with rotating buffers is the documented non-stalling pattern.
    static constexpr int BUFFER_COUNT     = 3;
    IDirect3DTexture9* plane[BUFFER_COUNT][3] = {{nullptr, nullptr, nullptr},
                                                 {nullptr, nullptr, nullptr},
                                                 {nullptr, nullptr, nullptr}};
    int cur                            = 0;  // slot sampled by draw()
    int next                           = 1;  // slot targeted by the next upload
    IDirect3DVertexDeclaration9* decl    = nullptr;
    IDirect3DVertexBuffer9* vertices     = nullptr;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader   = nullptr;
    IDirect3DPixelShader9* rgbShader     = nullptr; // XAV RGB passthrough
    int texWidth                         = 0;
    int texHeight                        = 0;
    bool ready                           = false;
    // Setup failures (notably D3DXCompileShader) are deterministic; latch them
    // so a broken compiler/flag combination is not retried once per frame.
    bool initFailed                      = false;
};

RendererState g_state;

// Render states we touch and must hand back to NanoVG unchanged.  NanoVG's own
// flush re-applies most of them, but not the viewport, the sampler addressing
// and the texture bindings, so we restore everything we change regardless.
struct SavedState
{
    D3DVIEWPORT9 viewport{};
    DWORD fillMode, cullMode, zEnable, zWriteEnable, alphaTestEnable, alphaBlendEnable;
    DWORD srcBlend, destBlend, stencilEnable, scissorTestEnable, colorWriteEnable;
    DWORD addressU, addressV;
};

void saveState(IDirect3DDevice9* device, SavedState& s)
{
    device->GetViewport(&s.viewport);
    device->GetRenderState(D3DRS_FILLMODE, &s.fillMode);
    device->GetRenderState(D3DRS_CULLMODE, &s.cullMode);
    device->GetRenderState(D3DRS_ZENABLE, &s.zEnable);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &s.zWriteEnable);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &s.alphaTestEnable);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &s.alphaBlendEnable);
    device->GetRenderState(D3DRS_SRCBLEND, &s.srcBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &s.destBlend);
    device->GetRenderState(D3DRS_STENCILENABLE, &s.stencilEnable);
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &s.scissorTestEnable);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &s.colorWriteEnable);
    device->GetSamplerState(0, D3DSAMP_ADDRESSU, &s.addressU);
    device->GetSamplerState(0, D3DSAMP_ADDRESSV, &s.addressV);
}

void restoreState(IDirect3DDevice9* device, const SavedState& s)
{
    device->SetViewport(&s.viewport);
    device->SetRenderState(D3DRS_FILLMODE, s.fillMode);
    device->SetRenderState(D3DRS_CULLMODE, s.cullMode);
    device->SetRenderState(D3DRS_ZENABLE, s.zEnable);
    device->SetRenderState(D3DRS_ZWRITEENABLE, s.zWriteEnable);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, s.alphaTestEnable);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, s.alphaBlendEnable);
    device->SetRenderState(D3DRS_SRCBLEND, s.srcBlend);
    device->SetRenderState(D3DRS_DESTBLEND, s.destBlend);
    device->SetRenderState(D3DRS_STENCILENABLE, s.stencilEnable);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, s.scissorTestEnable);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, s.colorWriteEnable);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, s.addressU);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, s.addressV);
    // NanoVG binds its own font/image textures per call, but never unbinds
    // them, so make sure the video planes do not survive into the next flush.
    device->SetTexture(0, nullptr);
    device->SetTexture(1, nullptr);
    device->SetTexture(2, nullptr);
}

bool compileShader(IDirect3DDevice9* device, const char* source, const char* profile, ID3DXBuffer** out)
{
    ID3DXBuffer* errors = nullptr;
    HRESULT hr          = D3DXCompileShader(source, (UINT)std::strlen(source), nullptr, nullptr, "main", profile, 0,
                                            out, &errors, nullptr);
    if (FAILED(hr)) {
        std::string detail = "no compiler output";
        if (errors && errors->GetBufferPointer()) detail = (const char*)errors->GetBufferPointer();
        brls::Logger::error("Xbox360YUVRenderer: {} shader compilation failed (0x{:08x}): {}", profile,
                            (unsigned int)hr, detail);
    }
    if (errors) errors->Release();
    return SUCCEEDED(hr);
}

void releaseObjects()
{
    for (auto& set : g_state.plane) {
        for (auto& tex : set) {
            if (tex) {
                g_probe.texReleased++;
                tex->Release();
                tex = nullptr;
            }
        }
    }
    if (g_state.vertices) {
        g_state.vertices->Release();
        g_state.vertices = nullptr;
    }
    if (g_state.decl) {
        g_state.decl->Release();
        g_state.decl = nullptr;
    }
    if (g_state.vertexShader) {
        g_state.vertexShader->Release();
        g_state.vertexShader = nullptr;
    }
    if (g_state.pixelShader) {
        g_state.pixelShader->Release();
        g_state.pixelShader = nullptr;
    }
    if (g_state.rgbShader) {
        g_state.rgbShader->Release();
        g_state.rgbShader = nullptr;
    }
    g_state.texWidth  = 0;
    g_state.texHeight = 0;
    g_state.ready     = false;
    g_state.device    = nullptr;
}

bool init(IDirect3DDevice9* device)
{
    if (g_state.initFailed) return false;
    if (g_state.device == device && g_state.ready) return true;
    if (g_state.device != device) releaseObjects();

    static const YUVVertex quad[4] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
    };
    static const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };

    ID3DXBuffer* vsCode = nullptr;
    ID3DXBuffer* psCode = nullptr;
    ID3DXBuffer* rgbCode = nullptr;
    if (!compileShader(device, YUV_VS_SOURCE, "vs_2_0", &vsCode) || !compileShader(device, YUV_PS_SOURCE, "ps_2_0", &psCode) ||
        !compileShader(device, RGB_PS_SOURCE, "ps_2_0", &rgbCode)) {
        if (vsCode) vsCode->Release();
        if (psCode) psCode->Release();
        if (rgbCode) rgbCode->Release();
        g_state.initFailed = true;
        releaseObjects();
        return false;
    }

    HRESULT hr = device->CreateVertexShader((const DWORD*)vsCode->GetBufferPointer(), &g_state.vertexShader);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader((const DWORD*)psCode->GetBufferPointer(), &g_state.pixelShader);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader((const DWORD*)rgbCode->GetBufferPointer(), &g_state.rgbShader);
    vsCode->Release();
    psCode->Release();
    rgbCode->Release();
    if (FAILED(hr)) {
        brls::Logger::error("Xbox360YUVRenderer: CreateShader failed (0x{:08x})", (unsigned int)hr);
        g_state.initFailed = true;
        releaseObjects();
        return false;
    }

    hr = device->CreateVertexDeclaration(elements, &g_state.decl);
    if (SUCCEEDED(hr)) {
        hr = device->CreateVertexBuffer(sizeof(quad), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_state.vertices, nullptr);
    }
    if (FAILED(hr)) {
        brls::Logger::error("Xbox360YUVRenderer: vertex setup failed (0x{:08x})", (unsigned int)hr);
        g_state.initFailed = true;
        releaseObjects();
        return false;
    }
    void* data = nullptr;
    if (FAILED(g_state.vertices->Lock(0, sizeof(quad), &data, 0)) || !data) {
        g_state.initFailed = true;
        releaseObjects();
        return false;
    }
    std::memcpy(data, quad, sizeof(quad));
    g_state.vertices->Unlock();

    g_state.device = device;
    g_state.ready  = true;
    brls::Logger::info("Xbox360YUVRenderer: shaders ready");
    return true;
}

// Fill a freshly created plane with one byte value.  CreateTexture memory on
// this platform starts out zeroed, and zeroed YUV renders as *green* through
// the conversion shader (U=V=0 is not the neutral chroma point), so paint
// limited-range black before the texture can be sampled.  One-time cost per
// texture creation.
void fillPlane(IDirect3DTexture9* texture, uint8_t value, int width, int height)
{
    if (!texture) return;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0)) || !locked.pBits) return;
    auto* dest = (unsigned char*)locked.pBits;
    for (int r = 0; r < height; r++) {
        std::memset(dest + (size_t)r * locked.Pitch, value, (size_t)width);
    }
    texture->UnlockRect(0);
}

bool ensureTextures(int width, int height)
{
    if (g_state.plane[0][0] && g_state.texWidth == width && g_state.texHeight == height) return true;

    for (auto& set : g_state.plane) {
        for (auto& tex : set) {
            if (tex) {
                g_probe.texReleased++;
                tex->Release();
                tex = nullptr;
            }
        }
    }

    const int chromaWidth  = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    const D3DPOOL pool     = D3DPOOL_MANAGED;
    bool ok                = true;
    for (int i = 0; i < RendererState::BUFFER_COUNT; i++) {
        HRESULT hr = g_state.device->CreateTexture(width, height, 1, 0, D3DFMT_LIN_L8, pool, &g_state.plane[i][0], nullptr);
        if (SUCCEEDED(hr)) {
            g_probe.texCreated++;
            hr = g_state.device->CreateTexture(chromaWidth, chromaHeight, 1, 0, D3DFMT_LIN_L8, pool, &g_state.plane[i][1], nullptr);
        }
        if (SUCCEEDED(hr)) {
            g_probe.texCreated++;
            hr = g_state.device->CreateTexture(chromaWidth, chromaHeight, 1, 0, D3DFMT_LIN_L8, pool, &g_state.plane[i][2], nullptr);
        }
        if (SUCCEEDED(hr)) g_probe.texCreated++;
        if (FAILED(hr)) {
            brls::Logger::error("Xbox360YUVRenderer: CreateTexture {}x{} failed (0x{:08x})", width, height,
                                (unsigned int)hr);
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (auto& set : g_state.plane) {
            for (auto& tex : set) {
                if (tex) {
                    g_probe.texReleased++;
                    tex->Release();
                    tex = nullptr;
                }
            }
        }
        return false;
    }

    g_state.texWidth  = width;
    g_state.texHeight = height;
    // Un-uploaded textures must never show as green: pre-fill black
    // (Y=16, U=V=128) until the first real frame arrives.
    for (int i = 0; i < RendererState::BUFFER_COUNT; i++) {
        fillPlane(g_state.plane[i][0], 16, width, height);
        fillPlane(g_state.plane[i][1], 128, chromaWidth, chromaHeight);
        fillPlane(g_state.plane[i][2], 128, chromaWidth, chromaHeight);
    }
    g_state.cur  = 0;
    g_state.next = 1;
    return true;
}

// Returns the number of bytes actually copied (0 = lock failed), so the probe
// can report per-frame upload volume and lock failures separately from timing.
std::size_t uploadPlane(IDirect3DTexture9* texture, const unsigned char* source, size_t sourcePitch, int width, int height)
{
    D3DLOCKED_RECT locked{};
    // NOOVERWRITE: we never read back, and the three slot sets rotate every
    // frame, so the GPU cannot still be sampling this exact buffer.  This lets
    // LockRect return without draining pending draw calls.  Only the per-frame
    // fill path uses it; fillPlane (one-time texture paint) keeps flags 0.
    if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_NOOVERWRITE)) || !locked.pBits)
    {
        g_probe.lockFails++;
        return 0;
    }
    auto* dest       = (unsigned char*)locked.pBits;
    const size_t row = (size_t)width;
    // The lock pointer targets write-combined GPU memory.  The Xenon WC
    // store/gather buffers only combine runs that are aligned, at least four
    // bytes wide, gap-free and written once (Kernel_Write_Combined), so a plain
    // memcpy of `row` bytes per line loses the combining at every line tail
    // whenever the row length or the line start is not four-byte aligned (a
    // 854-wide source, for example) and each tail becomes its own partial flush
    // (the PIX "Bad Write-Combined Writes" pattern).  XMemCpyStreaming_
    // WriteCombined is the XDK's cached -> write-combined copy: it issues the
    // dcbt/dcbf/dcbz128 sequence itself and keeps its L2 footprint at 128 KB,
    // which also makes the old manual prefetch loop unnecessary (its
    // destination dcbt was useless against non-cacheable memory anyway).
    if (locked.Pitch == sourcePitch) {
        // Locked line stride matches the source, so the whole plane is one
        // gap-free run: copy it in a single streaming burst instead of once per
        // line.  The source's own row padding is copied too, but those bytes
        // land in the texture's unsampled padding columns.
        XMemCpyStreaming_WriteCombined(dest, source, sourcePitch * (size_t)height);
    } else {
        for (int r = 0; r < height; r++) {
            XMemCpyStreaming_WriteCombined(dest + (size_t)r * locked.Pitch,
                                           source + (size_t)r * sourcePitch, row);
        }
    }
    texture->UnlockRect(0);
    return row * (size_t)height;
}

inline long floorI(float v)
{
    long i = (long)v;
    return (v < (float)i) ? i - 1 : i;
}

inline long ceilI(float v)
{
    long i = (long)v;
    return (v > (float)i) ? i + 1 : i;
}

} // namespace

bool Xbox360YUVRenderer::draw(NVGcontext* vg, const void* frame, int width, int height, std::size_t pitch, float x,
                              float y, float w, float h, float alpha)
{
    if (!vg || width <= 0 || height <= 0 || w <= 0.0f || h <= 0.0f) return false;
    if (alpha <= 0.0f) return false;
    if (pitch < (size_t)width || (pitch & 1) != 0) return false;

    // GPU letterbox: the surface keeps the video's native size for a 1:1
    // copy inside libmpv_sw, so mpv no longer bakes letterbox bars into the
    // buffer.  Fit the texture's aspect into the view rect here instead; the
    // bars fall back to the NanoVG black background that is flushed below,
    // right before the quad goes on top.  When the surface is the rect itself
    // (video size unknown yet) the aspects match and this is a no-op, so the
    // bars mpv did bake in keep showing as before.
    {
        const float texAspect  = (float)width / (float)height;
        const float rectAspect = w / h;
        if (rectAspect > texAspect) {
            const float fitW = h * texAspect;
            x += (w - fitW) * 0.5f;
            w = fitW;
        } else if (rectAspect < texAspect) {
            const float fitH = w / texAspect;
            y += (h - fitH) * 0.5f;
            h = fitH;
        }
    }

    NVGparams* params = nvgInternalParams(vg);
    if (!params || !params->renderFlush) return false;

    auto* videoContext = (Xbox360VideoContext*)Application::getPlatform()->getVideoContext();
    if (!videoContext) return false;
    IDirect3DDevice9* device = videoContext->getDevice();
    if (!device) return false;

    if (!init(device)) return false;
    if (frame && !ensureTextures(width, height)) return false;

    // Put everything NanoVG has queued so far (the video background plus all
    // UI drawn before this view) on the screen first; the geometry NanoVG
    // collects from here on is flushed when Application::frame calls
    // nvgEndFrame, i.e. on top of the video.
    params->renderFlush(params->userPtr);

    const std::uint64_t uploadStart = xbox360perf::nowUsec();
    if (frame && g_state.plane[0][0]) {
        auto& slot     = g_state.plane[g_state.next];
        const std::size_t yBytes =
            uploadPlane(slot[0], (const unsigned char*)frame, pitch, width, height);
        const size_t chromaPitch = pitch / 2;
        const int chromaWidth    = (width + 1) / 2;
        const int chromaHeight   = (height + 1) / 2;
        const size_t lumaSize    = pitch * (size_t)height;
        const std::size_t uBytes = uploadPlane(slot[1], (const unsigned char*)frame + lumaSize, chromaPitch,
                                               chromaWidth, chromaHeight);
        const std::size_t vBytes = uploadPlane(slot[2],
                                               (const unsigned char*)frame + lumaSize + chromaPitch * (size_t)chromaHeight,
                                               chromaPitch, chromaWidth, chromaHeight);
        g_probe.uploadBytes += (double)(yBytes + uBytes + vBytes);
        const double uploadMsDelta = xbox360perf::msSince(uploadStart);
        g_probe.uploadMs += uploadMsDelta;
        g_probe.uploadRing.add((float)uploadMsDelta);
        g_probe.uploadFrames++;
        // Publish the set we just filled and arm the next free slot.
        g_state.cur  = g_state.next;
        g_state.next = (g_state.next + 1) % RendererState::BUFFER_COUNT;
    }
    if (!g_state.plane[0][0]) return false;

    const std::uint64_t submitStart = xbox360perf::nowUsec();

    // The NanoVG rectangle lives in user space; map it through the transform
    // NanoVG is currently applying so scrolling/offset parents still work.
    float xform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    nvgCurrentTransform(vg, xform);
    const float cx[4] = {x, x, x + w, x + w};
    const float cy[4] = {y, y + h, y + h, y};
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float sx = xform[0] * cx[i] + xform[2] * cy[i] + xform[4];
        const float sy = xform[1] * cx[i] + xform[3] * cy[i] + xform[5];
        if (i == 0) {
            left = right = sx;
            top = bottom = sy;
        } else {
            if (sx < left) left = sx;
            if (sx > right) right = sx;
            if (sy < top) top = sy;
            if (sy > bottom) bottom = sy;
        }
    }

    const long targetW = (long)Application::windowWidth;
    const long targetH = (long)Application::windowHeight;
    long vpX = floorI(left);
    long vpY = floorI(top);
    long vpR = ceilI(right);
    long vpB = ceilI(bottom);
    if (vpX < 0) vpX = 0;
    if (vpY < 0) vpY = 0;
    if (vpR > targetW) vpR = targetW;
    if (vpB > targetH) vpB = targetH;
    if (vpR <= vpX || vpB <= vpY) return false;

    SavedState saved;
    saveState(device, saved);

    D3DVIEWPORT9 viewport{};
    viewport.X      = (DWORD)vpX;
    viewport.Y      = (DWORD)vpY;
    viewport.Width  = (DWORD)(vpR - vpX);
    viewport.Height = (DWORD)(vpB - vpY);
    viewport.MinZ   = 0.0f;
    viewport.MaxZ   = 1.0f;
    device->SetViewport(&viewport);

    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                       D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    const bool blended = alpha < 0.999f;
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, blended ? TRUE : FALSE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    device->SetVertexDeclaration(g_state.decl);
    device->SetStreamSource(0, g_state.vertices, 0, sizeof(YUVVertex));
    device->SetVertexShader(g_state.vertexShader);
    device->SetPixelShader(g_state.pixelShader);
    device->SetTexture(0, g_state.plane[g_state.cur][0]);
    device->SetTexture(1, g_state.plane[g_state.cur][1]);
    device->SetTexture(2, g_state.plane[g_state.cur][2]);
    for (int sampler = 0; sampler < 3; sampler++) {
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    // Limited-range matrices; the surface size decides which one mpv assumed,
    // mirroring mp_csp_guess_colorspace() (>= 1280 wide or > 576 tall => 709).
    const bool bt709 = width >= 1280 || height > 576;
    const float kY   = 1.164383f;
    float c0[4];
    float c1[4];
    if (bt709) {
        c0[0] = blended ? alpha : 1.0f;
        c0[1] = kY;
        c0[2] = 1.792741f;
        c0[3] = -0.213249f;
        c1[0] = -0.532909f;
        c1[1] = 2.112402f;
    } else {
        c0[0] = blended ? alpha : 1.0f;
        c0[1] = kY;
        c0[2] = 1.596027f;
        c0[3] = -0.391762f;
        c1[0] = -0.812968f;
        c1[1] = 2.017232f;
    }
    c1[2] = 0.0f;
    c1[3] = 0.0f;
    device->SetPixelShaderConstantF(0, c0, 1);
    device->SetPixelShaderConstantF(1, c1, 1);

    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

    restoreState(device, saved);

    const double submitMsDelta = xbox360perf::msSince(submitStart);
    g_probe.submitMs += submitMsDelta;
    g_probe.submitRing.add((float)submitMsDelta);
    g_probe.frames++;
    g_probe.flush();
    return true;
}

bool Xbox360YUVRenderer::drawTexture(NVGcontext* vg, void* texture, int width, int height, float x, float y, float w,
                                     float h, float alpha)
{
    if (!vg || !texture || width <= 0 || height <= 0 || w <= 0.0f || h <= 0.0f) return false;
    if (alpha <= 0.0f) return false;

    // Same GPU letterboxing as draw(): the XAV texture is the video's native
    // size while the view rect comes from the layout.
    {
        const float texAspect  = (float)width / (float)height;
        const float rectAspect = w / h;
        if (rectAspect > texAspect) {
            const float fitW = h * texAspect;
            x += (w - fitW) * 0.5f;
            w = fitW;
        } else if (rectAspect < texAspect) {
            const float fitH = w / texAspect;
            y += (h - fitH) * 0.5f;
            h = fitH;
        }
    }

    NVGparams* params = nvgInternalParams(vg);
    if (!params || !params->renderFlush) return false;

    auto* videoContext = (Xbox360VideoContext*)Application::getPlatform()->getVideoContext();
    if (!videoContext) return false;
    IDirect3DDevice9* device = videoContext->getDevice();
    if (!device) return false;

    if (!init(device)) return false;
    if (!g_state.rgbShader) return false;

    // Everything NanoVG queued so far goes on screen first, so the video lands
    // on top of the UI drawn before it.
    params->renderFlush(params->userPtr);

    const std::uint64_t submitStart = xbox360perf::nowUsec();

    float xform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    nvgCurrentTransform(vg, xform);
    const float cx[4] = {x, x, x + w, x + w};
    const float cy[4] = {y, y + h, y + h, y};
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    for (int i = 0; i < 4; i++) {
        const float sx = xform[0] * cx[i] + xform[2] * cy[i] + xform[4];
        const float sy = xform[1] * cx[i] + xform[3] * cy[i] + xform[5];
        if (i == 0) {
            left = right = sx;
            top = bottom = sy;
        } else {
            if (sx < left) left = sx;
            if (sx > right) right = sx;
            if (sy < top) top = sy;
            if (sy > bottom) bottom = sy;
        }
    }

    const long targetW = (long)Application::windowWidth;
    const long targetH = (long)Application::windowHeight;
    long vpX = floorI(left);
    long vpY = floorI(top);
    long vpR = ceilI(right);
    long vpB = ceilI(bottom);
    if (vpX < 0) vpX = 0;
    if (vpY < 0) vpY = 0;
    if (vpR > targetW) vpR = targetW;
    if (vpB > targetH) vpB = targetH;
    if (vpR <= vpX || vpB <= vpY) return false;

    SavedState saved;
    saveState(device, saved);

    D3DVIEWPORT9 viewport{};
    viewport.X      = (DWORD)vpX;
    viewport.Y      = (DWORD)vpY;
    viewport.Width  = (DWORD)(vpR - vpX);
    viewport.Height = (DWORD)(vpB - vpY);
    viewport.MinZ   = 0.0f;
    viewport.MaxZ   = 1.0f;
    device->SetViewport(&viewport);

    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                       D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    const bool blended = alpha < 0.999f;
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, blended ? TRUE : FALSE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    device->SetVertexDeclaration(g_state.decl);
    device->SetStreamSource(0, g_state.vertices, 0, sizeof(YUVVertex));
    device->SetVertexShader(g_state.vertexShader);
    device->SetPixelShader(g_state.rgbShader);
    device->SetTexture(0, (IDirect3DTexture9*)texture);
    device->SetTexture(1, nullptr);
    device->SetTexture(2, nullptr);
    for (int sampler = 0; sampler < 3; sampler++) {
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    const float a = blended ? alpha : 1.0f;
    float c0[4]   = {a, 0.0f, 0.0f, 0.0f};
    device->SetPixelShaderConstantF(0, c0, 1);

    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

    restoreState(device, saved);

    const double submitMsDelta = xbox360perf::msSince(submitStart);
    g_probe.submitMs += submitMsDelta;
    g_probe.submitRing.add((float)submitMsDelta);
    g_probe.frames++;
    g_probe.flush();
    return true;
}

void Xbox360YUVRenderer::release(void* device)
{
    if (device && g_state.device && g_state.device != (IDirect3DDevice9*)device) return;
    releaseObjects();
}
} // namespace brls

#endif
