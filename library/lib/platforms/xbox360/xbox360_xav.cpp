#include <borealis/platforms/xbox360/xbox360_xav.hpp>

#if defined(PLATFORM_XBOX360) && X360_XAV

#include <borealis/core/logger.hpp>

#include <xbox.h>

// XAV is an MSVC-mangled C++ API: xav.lib is a static XMF media pipeline built
// with Microsoft's compiler, while OXDK's clang emits Itanium names.  Calling
// the declarations straight out of <xav.h> leaves the linker with _Z13XAV...,
// which nothing in the XDK defines.  Take the types from <xav.h> (renaming the
// five entry points on the way in) and bind the real Microsoft symbols.
#define XAVInitialize XAVInitialize_decl
#define XAVShutdown XAVShutdown_decl
#define XAVPreRender XAVPreRender_decl
#define XAVPostRender XAVPostRender_decl
#define CreateXAVPlayer CreateXAVPlayer_decl
#include <xav.h>
#undef XAVInitialize
#undef XAVShutdown
#undef XAVPreRender
#undef XAVPostRender
#undef CreateXAVPlayer

extern "C" {
HRESULT XAVInitialize(const XAVInit* pInit, DWORD* affinities) asm("?XAVInitialize@@YAJPBUXAVInit@@QAK@Z");
HRESULT XAVShutdown() asm("?XAVShutdown@@YAJXZ");
HRESULT XAVPreRender() asm("?XAVPreRender@@YAJXZ");
HRESULT XAVPostRender() asm("?XAVPostRender@@YAJXZ");
HRESULT CreateXAVPlayer(IXAVPlayer** ppPlayer, IXAVPlayerEvents* pEvents, IXAudio2* pAudio2)
    asm("?CreateXAVPlayer@@YAJPAPAUIXAVPlayer@@PAUIXAVPlayerEvents@@PAUIXAudio2@@@Z");
}

#include <atomic>

namespace brls
{

namespace
{

// XAV calls its event callbacks from its own threads, so everything the
// callbacks touch has to be atomic.
struct SharedState
{
    std::atomic<std::uint64_t> position{ 0 }; // hundred-nanosecond units
    std::atomic<bool> fatalError{ false };
};

class XavEvents : public IXAVPlayerEvents
{
  public:
    explicit XavEvents(SharedState* shared) : state(shared) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IXAVPlayerEvents) || riid == __uuidof(IUnknown))
        {
            *ppv = static_cast<IXAVPlayerEvents*>(this);
            this->AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++this->refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --this->refs;
        if (n == 0) delete this;
        return n;
    }

    // IXAVPlayerEvents (declared without a calling convention in xav.h)
    void StatusChanged(const IXAVPlayerStatus* status) override { this->note(status); }
    void PositionUpdate(const IXAVPlayerStatus* status) override { this->note(status); }
    void Alert(XAVPlayerAlert* alert, const IXAVPlayerStatus* status) override
    {
        if (alert && alert->Level == XAVPlayerAlert::FatalError)
        {
            this->state->fatalError.store(true, std::memory_order_relaxed);
            brls::Logger::error("XAV fatal alert: component={} type={} hr=0x{:08x}", (int)alert->Component,
                                (int)alert->dwType, (unsigned int)alert->hrAlert);
        }
        this->note(status);
    }

  private:
    void note(const IXAVPlayerStatus* status)
    {
        if (status) this->state->position.store(status->GetPosition(), std::memory_order_relaxed);
    }

    std::atomic<ULONG> refs{ 1 };
    SharedState* state;
};

struct Impl
{
    IDirect3DDevice9* device = nullptr;
    const D3DPRESENT_PARAMETERS* pp = nullptr;
    bool globalReady = false;
    bool globalFailed = false;

    SharedState shared;
    IXAVPlayer* player = nullptr;
    IXAVPlayerGetTexture* frameInfo = nullptr;
    IDirect3DTexture9* frame = nullptr;
    bool muted = true;
};

Impl* xavImpl()
{
    static Impl s;
    return &s;
}

// One event sink for the whole process. XAV's Shutdown() is asynchronous and a
// callback could still be in flight right after it, so the sink outlives every
// player instead of being destroyed with one.
IXAVPlayerEvents* eventSink(SharedState* shared)
{
    static IXAVPlayerEvents* sink = new XavEvents(shared);
    return sink;
}

void setBool(IXAVPropertyStore* props, XAVPropertyKey key, BOOL value)
{
    XAVPropertyValue v;
    v.m_Type         = XAVPropertyValue::BOOLProperty;
    v.m_Value.fValue = value;
    const HRESULT hr = props->SetValue((DWORD)key, &v);
    if (FAILED(hr))
        brls::Logger::warning("XAV property {} = {} failed: 0x{:08x}", (int)key, (int)value, (unsigned int)hr);
}

void destroyPlayer(Impl* p)
{
    if (p->frame)
    {
        p->frame->Release();
        p->frame = nullptr;
    }
    if (p->frameInfo)
    {
        p->frameInfo->Release();
        p->frameInfo = nullptr;
    }
    if (p->player)
    {
        // Shutdown() breaks the player <-> events circular reference.
        p->player->Shutdown();
        p->player->Release();
        p->player = nullptr;
    }
}

} // namespace

Xbox360Xav& Xbox360Xav::instance()
{
    static Xbox360Xav s;
    return s;
}

