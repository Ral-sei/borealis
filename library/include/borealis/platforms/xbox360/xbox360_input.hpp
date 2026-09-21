#pragma once

#include <borealis/core/input.hpp>

namespace brls
{

class Xbox360InputManager : public InputManager
{
  public:
    short getControllersConnectedCount() override;
    void updateUnifiedControllerState(ControllerState* state) override;
    void updateControllerState(ControllerState* state, int controller) override;
    bool getKeyboardKeyState(BrlsKeyboardScancode key) override;
    void updateTouchStates(std::vector<RawTouchState>* states) override;
    void updateMouseStates(RawMouseState* state) override;
    void sendRumble(unsigned short controller, unsigned short lowFreqMotor, unsigned short highFreqMotor) override;
};

} // namespace brls
