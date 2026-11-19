// [lcs] The settings menu on Liberty City Stories: the settings that only act on Vice City Stories' code are greyed
// out with a note ("Vice City Stories only for now"), so a row that does nothing doesn't look like a bug. The list and
// why each one is on it: lcs_settings.cpp. A setting ported to LCS leaves the list.
#pragma once

namespace lcs::settings_menu
{
    // After main's registerSettings, in an LCS run only (before the menu first opens).
    void lockVcsOnly();
}
