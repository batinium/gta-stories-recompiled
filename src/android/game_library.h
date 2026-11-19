// [android] The runtime / game split (docs/ANDROID.md "Runtime / game split"): libmain.so (the APK: SDL, the runtime,
// our code) loads the player's game library (libvcs_game.so: the generated code) before the game's main runs.
#pragma once

#include <string>

namespace vcs_android
{
    // The library's stamp (game_abi.h) read from the file: empty if it has none. Nothing is loaded.
    std::string readGameLibraryStamp(const char *path);

    // Checks the stamp (a different VCS_GAME_ABI is refused before any of its code runs), loads it (its static
    // initialisers fill the function table and register the compiled VU1 code) and points the runtime at its table.
    // false: `error` says why, for the player.
    bool loadGameLibrary(const char *path, std::string &error);

    // [lcs] The game a stamp names (" game=<id>": "vcs", "lcs"); "vcs" for a stamp without one (made before LCS).
    std::string stampGame(const std::string &stamp);
    // The loaded library's game ("vcs" before one is loaded): main.cpp boots that game's ELF (disc/known_games.h).
    const char *loadedGameId();
}
