// [stories] Both cities on Android, the one-process host in libmain.so (docs/ANDROID.md "Both cities on Android").
//
// VCSActivity sets VCS_STORIES=1 (a --stories build's "Both cities" row): instead of loading a code pack, SDL_main
// (game_main.cpp) runs this. It loads both games' self-contained libraries (libstories_lcs.so, libstories_vcs.so:
// each a whole runtime, everything local but stories_game_main; packaged in the APK), hands each its own environment,
// and runs Liberty City in front on this thread (SDL's SDLThread: the one window, the event pump) and Vice City behind
// on a thread of its own, headless (PS2X_HEADLESS: no window, no events; Vulkan offscreen). The dual world's shared block
// is plain memory reserved here (STORIES_DW_BLOCK); the far city's picture goes over through it (the CPU path).
#include "android/stories_host.h"

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace vcs_android
{
    namespace
    {
        using GameMain = int (*)(int argc, char *argv[], const char *const *env);

        struct Game
        {
            const char *id = "";
            std::string library, disc;
            std::vector<std::string> env;
            GameMain main = nullptr;
            int code = 0;
        };

        std::string envOr(const char *name, const char *fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? v : fallback;
        }

        bool load(Game &g, std::string &error)
        {
            // RTLD_LOCAL: nothing of it joins the global group; its own names are local anyway (game_so.map), its SDL_* and
            // JNI-glue imports resolve to libmain.so (in the global group: -z global)
            void *h = dlopen(g.library.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h)
            {
                const char *why = dlerror();
                error = g.library + ": " + (why ? why : "does not load");
                return false;
            }
            g.main = reinterpret_cast<GameMain>(dlsym(h, "stories_game_main"));
            if (!g.main)
            {
                error = g.library + ": no stories_game_main (not built with tools/android.py build-game --stories)";
                return false;
            }
            std::fprintf(stderr, "[stories] loaded %s\n", g.library.c_str());
            return true;
        }

        int run(Game &g)
        {
            std::vector<const char *> env;
            for (const std::string &e : g.env)
                env.push_back(e.c_str());
            env.push_back(nullptr);
            std::string arg0 = g.library;
            std::vector<char *> argv{arg0.data(), g.disc.data(), nullptr};
            std::fprintf(stderr, "[stories] %s starts (%s)\n", g.id, g.disc.c_str());
            g.code = g.main(2, argv.data(), env.data());
            std::fprintf(stderr, "[stories] %s ended: %d\n", g.id, g.code);
            return g.code;
        }

        // the game behind: once the front's window is there (SDL's video and audio set up by the front's platform::init on
        // SDL's thread, so this one only adds its audio stream), up to 20 s
        void *backThread(void *arg)
        {
            Game &g = *static_cast<Game *>(arg);
            for (int i = 0; i < 200; ++i)
            {
                int count = 0;
                if (SDL_Window **w = SDL_GetWindows(&count))
                    SDL_free(w);
                if (count > 0)
                    break;
                SDL_Delay(100);
            }
            SDL_Delay(500);
            run(g);
            return nullptr;
        }

        // the background Vice City's own card: a copy of the player's Vice City card the first time (as tools/stories.py's
        // background_card: the player's own card is never written by the game behind)
        void prepareBackground(const fs::path &dir, const fs::path &playerData, const fs::path &appDir)
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (!fs::exists(dir / "mc0", ec) && fs::is_directory(playerData / "mc0", ec))
            {
                fs::copy(playerData / "mc0", dir / "mc0", fs::copy_options::recursive, ec);
                std::fprintf(stderr, "[stories] Vice City behind: a copy of your Vice City card%s%s\n", ec ? ": " : "",
                             ec ? ec.message().c_str() : "");
            }
            // its settings: the player's (shared) vcs.ini as they are now, in a file of its own (VCS_KEEP_SETTINGS: never
            // written back)
            if (fs::is_regular_file(appDir / "vcs.ini", ec))
                fs::copy_file(appDir / "vcs.ini", dir.parent_path() / "vcs-background.ini", fs::copy_options::overwrite_existing, ec);
        }

        int fail(const std::string &message)
        {
            std::fprintf(stderr, "[stories] %s\n", message.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Both cities", message.c_str(), nullptr);
            return 1;
        }
    }

    bool storiesRequested()
    {
        const char *s = std::getenv("VCS_STORIES");
        return s && s[0] == '1';
    }

    int storiesMain(int argc, char *argv[])
    {
        if (argc < 3 || !*argv[1] || !*argv[2])
            return fail("Both cities needs both discs: choose Liberty City Stories' and Vice City Stories' under Disc.");
        const fs::path appDir = envOr("VCS_APP_DIR", ".");
        const fs::path storiesDir = appDir / "stories";
        const fs::path bgData = storiesDir / "vcs-background";
        prepareBackground(bgData, envOr("VCS_STORIES_VCS_DATA", (appDir / "userdata").c_str()), appDir);

        // the dual world's block (dualworld.cpp Shared: ~100 MB of picture slots at most): reserved, not committed - only
        // the pages the games touch become memory
        constexpr size_t kBlock = size_t(256) << 20;
        void *block = mmap(nullptr, kBlock, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (block == MAP_FAILED)
            return fail("Both cities: no memory for the shared block");
        char blockEnv[96];
        std::snprintf(blockEnv, sizeof(blockEnv), "STORIES_DW_BLOCK=%llx:%llx",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(block)), static_cast<unsigned long long>(kBlock));

        // both games: the same world settings as tools/stories.py's; no crossing yet (the one surface stays the front's)
        const std::vector<std::string> common = {
            blockEnv,
            "STORIES_DW_OFFSET=" + envOr("STORIES_DW_OFFSET", "-3800,0,0"),
            "STORIES_DW_BORDER=" + envOr("STORIES_DW_BORDER", "1800"),
            "STORIES_DW_HANDOVER=0",
            "STORIES_DW_WINDOWS=0",
            "STORIES_DW_MAX=" + envOr("STORIES_DW_MAX", "1280x720"),
            "STORIES_HERO=city",
            "VCS_SKIP_MOVIES=1", // the guests are built without FFmpeg
        };
        static Game front, back; // back: its thread's for the process's life
        front.id = "lcs";
        front.library = envOr("VCS_STORIES_LCS_LIBRARY", "libstories_lcs.so");
        front.disc = argv[1];
        front.env = common;
        front.env.insert(front.env.end(), {"STORIES_GAME=lcs", "STORIES_DUALWORLD=front"});
        back.id = "vcs";
        back.library = envOr("VCS_STORIES_VCS_LIBRARY", "libstories_vcs.so");
        back.disc = argv[2];
        back.env = common;
        const std::string size = envOr("STORIES_DW_SIZE", "1280x720");
        back.env.insert(back.env.end(), {
            "STORIES_GAME=vcs", "STORIES_DUALWORLD=back", "PS2X_HEADLESS=1", "PS2X_WINDOW_HIDDEN=1",
            "PS2X_HIDDEN_WINDOW=" + size, "STORIES_DW_SIZE=" + size,
            "VCS_APP_DIR=" + storiesDir.string(), // its own files (logs, notes) never over the front's
            "VCS_DATA_DIR=" + bgData.string(), "VCS_KEEP_SETTINGS=1", "VCS_FAR_CLIP=6000", "STORIES_DW_OPENING=0",
            "VCS_LOAD_SLOT=",     // the launcher's slot is Liberty City's: Vice City plays its own card's newest
            "VCS_MODS_PLACES=",   // Liberty City's mods
            "VCS_RSL_RECORDINGS_MB=" + envOr("STORIES_BACK_RSL_MB", "96"), // memory: docs/ANDROID.md (e)
        });
        if (fs::is_regular_file(storiesDir / "vcs-background.ini"))
            back.env.push_back("VCS_SETTINGS_INI=" + (storiesDir / "vcs-background.ini").string());

        std::string error;
        if (!load(front, error) || !load(back, error))
            return fail("Both cities: " + error);

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, size_t(8) << 20);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        const int started = pthread_create(&thread, &attr, backThread, &back);
        pthread_attr_destroy(&attr);
        if (started != 0)
            return fail("Both cities: Vice City's thread did not start");
        pthread_setname_np(thread, "vcs-behind");
        return run(front);
    }
}
