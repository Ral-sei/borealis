#pragma once

#include <borealis/core/platform.hpp>
#include <borealis/platforms/xbox360/xbox360_font.hpp>
#include <borealis/platforms/xbox360/xbox360_ime.hpp>
#include <borealis/platforms/xbox360/xbox360_input.hpp>
#include <borealis/platforms/xbox360/xbox360_video.hpp>

namespace brls
{

class Xbox360Platform : public Platform
{
  public:
    Xbox360Platform();
    ~Xbox360Platform() override;

    void createWindow(std::string title, uint32_t width, uint32_t height, float x, float y) override;
    std::string getName() override;
    bool mainLoopIteration() override;
    ThemeVariant getThemeVariant() override;
    void setThemeVariant(ThemeVariant theme) override;
    std::string getLocale() override;
    AudioPlayer* getAudioPlayer() override;
    VideoContext* getVideoContext() override;
    InputManager* getInputManager() override;
    ImeManager* getImeManager() override;
    FontLoader* getFontLoader() override;
    bool canShowBatteryLevel() override;
    int getBatteryLevel() override;
    bool isBatteryCharging() override;
    bool canShowWirelessLevel() override;
    bool hasWirelessConnection() override;
    bool hasEthernetConnection() override;
    int getWirelessLevel() override;
    std::string getIpAddress() override;
    std::string getDnsServer() override;
    void disableScreenDimming(bool disable, const std::string& reason, const std::string& app) override;
    bool isScreenDimmingDisabled() override;
    void setBacklightBrightness(float brightness) override;
    float getBacklightBrightness() override;
    bool canSetBacklightBrightness() override;
    bool isApplicationMode() override;
    void exitToHomeMode(bool value) override;
    void forceEnableGamePlayRecording() override;
    void openBrowser(std::string url) override;

  private:
    ThemeVariant theme = ThemeVariant::DARK;
    bool dimmingDisabled = false;
    NullAudioPlayer* audio = nullptr;
    Xbox360VideoContext* video = nullptr;
    Xbox360InputManager* input = nullptr;
    Xbox360ImeManager* ime = nullptr;
    Xbox360FontLoader* font = nullptr;
};

} // namespace brls
