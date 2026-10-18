//
// Xbox 360 XAV (Xbox Audio/Video) hardware-decode player wrapper.
//
// This is the experimental path behind Settings > Playback > Hardware decode
// (MPVCore::HARDWARE_DEC).  XAV is public XDK API (xav.h / xav.lib) and is the
// same engine the official YouTube client uses (CreateXAVPlayer /
// IXAVPlayerGetTexture).  mpv stays in charge of demux + audio (loaded with
// vid=no); XAV only decodes the video URL and hands back one
// IDirect3DTexture9 per frame, which the renderer draws as a single quad.
//
// Limits to keep in mind:
//  * XAV's Open() takes a plain URL and carries no custom HTTP headers, so a
//    Bilibili CDN URL (which needs Referer) will not load without a local
//    header-injecting proxy.  A URL that needs no special headers is the way
//    to validate the pipeline itself.
//  * Non-WMV/WMA content needs the system "Optional Media Update" codec pack.
//    If it is missing, Open() fails; the caller reports it and keeps going
//    with the software path.
//
#pragma once

#if defined(PLATFORM_XBOX360) && X360_XAV

// The XDK d3d9.h pulls in xbox.h, which needs the Win32 base typedefs first
// (same ordering as xbox360_video.hpp).
#include <windows.h>

#include <d3d9.h>
#include <string>

namespace brls
{

class Xbox360Xav
{
  public:
    static Xbox360Xav& instance();

    /// One-time global init. Needs the app's D3D device and the
    /// D3DPRESENT_PARAMETERS it was created with. Safe to call repeatedly;
    /// returns false when XAVInitialize failed.
    bool init(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* presentParams);
    bool initialized() const;

    /// Open (or re-open) a URL. False when XAV is unavailable or Open failed.
    bool open(const std::string& url);
    void close();
    bool isOpen() const;

    /// close() plus XAVShutdown(); call once, before the D3D device goes away.
    void shutdown();

    void setPaused(bool paused);
    void seekAbsolute(double seconds);
    void setMuted(bool muted);

    /// Present-loop contract: postRender() right before Swap, preRender()
    /// right after Swap. No-ops while XAV is not initialized.
    void postRender();
    void preRender();

    /// Current video frame. Returns nullptr when no frame is ready; the
    /// returned texture must be handed back with releaseFrame() in the same
    /// frame.
    IDirect3DTexture9* acquireFrame(int& width, int& height);
    void releaseFrame();

    /// Last position/state seen by the XAV event callbacks.
    double positionSeconds() const;
    bool failed() const;

  private:
    Xbox360Xav()  = default;
    ~Xbox360Xav() = default;
    Xbox360Xav(const Xbox360Xav&)            = delete;
    Xbox360Xav& operator=(const Xbox360Xav&) = delete;

    void* impl = nullptr; // Impl*, defined in xbox360_xav.cpp
};

} // namespace brls

#endif
