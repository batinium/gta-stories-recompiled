// Is a file the supported disc? The check vcs.exe makes (src/disc/vcs_disc_id.h), plus the reasons a player can act
// on: another release, another game, an archive or a .bin / .chd that needs converting. tools/vcs_disc.py check is
// the same for the command line.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher
{
    struct DiscResult
    {
        bool ok = false;
        std::string code;    // ok, not_found, not_iso, not_ps2, other_release, unsupported_build
        std::string message; // for the player
        std::string boot;    // SYSTEM.CNF's executable
        std::string game;    // [lcs] the known game the disc is (src/disc/known_games.h: "vcs", "lcs"), when it is one
    };

    // [lcs] any known game's disc is ok (Vice City Stories or Liberty City Stories, USA): `game` says which
    DiscResult checkDisc(const std::filesystem::path &path);

    // [lcs] the launcher's games, as tools/toolchain.py GAMES: "vcs", "lcs"
    const char *gameTitle(const std::string &game); // "Vice City Stories", "Liberty City Stories"

    // .iso files where players keep them: the launcher's folder, Downloads, Desktop, Documents (and one level below
    // each), sized like a DVD image. Not checked yet.
    // [lcs] Discs named for `game` first ("vice city stories", "liberty city stories").
    std::vector<std::filesystem::path> discCandidates(const std::filesystem::path &root, const std::string &game = "vcs");
}
