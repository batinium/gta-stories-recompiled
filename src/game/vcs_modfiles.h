// [mods] Where a mod's file is, and reading it (docs/ANDROID.md "Mods", "Kept where it is"). The mod manager (the
// Android launcher's ModsMenu, the desktop's tools/mods.py) writes file paths into mods/active.txt (vcs_mods.h):
//
//   <relative path>   under <data>/mods/: the manager's own copy ("Import": <id>/source/<file>, or <id>/files/<file>
//                     for a file taken out of a .vcm or of a zip inside a zip, and for mods imported before this)
//   <absolute path>   the desktop's "Keep where it is": the player's own file, read where it is (C:/..., /home/...)
//   place:<handle>    Android's "Keep where it is": the game's activity (VCSActivity) opened the document through its
//                     persisted read permission and put "<handle>=<fd>" into VCS_MODS_PLACES; read as /proc/self/fd/<fd>
//
// A value may end in "!<entry>": that entry of a .zip (stored or deflated) or a VC Mod Manager .vcm, read on demand.
// %XX escapes a byte (space, %, !, non-ASCII), so a value is one token of the line.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vcs::mods::files
{
    struct Location
    {
        std::filesystem::path file; // the file itself, or the .zip / .vcm holding the entry
        std::string entry;          // the entry inside it ("" = the whole file)
        std::string shown;          // for the report: the value as written, decoded
        std::string error;          // why it can't be used ("" = resolved)
        bool inPlace = false;       // the player's own file (an absolute path or place:), not the manager's copy
    };

    // active.txt value -> Location (error set for "..", an unknown place: handle...)
    Location resolve(const std::string &value, const std::filesystem::path &modsDir);

    // The file (or its container) is there and can be opened
    bool available(const Location &l);

    // A cheap identity of what would be read: the container's size and modification time and the entry's name; empty
    // when unknown (a pipe from a document provider, a missing file). Used by the converted-model cache to skip
    // reading the source at all (vcs_mods.cpp ModelCache).
    std::string stamp(const Location &l);

    // The bytes (the whole file or one entry); false (and why) when missing, damaged or over `limit`
    bool read(const Location &l, std::vector<uint8_t> &out, size_t limit, std::string &error);

    // UTF-8 text -> a path (Windows: not the ANSI code page)
    std::filesystem::path pathOf(const std::string &utf8);
}
