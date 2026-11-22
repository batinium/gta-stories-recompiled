// [android] The game's entry point in libmain.so: SDL's Java activity (VCSActivity) calls SDL_main with the arguments
// it chose (argv[1] = the player's disc as /proc/self/fd/N). main.cpp's main is compiled as vcs_game_main on Android
// (CMakeLists.txt), so the desktop entry point stays as it is.
//
// The split build (the default APK, PS2X_SPLIT_RUNTIME): the generated code is the player's game library, loaded here
// first (game_library.cpp). VCSActivity names it in VCS_GAME_LIBRARY (files/game/libvcs_game.so, imported by the
// launcher). A refused library leaves its reason in files/game_library_error.txt for the launcher, and in a message box.
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_messagebox.h>

#include "android/android_log.h"
#include "android/diagnostics.h"
#if defined(PS2X_SPLIT_RUNTIME) && PS2X_SPLIT_RUNTIME
#include "android/game_library.h"
#include "android/stories_host.h" // [stories] both cities: two games' own libraries instead of a code pack
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

int vcs_game_main(int argc, char *argv[]);

namespace
{
#if defined(PS2X_SPLIT_RUNTIME) && PS2X_SPLIT_RUNTIME
    std::string appDir()
    {
        const char *app = std::getenv("VCS_APP_DIR");
        return app && *app ? app : ".";
    }

    bool loadGame()
    {
        const std::string errorFile = appDir() + "/game_library_error.txt";
        const char *env = std::getenv("VCS_GAME_LIBRARY");
        const std::string path = env && *env ? env : appDir() + "/game/libvcs_game.so";
        std::string error;
        if (vcs_android::loadGameLibrary(path.c_str(), error))
        {
            std::remove(errorFile.c_str());
            return true;
        }
        std::fprintf(stderr, "[game-library] refused: %s\n", error.c_str());
        if (std::FILE *f = std::fopen(errorFile.c_str(), "w"))
        {
            std::fputs(error.c_str(), f);
            std::fclose(f);
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game library", error.c_str(), nullptr);
        return false;
    }
#endif
}

namespace
{
    // [android] Switches for A/B runs on the phone (the desktop takes them from the environment): files/env.txt, one
    // NAME=value a line (# comments), set before the game starts. Written by `python tools/android.py env NAME=value ...`
    // (adb, run-as: a debug build); a name already in the environment (VCSActivity's) is left as it is. Logged.
    void loadEnvFile()
    {
        const char *app = std::getenv("VCS_APP_DIR");
        const std::string path = std::string(app && *app ? app : ".") + "/env.txt";
        std::FILE *f = std::fopen(path.c_str(), "r");
        if (!f)
            return;
        char line[512];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            const size_t eq = s.find('=');
            if (s.empty() || s[0] == '#' || eq == std::string::npos || eq == 0)
                continue;
            const std::string name = s.substr(0, eq), value = s.substr(eq + 1);
            if (std::getenv(name.c_str()))
            {
                std::fprintf(stderr, "[android] env.txt: %s already set, kept\n", name.c_str());
                continue;
            }
            setenv(name.c_str(), value.c_str(), 1);
            std::fprintf(stderr, "[android] env.txt: %s=%s\n", name.c_str(), value.c_str());
        }
        std::fclose(f);
    }
}

int main(int argc, char *argv[]) // SDL_main.h renames this SDL_main, the symbol the activity looks up
{
    vcs_android::redirectStdioToLogcat("vcs");
    std::fprintf(stderr, "[android] game start, %d argument(s)%s%s\n", argc - 1, argc > 1 ? ": " : "", argc > 1 ? argv[1] : "");
    loadEnvFile();
    vcs_android::startDiagnostics(); // [diag] heartbeat + stall watchdog in game.log (VCS_WATCHDOG=0: no watchdog)
#if defined(PS2X_SPLIT_RUNTIME) && PS2X_SPLIT_RUNTIME
    if (vcs_android::storiesRequested()) // [stories] docs/ANDROID.md "Both cities on Android"
    {
        const int code = vcs_android::storiesMain(argc, argv);
        std::fprintf(stderr, "[android] both cities exit %d\n", code);
        return code;
    }
    if (!loadGame())
        return 1;
#else
    std::fprintf(stderr, "[android] personal build: the game is inside this APK\n");
#endif
    const int code = vcs_game_main(argc, argv);
    std::fprintf(stderr, "[android] game exit %d\n", code);
    return code;
}
