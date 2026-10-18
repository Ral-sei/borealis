#include <borealis/platforms/xbox360/xbox360_input.hpp>
#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
#include <windows.h>
#include <xbox.h>
#endif
namespace brls {

namespace {

float normalizeThumb(int16_t value, int16_t deadzone) {
    const float magnitude = static_cast<float>(value);
    const float sign = magnitude < 0.0f ? -1.0f : 1.0f;
    const float absolute = magnitude < 0.0f ? -magnitude : magnitude;
    if (absolute <= static_cast<float>(deadzone))
        return 0.0f;

    // Keep the dead zone while mapping the remaining range back to [-1, 1].
    const float normalized = (absolute - static_cast<float>(deadzone)) /
                             (32767.0f - static_cast<float>(deadzone));
    return sign * (normalized > 1.0f ? 1.0f : normalized);
}

// Clear only the polled fields.  ControllerState::repeatingButtonStop[] is
// owned by Application::processInput() (long-press / repeat deadlines): it is
// armed on button-down and consumed while the button stays held.  Wiping the
// whole struct (memset) reset every deadline to 0 on each frame, so the repeat
// condition could never become true again after the initial press — holding a
// direction moved exactly one step and never accelerated.  Every other platform
// driver (GLFW, SDL, PSV, Switch) clears only buttons/axes for the same reason.
void clearPolledState(ControllerState* s) {
    for (bool& button : s->buttons) button = false;
    for (float& axe : s->axes) axe = 0;
}

} // namespace

short Xbox360InputManager::getControllersConnectedCount() {
#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
    XINPUT_STATE state{};
    return XInputGetState(0, &state) == ERROR_SUCCESS ? 1 : 0;
#else
    return 0;
#endif
}
void Xbox360InputManager::updateUnifiedControllerState(ControllerState* s) {
    // updateControllerState() already clears the polled fields and leaves the
    // state cleared when controller 0 is absent, so probing
    // getControllersConnectedCount() first would only spend a second
    // XInputGetState() on the same pad every frame.
    updateControllerState(s, 0);
}
void Xbox360InputManager::updateControllerState(ControllerState* s, int controller) {
    clearPolledState(s);
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
    s->buttons[BUTTON_GUIDE] = (b & XINPUT_GAMEPAD_BIGBUTTON) != 0;
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
    s->axes[LEFT_X] = normalizeThumb(p.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    s->axes[LEFT_Y] = -normalizeThumb(p.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    s->axes[RIGHT_X] = normalizeThumb(p.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    s->axes[RIGHT_Y] = -normalizeThumb(p.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    s->axes[LEFT_Z] = p.bLeftTrigger / 255.0f; s->axes[RIGHT_Z] = p.bRightTrigger / 255.0f;

    // borealis dispatches focus navigation through the BUTTON_NAV_* entries;
    // the raw BUTTON_UP/DOWN/LEFT/RIGHT entries are not consumed by the
    // application navigation path on their own.
    s->buttons[BUTTON_NAV_UP] = s->buttons[BUTTON_UP] || s->axes[LEFT_Y] < -0.5f || s->axes[RIGHT_Y] < -0.5f;
    s->buttons[BUTTON_NAV_RIGHT] = s->buttons[BUTTON_RIGHT] || s->axes[LEFT_X] > 0.5f || s->axes[RIGHT_X] > 0.5f;
    s->buttons[BUTTON_NAV_DOWN] = s->buttons[BUTTON_DOWN] || s->axes[LEFT_Y] > 0.5f || s->axes[RIGHT_Y] > 0.5f;
    s->buttons[BUTTON_NAV_LEFT] = s->buttons[BUTTON_LEFT] || s->axes[LEFT_X] < -0.5f || s->axes[RIGHT_X] < -0.5f;
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
