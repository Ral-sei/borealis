//
// Created for the Xbox 360 port.
//
// GPU colour conversion for planar YUV420 frames produced by libmpv's software
// renderer (MPV_RENDER_API_TYPE_SW with sw_format "yuv420p").  The CPU only
// copies planes into three L8 textures; the YUV -> RGB matrix runs in a pixel
// shader, which is far cheaper on Xenon than swscale's YUV -> RGBA conversion.
//
// This deliberately does not own the swap chain: the draw happens inside the
// NanoVG frame opened by Application::frame(), so borealis stays in control of
// Present().
//

#pragma once

#if defined(PLATFORM_XBOX360)

#include <cstddef>

struct NVGcontext;

namespace brls
{

class Xbox360YUVRenderer
{
  public:
    // Uploads `frame` (if non-null) and draws the video into the NanoVG-space
    // rectangle [x, y, x+w, y+h] using the current NanoVG transform.
    //
    // `frame` points at a contiguous I420 buffer laid out as
    //     [Y: pitch * height] [U: pitch/2 * ceil(height/2)] [V: pitch/2 * ceil(height/2)]
    // and `pitch` must be even and >= width.  Passing nullptr reuses the
    // previously uploaded textures, which is what you want while no new mpv
    // frame has arrived.
    //
    // Must be called from inside an active NanoVG frame.  Everything NanoVG has
    // queued so far is flushed first, so the video lands on top of the UI drawn
    // before it; anything drawn afterwards is flushed on top of the video when
    // Application::frame ends the frame.  Returns false when nothing was drawn
    // (no device, no shaders, no frame yet, or degenerate geometry).
    static bool draw(NVGcontext* vg, const void* frame, int width, int height, std::size_t pitch, float x, float y,
                     float w, float h, float alpha);

    // Draws an already-decoded RGB(A) D3D texture (the XAV hardware-decode
    // path) into the same rectangle, with the same letterboxing and NanoVG
    // flush rules as draw().  `texture` is borrowed and must stay valid for the
    // duration of the call.  Returns false when nothing was drawn.
    static bool drawTexture(NVGcontext* vg, void* texture, int width, int height, float x, float y, float w, float h,
                            float alpha);

    // Drops every cached object that belongs to `device`.  Call this before the
    // device is destroyed; a null device drops everything unconditionally.
    static void release(void* device);
};

} // namespace brls

#endif
