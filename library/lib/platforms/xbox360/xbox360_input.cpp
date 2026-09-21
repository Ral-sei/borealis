#include <borealis/platforms/xbox360/xbox360_input.hpp>
#include <cstring>
#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
#include <windows.h>
#include <xbox.h>
#endif
namespace brls {
short Xbox360InputManager::getControllersConnectedCount() {
#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
    XINPUT_STATE state{};
    return XInputGetState(0, &state) == ERROR_SUCCESS ? 1 : 0;
#else
    return 0;
#endif
}
void Xbox360InputManager::updateUnifiedControllerState(ControllerState* s) {
    std::memset(s, 0, sizeof(*s));
    if (getControllersConnectedCount() > 0) updateControllerState(s, 0);
}
void Xbox360InputManager::updateControllerState(ControllerState* s, int controller) {
    std::memset(s, 0, sizeof(*s));
#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
    XINPUT_STATE state{};
    if (controller < 0 || controller >= GAMEPADS_MAX || XInputGetState((DWORD)controller, &state) != ERROR_SUCCESS) return;
    const XINPUT_GAMEPAD& p = state.Gamepad;
    const WORD b = p.wButtons;
    s->buttons[BUTTON_UP] = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
    s->buttons[BUTTON_RIGHT] = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    s->buttons[BUTTON_DOWN] = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    s->buttons[BUTTON_LEFT] = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    s->buttons[BUTTON_BACK] = (b & XINPUT_GAMEPAD_BACK) != 0;
    s->buttons[BUTTON_START] = (b & XINPUT_GAMEPAD_START) != 0;
    s->buttons[BUTTON_LB] = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    s->buttons[BUTTON_RB] = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    s->buttons[BUTTON_LSB] = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    s->buttons[BUTTON_RSB] = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    s->buttons[BUTTON_A] = (b & XINPUT_GAMEPAD_A) != 0;
    s->buttons[BUTTON_B] = (b & XINPUT_GAMEPAD_B) != 0;
    s->buttons[BUTTON_X] = (b & XINPUT_GAMEPAD_X) != 0;
    s->buttons[BUTTON_Y] = (b & XINPUT_GAMEPAD_Y) != 0;
    s->buttons[BUTTON_LT] = p.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    s->buttons[BUTTON_RT] = p.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    s->axes[LEFT_X] = p.sThumbLX / 32767.0f; s->axes[LEFT_Y] = -p.sThumbLY / 32767.0f;
    s->axes[RIGHT_X] = p.sThumbRX / 32767.0f; s->axes[RIGHT_Y] = -p.sThumbRY / 32767.0f;
    s->axes[LEFT_Z] = p.bLeftTrigger / 255.0f; s->axes[RIGHT_Z] = p.bRightTrigger / 255.0f;
#else
    (void)controller;
#endif
}
bool Xbox360InputManager::getKeyboardKeyState(BrlsKeyboardScancode) { return false; }
void Xbox360InputManager::updateTouchStates(std::vector<RawTouchState>* s) { s->clear(); }
void Xbox360InputManager::updateMouseStates(RawMouseState* s) { if (s) *s = {}; }
void Xbox360InputManager::sendRumble(unsigned short controller, unsigned short low, unsigned short high) {
#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
    XINPUT_VIBRATION vibration{}; vibration.wLeftMotorSpeed = low; vibration.wRightMotorSpeed = high;
    XInputSetState(controller, &vibration);
#else
    (void)controller; (void)low; (void)high;
#endif
}
}
