// [stories] stories.exe: both cities in ONE process and ONE window (docs/STORIES.md "One process"; experimental, Windows).
//
// Loads the two games' DLLs (lcs_game.dll, vcs_game.dll: tools/build.py --dll, one per game's build folder) and runs each
// game's main on a thread of its own. A DLL image has its own copy of everything static - the runtime, the renderer
// and its D3D11 device, SDL, the settings, the generated code - so the two games are as separate as two processes,
// except for what a process shares, handled here:
//  - the environment: each game reads its own (src/stories/game_dll.cpp's overlay): the pairs below plus --env;
//  - the window: this exe owns the one top-level window (the frame); each game's window is a child filling it
//    (src/platform/stories_child_win32.cpp), shown by the game in front, hidden by the one behind. The front switch
//    at the border (src/game/dualworld.cpp, unchanged: its shared block works inside one process too) is a child shown
//    and another hidden: no foreground change, no focus lost;
//  - quitting: closing the frame closes both games; when a game ends the process ends (a game leaves through _Exit).
//
// stories.exe [--lcs <lcs_game.dll>] [--vcs <vcs_game.dll>] [--lcs-iso <iso>] [--vcs-iso <iso>] [--front lcs|vcs]
//             [--env lcs:NAME=value | vcs:NAME=value | all:NAME=value]... [--size WxH]
// PS2X_HIDDEN=1 (tests): the frame is never shown (both games still run and render).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    constexpr UINT kMsgFullscreen = WM_APP + 0x51;   // platform/stories_child.h
    constexpr UINT kMsgFrontChanged = WM_APP + 0x52; // platform/stories_child.h
    constexpr UINT kMsgGameEnded = WM_APP + 0x60;    // wParam: game index, lParam: exit code

    using GameMain = int (*)(int, char **, const char *const *);

    struct Game
    {
        std::string id;      // "lcs" / "vcs"
        std::wstring dll;    // its DLL
        std::string iso;     // optional: the disc (else the game's vcs.ini / folder)
        std::vector<std::string> env; // NAME=value for its overlay
        std::vector<const char *> envPtrs;
        std::vector<std::string> args;
        std::vector<char *> argPtrs;
        GameMain main = nullptr;
        HANDLE thread = nullptr;
        int index = 0;
    };

    HWND g_frame = nullptr;
    HWND g_front = nullptr; // the child shown last (the game in front)
    bool g_fullscreen = false;
    WINDOWPLACEMENT g_windowed{sizeof(WINDOWPLACEMENT)};
    Game g_games[2];

    std::string narrow(const std::wstring &w)
    {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
    std::wstring widen(const std::string &s)
    {
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    std::wstring folderOf(const std::wstring &path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : path.substr(0, slash);
    }

    BOOL CALLBACK resizeChild(HWND child, LPARAM lp)
    {
        const RECT *rc = reinterpret_cast<const RECT *>(lp);
        if (GetParent(child) == g_frame)
            SetWindowPos(child, nullptr, 0, 0, rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        return TRUE;
    }
    BOOL CALLBACK closeChild(HWND child, LPARAM)
    {
        if (GetParent(child) == g_frame)
            PostMessageW(child, WM_CLOSE, 0, 0);
        return TRUE;
    }

    // the frame's place on the desktop, kept next to stories.exe (stories_window.txt: x y w h maximized), as the games keep
    // theirs in vcs.ini; the restored (windowed) place, also while full screen
    std::wstring g_placeFile;

    void savePlacement()
    {
        if (g_placeFile.empty() || !IsWindowVisible(g_frame))
            return;
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (g_fullscreen)
            wp = g_windowed;
        else if (!GetWindowPlacement(g_frame, &wp))
            return;
        const RECT &r = wp.rcNormalPosition;
        if (FILE *f = _wfopen(g_placeFile.c_str(), L"w"))
        {
            std::fprintf(f, "%ld %ld %ld %ld %d\n", r.left, r.top, r.right - r.left, r.bottom - r.top,
                         wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
            std::fclose(f);
        }
    }

    // true: placed (maximized: shown maximized)
    bool loadPlacement(bool &maximized)
    {
        long x = 0, y = 0, w = 0, h = 0;
        int max = 0;
        FILE *f = _wfopen(g_placeFile.c_str(), L"r");
        if (!f)
            return false;
        const int n = std::fscanf(f, "%ld %ld %ld %ld %d", &x, &y, &w, &h, &max);
        std::fclose(f);
        if (n < 4 || w < 200 || h < 150)
            return false;
        RECT r{x, y, x + w, y + h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
            return false; // that monitor is gone: the default place
        WINDOWPLACEMENT wp{sizeof(wp)};
        wp.showCmd = SW_HIDE;
        wp.rcNormalPosition = r;
        SetWindowPlacement(g_frame, &wp);
        maximized = max != 0;
        return true;
    }

    void setFullscreen(bool on)
    {
        if (on == g_fullscreen)
            return;
        g_fullscreen = on;
        if (!IsWindowVisible(g_frame))
            return; // a hidden run never goes full screen
        const LONG style = GetWindowLongW(g_frame, GWL_STYLE);
        if (on)
        {
            GetWindowPlacement(g_frame, &g_windowed);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(g_frame, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongW(g_frame, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_frame, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        else
        {
            SetWindowLongW(g_frame, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(g_frame, &g_windowed);
            SetWindowPos(g_frame, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }

    LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_SIZE:
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            EnumChildWindows(hwnd, &resizeChild, reinterpret_cast<LPARAM>(&rc));
            if (wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED)
                savePlacement();
            return 0;
        }
        case WM_EXITSIZEMOVE:
            savePlacement();
            return 0;
        case WM_ERASEBKGND:
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        case WM_ACTIVATE:
            // the games poll this (platform_sdl3.cpp syncHostedFocus: the frame in the foreground + their child shown =
            // keyboard and mouse focus): logged here so a focus question can be read off the log
            std::fprintf(stderr, "[stories] frame %s\n", LOWORD(wp) != WA_INACTIVE ? "activated" : "deactivated");
            break; // DefWindowProc: the focus to the frame (WM_SETFOCUS below hands it on)
        case WM_SETFOCUS:
            // the keyboard to the game in front. Its child is another thread's window: a cross-thread parent / child
            // pair shares one input state (Windows attaches the threads' input), so SetFocus reaches it
            if (g_front && IsWindow(g_front) && !SetFocus(g_front))
                std::fprintf(stderr, "[stories] SetFocus(front child) failed: %lu\n", GetLastError());
            return 0;
        case kMsgFrontChanged:
            g_front = reinterpret_cast<HWND>(lp);
            if (GetForegroundWindow() == hwnd && g_front)
                SetFocus(g_front);
            std::fprintf(stderr, "[stories] front: child %p\n", static_cast<void *>(g_front));
            return 0;
        case kMsgFullscreen:
            setFullscreen(wp != 0);
            return 0;
        case kMsgGameEnded:
            std::fprintf(stderr, "[stories] %s ended (%d): closing\n", g_games[wp].id.c_str(), static_cast<int>(lp));
            std::fflush(stderr);
            std::fflush(stdout);
            if (lp != 0 && IsWindowVisible(hwnd)) // a game that could not start or stopped with an error: said, not vanished
                MessageBoxW(hwnd,
                            wp == 0 ? L"Liberty City Stories stopped (it could not start, or ended with an error), so GTA "
                                      L"Stories closes. The details are in the log (build\\stories\\oneproc.log)."
                                    : L"Vice City Stories stopped (it could not start, or ended with an error), so GTA "
                                      L"Stories closes. The details are in the log (build\\stories\\oneproc.log).",
                            L"GTA Stories", MB_OK | MB_ICONERROR);
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(lp)); // the other game too, no DLL teardown under it
            return 0;
        case WM_CLOSE:
            // both games asked to quit (each sees its window's close request); the first to leave ends the process
            EnumChildWindows(hwnd, &closeChild, 0);
            SetTimer(hwnd, 1, 5000, nullptr); // [launcherfix] each game's own guard ends it after ~3 s
            return 0;
        case WM_TIMER:
            if (wp == 1)
            {
                std::fprintf(stderr, "[stories] the games did not quit in 5 s: ending\n");
                TerminateProcess(GetCurrentProcess(), 0);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    DWORD WINAPI gameThread(void *p)
    {
        Game &g = *static_cast<Game *>(p);
        const int code = g.main(static_cast<int>(g.argPtrs.size()) - 1, g.argPtrs.data(), g.envPtrs.data());
        PostMessageW(g_frame, kMsgGameEnded, static_cast<WPARAM>(g.index), static_cast<LPARAM>(code));
        return static_cast<DWORD>(code);
    }

    bool envOn(const char *name)
    {
        const char *v = std::getenv(name);
        return v && *v && *v != '0';
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t exePath[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    const std::wstring exeDir = folderOf(exePath);

    g_games[0].id = "lcs";
    g_games[1].id = "vcs";
    g_games[0].dll = exeDir + L"\\lcs\\lcs_game.dll";
    g_games[1].dll = exeDir + L"\\vcs\\vcs_game.dll";
    std::string front = "lcs";
    int width = 1280, height = 720;
    std::vector<std::string> extra[2];
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--lcs" && more)
            g_games[0].dll = argv[++i];
        else if (a == L"--vcs" && more)
            g_games[1].dll = argv[++i];
        else if (a == L"--lcs-iso" && more)
            g_games[0].iso = narrow(argv[++i]);
        else if (a == L"--vcs-iso" && more)
            g_games[1].iso = narrow(argv[++i]);
        else if (a == L"--front" && more)
            front = narrow(argv[++i]);
        else if (a == L"--size" && more)
            std::swscanf(argv[++i], L"%dx%d", &width, &height);
        else if (a == L"--env" && more)
        {
            const std::string e = narrow(argv[++i]);
            const size_t colon = e.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string who = e.substr(0, colon), pair = e.substr(colon + 1);
            for (int k = 0; k < 2; ++k)
                if (who == "all" || who == g_games[k].id)
                    extra[k].push_back(pair);
        }
        else
            std::fprintf(stderr, "[stories] unknown argument: %s\n", narrow(a).c_str());
    }
    LocalFree(argv);

    // DPI: the frame's pixels are the screen's (the games' windows were per-monitor aware through their manifest)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &frameProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"GTAStoriesFrame";
    RegisterClassExW(&wc);
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    g_frame = CreateWindowExW(0, wc.lpszClassName, L"GTA Stories (both cities, experimental)",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_frame)
    {
        std::fprintf(stderr, "[stories] CreateWindowEx: %lu\n", GetLastError());
        return 1;
    }
    const bool hidden = envOn("PS2X_HIDDEN");
    g_placeFile = exeDir + L"\\stories_window.txt";
    bool maximized = false;
    if (!hidden)
        loadPlacement(maximized);
    if (!hidden)
    {
        ShowWindow(g_frame, maximized ? SW_SHOWMAXIMIZED : show);
        UpdateWindow(g_frame);
    }
    std::fprintf(stderr, "[stories] frame %p (%dx%d)%s\n", static_cast<void *>(g_frame), width, height,
                 hidden ? ", hidden (PS2X_HIDDEN)" : "");

    char hwnd[32];
    std::snprintf(hwnd, sizeof(hwnd), "%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_frame)));
    for (int k = 0; k < 2; ++k)
    {
        Game &g = g_games[k];
        g.index = k;
        const std::string dir = narrow(folderOf(g.dll));
        g.env = {"STORIES_HOST_HWND=" + std::string(hwnd), "STORIES_GAME=" + g.id, "VCS_APP_DIR=" + dir,
                 std::string("STORIES_DUALWORLD=") + (g.id == front ? "front" : "back")};
        for (const std::string &e : extra[k])
            g.env.push_back(e);
        for (const std::string &e : g.env)
            g.envPtrs.push_back(e.c_str());
        g.envPtrs.push_back(nullptr);
        g.args = {narrow(g.dll)};
        if (!g.iso.empty())
            g.args.push_back(g.iso);
        for (std::string &a : g.args)
            g.argPtrs.push_back(a.data());
        g.argPtrs.push_back(nullptr);
        // its own DLLs (FFmpeg) from its own folder
        HMODULE mod = LoadLibraryExW(g.dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod)
        {
            std::fprintf(stderr, "[stories] cannot load %s (%lu)\n", narrow(g.dll).c_str(), GetLastError());
            return 2;
        }
        g.main = reinterpret_cast<GameMain>(GetProcAddress(mod, "stories_game_main"));
        if (!g.main)
        {
            std::fprintf(stderr, "[stories] %s has no stories_game_main: not a stories game DLL\n", narrow(g.dll).c_str());
            return 2;
        }
        std::fprintf(stderr, "[stories] %s: %s at %p\n", g.id.c_str(), narrow(g.dll).c_str(), static_cast<void *>(mod));
    }
    std::fflush(stderr);
    // the front game first (its window comes up first); a thread's stack as big as an exe's main thread could want
    for (int pass = 0; pass < 2; ++pass)
        for (Game &g : g_games)
            if ((g.id == front) == (pass == 0))
            {
                g.thread = CreateThread(nullptr, 64u << 20, &gameThread, &g, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
                if (!g.thread)
                {
                    std::fprintf(stderr, "[stories] CreateThread: %lu\n", GetLastError());
                    return 3;
                }
            }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