bool Xbox360Xav::init(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* presentParams)
{
    Impl* p = xavImpl();
    if (p->globalReady) return true;
    if (p->globalFailed || !device || !presentParams) return false;

    XAVInit initInfo{};
    initInfo.pDevice       = device;
    initInfo.pPresentParam = presentParams;
    // FALSE keeps video colour conversion exact; the UI is drawn by NanoVG
    // either way and is unaffected.
    initInfo.fGameMode = FALSE;

    DWORD affinities[XAVMaxThreadTypes];
    for (int i = 0; i < XAVMaxThreadTypes; i++) affinities[i] = c_dwUseDefaultAffinity;

    const HRESULT hr = XAVInitialize(&initInfo, affinities);
    if (FAILED(hr))
    {
        p->globalFailed = true;
        brls::Logger::error("XAVInitialize failed: 0x{:08x}", (unsigned int)hr);
        return false;
    }
    p->device      = device;
    p->pp          = presentParams;
    p->globalReady = true;
    brls::Logger::info("XAV initialized (experimental hardware decode)");
    return true;
}

bool Xbox360Xav::initialized() const { return xavImpl()->globalReady; }

bool Xbox360Xav::open(const std::string& url)
{
    Impl* p = xavImpl();
    if (!p->globalReady || url.empty()) return false;

    // Re-opening reuses the player; only build it the first time.
    if (!p->player)
    {
        const HRESULT hr = CreateXAVPlayer(&p->player, eventSink(&p->shared), nullptr);
        if (FAILED(hr) || !p->player)
        {
            brls::Logger::error("CreateXAVPlayer failed: 0x{:08x}", (unsigned int)hr);
            destroyPlayer(p);
            return false;
        }

        IXAVPropertyStore* props = nullptr;
        if (SUCCEEDED(p->player->GetPlayerPropertyStore(&props)) && props)
        {
            setBool(props, XAVOpenParamAutoPlay, TRUE);
            // Do NOT pop the OS "download the Optional Media Update" UI: if the
            // codec pack is missing we want Open() to fail fast so the caller
            // can keep using the software path.
            setBool(props, XAVOpenParamAutoDownloadCodecPack, FALSE);
            props->Release();
        }

        if (FAILED(p->player->GetPlayerFrameInfo(&p->frameInfo)) || !p->frameInfo)
        {
            brls::Logger::error("XAV GetPlayerFrameInfo failed");
            destroyPlayer(p);
            return false;
        }

        if (p->muted) p->player->Mute(TRUE);
    }

    p->shared.fatalError.store(false, std::memory_order_relaxed);
    const HRESULT hr = p->player->Open(url.c_str(), c_ullDefaultStartPosition);
    if (FAILED(hr))
    {
        // Most likely the Optional Media Update codec pack is missing.
        brls::Logger::error("XAV Open failed: 0x{:08x} (codec pack missing?)", (unsigned int)hr);
        return false;
    }
    brls::Logger::info("XAV opened: {}", url);
    return true;
}

void Xbox360Xav::close() { destroyPlayer(xavImpl()); }

void Xbox360Xav::shutdown()
{
    Impl* p = xavImpl();
    destroyPlayer(p);
    if (p->globalReady)
    {
        p->globalReady = false;
        const HRESULT hr = XAVShutdown();
        if (FAILED(hr)) brls::Logger::warning("XAVShutdown failed: 0x{:08x}", (unsigned int)hr);
    }
}

bool Xbox360Xav::isOpen() const { return xavImpl()->player != nullptr; }

void Xbox360Xav::setPaused(bool paused)
{
    Impl* p = xavImpl();
    if (!p->player) return;
    if (paused)
        p->player->Pause();
    else
        p->player->Play();
}

void Xbox360Xav::seekAbsolute(double seconds)
{
    Impl* p = xavImpl();
    if (!p->player || seconds < 0) return;
    p->player->SeekAbsolute((ULONGLONG)(seconds * 10000000.0)); // hundred-ns units
}

void Xbox360Xav::setMuted(bool muted)
{
    Impl* p = xavImpl();
    p->muted = muted;
    if (p->player) p->player->Mute(muted ? TRUE : FALSE);
}

void Xbox360Xav::postRender()
{
    if (xavImpl()->globalReady) XAVPostRender();
}

void Xbox360Xav::preRender()
{
    if (xavImpl()->globalReady) XAVPreRender();
}

IDirect3DTexture9* Xbox360Xav::acquireFrame(int& width, int& height)
{
    Impl* p = xavImpl();
    if (!p->frameInfo) return nullptr;

    if (p->frame)
    {
        p->frame->Release();
        p->frame = nullptr;
    }

    IDirect3DTexture9* tex = nullptr;
    XAVRenderResultInfo info{};
    // fCopyTexture = FALSE: the frame is drawn within this render loop cycle
    // and released again, so XAV may keep reusing the same texture.
    const HRESULT hr = p->frameInfo->GetTexture(FALSE, &tex, &info);
    if (FAILED(hr) || !tex) return nullptr;

    p->frame = tex;

    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(tex->GetLevelDesc(0, &desc)))
    {
        width  = (int)desc.Width;
        height = (int)desc.Height;
    }
    return tex;
}

void Xbox360Xav::releaseFrame()
{
    Impl* p = xavImpl();
    if (p->frame)
    {
        p->frame->Release();
        p->frame = nullptr;
    }
}

double Xbox360Xav::positionSeconds() const
{
    return (double)xavImpl()->shared.position.load(std::memory_order_relaxed) / 10000000.0;
}

bool Xbox360Xav::failed() const { return xavImpl()->shared.fatalError.load(std::memory_order_relaxed); }

} // namespace brls

#endif
