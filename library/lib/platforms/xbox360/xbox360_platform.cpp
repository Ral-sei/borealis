#include <borealis/platforms/xbox360/xbox360_platform.hpp>
#include <borealis/core/application.hpp>
namespace brls {
Xbox360Platform::Xbox360Platform() : audio(new NullAudioPlayer()), input(new Xbox360InputManager()), ime(new Xbox360ImeManager()), font(new Xbox360FontLoader()) {}
Xbox360Platform::~Xbox360Platform() { delete video; delete input; delete ime; delete font; delete audio; }
void Xbox360Platform::createWindow(std::string, uint32_t w, uint32_t h, float, float) {
    delete video;
    video = new Xbox360VideoContext(w ? w : 1280, h ? h : 720);
}
std::string Xbox360Platform::getName() { return "Xbox 360"; }
bool Xbox360Platform::mainLoopIteration() {
    // XInput is polled by Application::processInput; this hook is intentionally
    // side-effect free and keeps the title alive until Application::quit().
    return true;
}
ThemeVariant Xbox360Platform::getThemeVariant() { return theme; }
void Xbox360Platform::setThemeVariant(ThemeVariant t) { theme = t; }
std::string Xbox360Platform::getLocale() { return LOCALE_DEFAULT; }
AudioPlayer* Xbox360Platform::getAudioPlayer() { return audio; }
VideoContext* Xbox360Platform::getVideoContext() { return video; }
InputManager* Xbox360Platform::getInputManager() { return input; }
ImeManager* Xbox360Platform::getImeManager() { return ime; }
FontLoader* Xbox360Platform::getFontLoader() { return font; }
bool Xbox360Platform::canShowBatteryLevel() { return false; } int Xbox360Platform::getBatteryLevel() { return 0; } bool Xbox360Platform::isBatteryCharging() { return false; }
bool Xbox360Platform::canShowWirelessLevel() { return false; } bool Xbox360Platform::hasWirelessConnection() { return false; } bool Xbox360Platform::hasEthernetConnection() { return true; } int Xbox360Platform::getWirelessLevel() { return 0; }
std::string Xbox360Platform::getIpAddress() { return "-"; } std::string Xbox360Platform::getDnsServer() { return "-"; }
void Xbox360Platform::disableScreenDimming(bool d, const std::string&, const std::string&) { dimmingDisabled = d; } bool Xbox360Platform::isScreenDimmingDisabled() { return dimmingDisabled; }
void Xbox360Platform::setBacklightBrightness(float) {} float Xbox360Platform::getBacklightBrightness() { return 1.0f; } bool Xbox360Platform::canSetBacklightBrightness() { return false; }
bool Xbox360Platform::isApplicationMode() { return true; } void Xbox360Platform::exitToHomeMode(bool) {} void Xbox360Platform::forceEnableGamePlayRecording() {} void Xbox360Platform::openBrowser(std::string) {}
}
