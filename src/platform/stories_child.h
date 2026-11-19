// [stories] The one-process host's window (stories_child_win32.cpp; src/stories/host.cpp): a game DLL's window as a
// child of the host's one top-level window. All of it is inactive unless the host set STORIES_HOST_HWND.
#pragma once

namespace platform::stories
{
    // messages the frame (stories.exe) understands, posted by a game
    constexpr unsigned kMsgFullscreen = 0x8000u + 0x51u;   // WM_APP + 0x51: wParam 1 full screen, 0 windowed
    constexpr unsigned kMsgFrontChanged = 0x8000u + 0x52u; // WM_APP + 0x52: lParam the child now shown (focus goes there)

    void *frame();  // the host's top-level window (HWND), null when not hosted
    void *module(); // this DLL (HMODULE)
    // the game's window: a child of the frame, its client size (not shown yet)
    void *createChild(int &width, int &height);
    void showChild(bool shown);
    bool frameVisible();
    // the frame is the foreground window and not minimized: the game in front then has the keyboard and the mouse
    // (SDL never sees a child window as focused: its WIN_UpdateFocus compares GetForegroundWindow() with the child)
    bool frameForeground();
    // this process has a raw-input mouse registration (RegisterRawInputDevices is per process: the other game's SDL
    // removing its own takes ours away too)
    bool rawMouseRegistered();
    void requestFullscreen(bool on);
}
