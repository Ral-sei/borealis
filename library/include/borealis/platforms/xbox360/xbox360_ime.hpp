#pragma once

#include <borealis/core/ime.hpp>

namespace brls
{

class Xbox360ImeManager : public ImeManager
{
  public:
    bool openForText(std::function<void(std::string)> f, std::string headerText = "",
        std::string subText = "", int maxStringLength = 32, std::string initialText = "",
        int kbdDisableBitmask = KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NONE) override;
    bool openForNumber(std::function<void(long)> f, std::string headerText = "",
        std::string subText = "", int maxStringLength = 18, std::string initialText = "",
        std::string leftButton = "", std::string rightButton = "",
        int kbdDisableBitmask = KeyboardKeyDisableBitmask::KEYBOARD_DISABLE_NONE) override;
};

} // namespace brls
