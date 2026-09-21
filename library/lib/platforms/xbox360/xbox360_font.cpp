#include <borealis/platforms/xbox360/xbox360_font.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
namespace brls {
void Xbox360FontLoader::loadFonts() {
    auto* vg = Application::getNVGContext();
    const bool regular = loadFontFromFile(FONT_REGULAR, USER_FONT_PATH) ||
                         loadFontFromFile(FONT_REGULAR, BRLS_ASSET("font/switch_font.ttf"));
    if (!regular) Logger::warning("xbox360: no regular font available");
    if (loadFontFromFile(FONT_SWITCH_ICONS, USER_ICON_PATH))
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_SWITCH_ICONS));
    if (loadMaterialFromResources())
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_MATERIAL_ICONS));
    if (!USER_EMOJI_PATH.empty() && loadFontFromFile(FONT_EMOJI, USER_EMOJI_PATH))
        nvgAddFallbackFontId(vg, Application::getFont(FONT_REGULAR), Application::getFont(FONT_EMOJI));
}
}
