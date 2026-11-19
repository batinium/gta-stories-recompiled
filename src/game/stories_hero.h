// [hero] One hero in both cities (docs/STORIES.md "One hero in both cities"): the "Stories: hero" option
// (STORIES_HERO = city | vic | toni; city, the default, is each game's own hero). In Liberty City Stories with
// STORIES_HERO=vic, Vic Vance's model - translated once from the player's own Vice City Stories disc into LCS's
// model dialect by tools/stories_pack.py hero (mod stories_vcs_hero, active.txt `hero name=vic mdl=... xtx=...`) - is
// served in place of Toni's gameplay outfits; cutscenes keep their own models (Toni).
#pragma once

#include <string>

class PS2Runtime;

namespace stories::hero
{
    // From applyLcs (lcs_overrides.cpp), before vcs::mods::install.
    void install(PS2Runtime &runtime);
    // Any thread: the hero asked for ("city", "vic", "toni") and whether this game now plays him (not its own).
    std::string wanted();
    bool active();
}
