// [stories] The one-process host's window (src/stories/host.cpp; docs/STORIES.md "One process"): Windows only.
//
// stories.exe owns ONE top-level window (the frame). Each game DLL in it makes its window a child of that frame, filling
// its client area, on the game's own main thread (so the game's SDL copy pumps it as always), and wraps it in SDL
// (platform_sdl3.cpp init). The game in front shows its child; the one behind hides its own. Switching games is then
// a child shown and another hidden inside the same top-level window: no foreground change, no lost focus. Full screen,
// resizing and closing belong to the frame (messages below). Without STORIES_HOST_HWND (every normal start) nothing here
// runs.
#include "platform/stories_child.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace platform::stories
{
    namespace
    {
        HWND g_frame = nullptr;
        HWND g_child = nullptr;
        bool g_read = false;

        HMODULE thisModule()
        {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&thisModule), &m);
            return m;
        }

        LRESULT CALLBACK childProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            if (msg == WM_ERASEBKGND)
                return 1; // the renderer draws all of it
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void *frame()
    {
        if (!g_read)
        {
            g_read = true;
            if (const char *h = std::getenv("STORIES_HOST_HWND"); h && *h)
            {
                const unsigned long long v = std::strtoull(h, nullptr, 16);
                g_frame = reinterpret_cast<HWND>(static_cast<uintptr_t>(v));
                if (g_frame && !IsWindow(g_frame))
                {
                    std::fprintf(stderr, "[stories] STORIES_HOST_HWND %s is no window: a window of our own\n", h);
                    g_frame = nullptr;
                }
            }
        }
        return g_frame;
    }

    void *module() { return thisModule(); }

    void *createChild(int &width, int &height)
    {
        if (!frame())
            return nullptr;
        HMODULE mod = thisModule();
        // the class per DLL (its own window procedure, in its own module): two game DLLs, two classes
        wchar_t cls[64];
        std::swprintf(cls, 64, L"GTAStoriesGame_%p", static_cast<void *>(mod));
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = &childProc;
        wc.hInstance = mod;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            std::fprintf(stderr, "[stories] RegisterClassEx: %lu\n", GetLastError());
            return nullptr;
        }
        RECT rc{};
        GetClientRect(g_frame, &rc);
        width = std::max<int>(1, rc.right - rc.left);
        height = std::max<int>(1, rc.bottom - rc.top);
        // not WS_VISIBLE: the game in front shows its own (showChild)
        g_child = CreateWindowExW(0, cls, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, width, height, g_frame,
                                  nullptr, mod, nullptr);
        if (!g_child)
            std::fprintf(stderr, "[stories] CreateWindowEx (child): %lu\n", GetLastError());
        return g_child;
    }

    void showChild(bool shown)
    {
        if (!g_child)
            return;
        if (!shown)
        {
            ShowWindow(g_child, SW_HIDE);
            return;
        }
        RECT rc{};
        GetClientRect(g_frame, &rc);
        // above the other game's child, never activating anything (the frame keeps the foreground it has, or not)
        SetWindowPos(g_child, HWND_TOP, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // the keyboard focus into this game, only while the frame is the foreground window (it never takes it)
        if (GetForegroundWindow() == g_frame)
            SetFocus(g_child);
        PostMessageW(g_frame, kMsgFrontChanged, 0, reinterpret_cast<LPARAM>(g_child));
    }

    bool frameVisible() { return g_frame && IsWindowVisible(g_frame); }

    bool frameForeground() { return g_frame && GetForegroundWindow() == g_frame && !IsIconic(g_frame); }

    bool rawMouseRegistered()
    {
        RAWINPUTDEVICE devs[16];
        UINT n = 16;
        const UINT got = GetRegisteredRawInputDevices(devs, &n, sizeof(RAWINPUTDEVICE));
        if (got == static_cast<UINT>(-1))
            return true; // cannot tell: assume yes (never a needless re-register loop)
        for (UINT i = 0; i < got; ++i)
            if (devs[i].usUsagePage == 0x01 && devs[i].usUsage == 0x02) // generic desktop / mouse
                return true;
        return false;
    }

    void requestFullscreen(bool on)
    {
        if (g_frame)
            PostMessageW(g_frame, kMsgFullscreen, on ? 1 : 0, 0);
    }
}
#else
namespace platform::stories
{
    void *frame() { return nullptr; }
    void *module() { return nullptr; }
    void *createChild(int &, int &) { return nullptr; }
    void showChild(bool) {}
    bool frameVisible() { return false; }
    bool frameForeground() { return false; }
    bool rawMouseRegistered() { return true; }
    void requestFullscreen(bool) {}
}
#endif
