#pragma once

#if defined(PLATFORM_XBOX360)
#include <windows.h>
#endif

#include <borealis/core/video.hpp>
#include <d3d9.h>

namespace brls
{

class Xbox360VideoContext : public VideoContext
{
  public:
    Xbox360VideoContext(uint32_t width, uint32_t height);
    ~Xbox360VideoContext() override;

    void clear(NVGcolor color) override;
    void beginFrame() override;
    void endFrame() override;
    void resetState() override;
    double getScaleFactor() override;
    NVGcontext* getNVGContext() override;
    void setSwapInterval(int interval) override;

    IDirect3DDevice9* getDevice() const;

  private:
    IDirect3DDevice9* device = nullptr;
    NVGcontext* vg = nullptr;
    uint32_t width;
    uint32_t height;
};

} // namespace brls
