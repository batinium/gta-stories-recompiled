// [lcs] The games this code base can run, one entry each: what identifies its disc (the boot executable SYSTEM.CNF
// names and that file's sha1) and the short id the builds use (CMake VCS_GAME, the Android game library's stamp
// "game=<id>", src/android/game_abi.h). VCS's values are vcs_disc_id.h's (upstream's, the PC launcher reads it).
// The same table is in tools/vcs_disc.py (KNOWN_GAMES) and android/.../DiscCheck.java (GAMES): change all three.
#pragma once

#include "disc/vcs_disc_id.h"

#include <cstring>

namespace vcs::disc
{
    struct KnownGame
    {
        const char *id;      // "vcs", "lcs": CMake VCS_GAME, the game library's stamp
        const char *elfName; // the boot executable on the disc
        const char *elfSha1; // the one build of it the recompiled code matches
        const char *title;   // for the window / messages
        bool vcs;            // Vice City Stories: the game every src/game feature was written for
    };

    inline constexpr KnownGame kGameVcs{"vcs", kElfName, kElfSha1, "GTA: Vice City Stories", true};
    // Liberty City Stories (USA), SYSTEM.CNF "BOOT2 = cdrom0:\SLUS_214.23;1", VER 1.01: in bring-up
    inline constexpr KnownGame kGameLcs{"lcs", "SLUS_214.23", "9b021b4c4e42087bb9b374709f26cf67744b75ab",
                                        "GTA: Liberty City Stories", false};
    inline constexpr const KnownGame *kKnownGames[] = {&kGameVcs, &kGameLcs};

    // The game with this id ("vcs", "lcs"); null for an unknown one
    inline const KnownGame *gameById(const char *id)
    {
        for (const KnownGame *g : kKnownGames)
            if (id && std::strcmp(g->id, id) == 0)
                return g;
        return nullptr;
    }
}
