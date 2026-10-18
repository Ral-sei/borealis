// Xbox 360 text input.
//
// The console has no system software keyboard reachable through the XDK the way
// the other platforms do, and wiliwili does not ship an on-screen keyboard of
// its own yet, so there is nothing to open. Every call site treats a `false`
// return as "user cancelled" and silently does nothing, which left search,
// comments and chat looking broken with no feedback at all — so tell the user
// why instead, right here rather than at each of the eight entry points.
//
// The callbacks are intentionally never invoked: callers free their IME state
// when the callback fires, so firing it would look like a successful-but-empty
// submission.

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/platforms/xbox360/xbox360_ime.hpp>

#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)
#include <windows.h>
#include <xbox.h>
#include <thread>
#endif

#include <cstdlib>
#include <string>

namespace brls {

using namespace literals;

#if defined(__XBOX360__) || defined(PLATFORM_XBOX360)

namespace {

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string wideToUtf8(const wchar_t* w)
{
    if (!w || !*w)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0')
        s.pop_back(); // drop the terminator counted/written by -1
    return s;
}

// One keyboard at a time: XShowKeyboardUI refuses (or misbehaves) if a Guide
// overlay is already up, and a second call would leak a thread waiting forever.
static bool s_keyboardOpen = false;

// dwUserIndex must reference a signed-in gamer (XShowKeyboardUI) — using a
// fixed 0 silently fails whenever no profile sits in slot 0.
DWORD pickUserIndex()
{
    for (DWORD i = 0; i < 4; i++)
    {
        XUSER_SIGNIN_STATE st = XUserGetSigninState(i);
        if (st == eXUserSigninState_SignedInLocally || st == eXUserSigninState_SignedInToLive)
            return i;
    }
    return 0; // no profile signed in; let XShowKeyboardUI report its own error
}

// Run the Guide keyboard on a worker thread: XShowKeyboardUI is overlapped,
// and blocking on its event keeps the caller's later-callback contract via
// Threading::sync (borealis is not thread-safe) instead of deadlocking the
// main thread or never firing.
void runKeyboard(DWORD flags, std::string header, std::string sub, std::string initial,
    std::function<void(std::string)> done)
{
    std::thread([flags, header, sub, initial, done]() {
        std::wstring wHeader  = utf8ToWide(header);
        std::wstring wSub     = utf8ToWide(sub);
        std::wstring wInitial = utf8ToWide(initial);

        wchar_t result[512] = { 0 };
        XOVERLAPPED ov{};
        HANDLE hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        ov.hEvent     = hEvent;

        DWORD userIndex = pickUserIndex();
        DWORD rc        = XShowKeyboardUI(userIndex, flags, wInitial.empty() ? nullptr : wInitial.c_str(),
            wHeader.empty() ? nullptr : wHeader.c_str(),
            wSub.empty() ? nullptr : wSub.c_str(),
            result, (DWORD)(sizeof(result) / sizeof(result[0])), &ov);

        bool ok        = false;
        bool cancelled = false;
        if (rc == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(hEvent, INFINITE) == WAIT_OBJECT_0)
            {
                ok        = (ov.dwExtendedError == ERROR_SUCCESS);
                cancelled = (ov.dwExtendedError == ERROR_CANCELLED);
            }
            else
            {
                Logger::error("xbox360 ime: wait failed, GetLastError={}", GetLastError());
            }
        }
        else if (rc == ERROR_SUCCESS)
        {
            ok = true;
        }

        if (!ok && !cancelled)
            Logger::error("xbox360 ime: XShowKeyboardUI failed, user={} rc={} extended={}",
                userIndex, rc, ov.dwExtendedError);

        if (hEvent)
            CloseHandle(hEvent);

        std::string text = ok ? wideToUtf8(result) : std::string();
        Threading::sync([done, text, ok, cancelled]() {
            s_keyboardOpen = false;
            if (ok)
                done(text);
            else if (cancelled)
                done(""); // treated as cancel by callers that check empty
            else
                Application::notify("wiliwili/dialog/ime_unavailable"_i18n); // fail visibly
        });
    }).detach();
}

} // namespace

bool Xbox360ImeManager::openForText(std::function<void(std::string)> f, std::string headerText,
    std::string subText, int maxStringLength, std::string initialText,
    int kbdDisableBitmask)
{
    (void)maxStringLength;
    (void)kbdDisableBitmask;
    runKeyboard(VKBD_DEFAULT, headerText, subText, initialText,
        [f](std::string text) {
            if (!text.empty())
                f(text);
        });
    return true;
}

bool Xbox360ImeManager::openForNumber(std::function<void(long)> f, std::string headerText,
    std::string subText, int maxStringLength, std::string initialText,
    std::string, std::string, int kbdDisableBitmask)
{
    (void)maxStringLength;
    (void)kbdDisableBitmask;
    runKeyboard(VKBD_LATIN_NUMERIC, headerText, subText, initialText,
        [f](std::string text) {
            if (text.empty())
                return;
            char* end = nullptr;
            long v    = std::strtol(text.c_str(), &end, 10);
            if (end && *end == '\0')
                f(v);
            else
                Logger::error("xbox360 ime: not a number: {}", text);
        });
    return true;
}

#else

namespace {
void notifyUnavailable()
{
    Application::notify("wiliwili/dialog/ime_unavailable"_i18n);
}
} // namespace

bool Xbox360ImeManager::openForText(std::function<void(std::string)>, std::string, std::string, int, std::string, int)
{
    notifyUnavailable();
    return false;
}

bool Xbox360ImeManager::openForNumber(std::function<void(long)>, std::string, std::string, int, std::string, std::string, std::string, int)
{
    notifyUnavailable();
    return false;
}

#endif

} // namespace brls
