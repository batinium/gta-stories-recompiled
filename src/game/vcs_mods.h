// [android] Mod loader, first step (docs/ANDROID.md "Mods"): tuning mods for existing vehicle and weapon slots, and
// streaming-entry redirection for model / texture replacement.
//
// The launcher (ModsMenu.java / ModFormats.java) imports a mod into <data>/mods/<id>/ and writes the enabled mods'
// values, already parsed, into <data>/mods/active.txt. Here, when the game has loaded and relocated its GTAG image
// (GAME.DTZ, FUN_00324A50), each value is converted to the game's units and written into the existing tables in guest
// RAM: the 0xE0-byte handling records, the weapon records (40 x 0x70, both copies) and the streaming directory. The
// original bytes are kept in host memory (revert()). Nothing is written to the disc, the save or GAME.DTZ, and no new
// model id, weapon type or streaming index is created: a save only ever holds stock ids, and without the mods the next
// start is the stock game.
//
// active.txt, one directive per line (# comments):
//   mod <id> <name...>                                   the mod the next lines belong to (for the log)
//   handling id=<model id> <field> <value> ...           PC handling.cfg units; fields in vcs_mods.cpp kHandling
//   weapon type=<weapon type> <field> <value> ...        weapon.dat units; fields in kWeapon
//   stream index=<streaming index> copy=<index>          serve <index>'s stock bytes (read from the player's disc)
//                                                        from host memory in place of this entry (plumbing test)
//   stream index=<streaming index> file=<path>           serve a converted VCS chunk from <data>/mods/<path>
//   model id=<model id> dff=<file> txd=<file>            a PC model converted in place of the slot's (converted once,
//                                                        cached in mods/<mod folder>/converted/: docs/ANDROID.md "Mods")
//   replace disc=<path on the disc> file=<path>          serve a whole disc file (e.g. TEXT/ENGLISH.GXT) from
//                                                        <data>/mods/<path>: the game's disc index is repointed
//   test give=<weapon type> / spawn=<vehicle model>      device tests once free play starts
//   needs file=<path>                                    [mods] a mod kept where it is: its original; missing -> the
//                                                        whole mod is skipped this start ("original missing")
// [mods] A <file> / <path> is under <data>/mods/, an absolute path, or place:<handle> (Android), and may name one entry
// of a .zip / .vcm as <container>!<entry>; %XX escapes (vcs_modfiles.h). A model's cache is taken by the sources'
// size and time alone when they are unchanged (.src next to .key), so a kept zip is not even opened then.
// VCS_MODS=0 turns the loader off. What was applied is written to <data>/mods/applied.txt and the log ("[mods]").
//
// [lcs] Liberty City Stories: the same file in LCS's own data folder (each game has its own mods); every directive,
// with LCS's addresses, handling record layout and chunk dialect (vcs_mods.cpp Profile, docs/LCS.md "Mods"). VCS_MODS_DUMP=1 logs the
// raw LCS tables the field maps come from.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class PS2Runtime;

namespace vcs::mods
{
    // Main thread, before runtime.run() (applyVcs: the GTAG relocation hook; applyLcs, after game::setCurrent: the
    // file-open and model-info-setup hooks).
    void install(PS2Runtime &runtime);
    // Game thread: put every byte the loader changed back (also drops the stream redirections).
    void revert(uint8_t *rdram);
    // Game thread, at the frame kick: a one-time log of the values the game reaches through its own pointers.
    void onGameFrame(uint8_t *rdram);
    // Number of mods applied at this start (any thread).
    int appliedMods();

    // [crossover] (game/lcs_crossover.cpp, LCS) game thread: a PC model (.dff / .txd under mods/) converted onto slot
    // `id` and served for its model and texture-list entries, its car values and handling fields applied - what a
    // start's `model` + `handling` directives do, at run time. cacheTag: the converted cache's name next to the slot id
    // (several models for one slot). False with `why` when it could not be converted.
    bool bindVehicle(uint8_t *rdram, int id, const std::string &mod, const std::string &dff, const std::string &txd,
                     const std::string &cacheTag, const std::vector<std::pair<std::string, std::string>> &handling,
                     std::string &why, int donor = 0);
    // [crossover] donor > 0: `id` is a new model id (no stock data): converted on the donor's stock model / texture
    // list. setScratch: guest memory holding new model infos / records (accepted by the pointer checks);
    // setHandlingOverride: a model info's handling record is `record` (0: back to its handling id's)
    void setScratch(uint32_t lo, uint32_t hi);
    // [crossover] a vehicle's paint colour pairs ("r,g,b,r,g,b", up to 8: the nearest colours of the game's palette),
    // as the `colours` directive (LCS)
    void setVehicleColours(uint8_t *rdram, int id, const std::string &mod, const std::vector<std::string> &pairs);
    void setHandlingOverride(uint32_t modelInfo, uint32_t record);
    // The tables the loader found (0 before): a streaming entry's address, a model info, the texture-list base index,
    // a vehicle model's handling record.
    uint32_t streamEntry(uint32_t index);
    uint32_t modelInfo(int id);
    uint32_t textureListBase();
    uint32_t handlingRecord(int id);

    // [hero] (game/stories_hero.cpp) game thread: `bytes` served as a new range of GTA3PS2.IMG (host memory, past the
    // disc's end); its first sector relative to the IMG, and its sector count. 0 when the IMG is not found.
    uint32_t serveBlob(std::vector<uint8_t> bytes, uint32_t &sectors);
    // [hero] called once on the game thread when LCS's GAME.DTZ tables are known (the model-info setup hook), after the
    // mods' table changes; before any model streams.
    void onTables(void (*fn)(uint8_t *rdram));
}
