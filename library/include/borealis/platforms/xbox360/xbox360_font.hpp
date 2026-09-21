#pragma once

#include <borealis/core/font.hpp>

namespace brls
{

class Xbox360FontLoader : public FontLoader
{
  public:
    void loadFonts() override;
};

} // namespace brls
