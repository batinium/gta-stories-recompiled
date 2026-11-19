// [lcs] Swimming for Liberty City Stories (docs/LCS.md "Swimming"). LCS's Toni drowns in deep water: CPed's buoyancy
// (0x1D65E0, from CPed::ProcessControl 0x1D6F30) hurts a ped whose head is under the surface. With the setting
// Enhancements > Gameplay > Swimming on (or STORIES_DUALWORLD set), the player on foot in deep water is held at the
// surface instead and moves with his normal on-foot controls; with the mod "Swim animations (from your VCS disc)"
// (tools/stories_pack.py swim, made from the player's own Vice City Stories disc) he swims with Vic's animations, else
// a stand-in. Jump climbs onto a ledge within reach; no weapons in the water. Other peds and vehicles are unchanged.
#pragma once

class PS2Runtime;

namespace lcs::swim
{
    // From applyLcs (lcs_overrides.cpp).
    void install(PS2Runtime &runtime);
    // From lcs::settings_menu::lockVcsOnly (before the settings are loaded).
    void registerSettings();

    // Any thread: swimming is on for this run (setting or STORIES_DUALWORLD) / the player is swimming now.
    bool enabled();
    bool swimming();
}
