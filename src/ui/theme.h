// The colours of the overlay UI we draw ourselves (F5 trainer panel and notices, F2 settings menu, fps readout):
// ONE place. The game this run is picks the palette (game::isLcs): Vice City Stories' pink / violet / cyan sunset, or
// [lcs] Liberty City Stories' crimson / amber / steel blue (sampled from LCS's own pause menu, colours only; the list
// and where each is from: docs/LCS.md "Overlay theme"). A game's palette is a table in theme.cpp; nothing else holds
// a colour.
#pragma once

#include "imgui.h"

namespace ui::theme
{
    struct Palette
    {
        ImU32 headerA, headerB; // the trainer panel's title bar gradient
        ImU32 accent;           // the selected row's stripe, the notices' stripe
        ImU32 path;             // the trainer's breadcrumb text
        ImU32 notice;          // notice background (before alpha)
        ImU32 fps;              // the fps readout
        // the trainer rows (unselected / selected background and the text on each)
        ImU32 row, rowSel, text, textSel, textOff, textOffSel, on, onSel, off, offSel;
        bool styleImGui;        // the settings menu (Dear ImGui widgets) gets the colours below; VCS keeps ImGui's dark default
        // Dear ImGui colours for the settings menu (used when styleImGui)
        ImVec4 windowBg, title, frame, frameHover, frameActive, button, buttonHover, buttonActive, header, headerHover,
            headerActive, tab, tabHover, tabActive, tabOverline, check, textColor, textDisabled, accentText, border, separator,
            scrollbarBg, scrollbarGrab, scrollbarGrabHover, scrollbarGrabActive;
    };

    const Palette &palette(); // the current game's
    void applyImGuiStyle();   // main thread, per settings-menu frame: ImGui's colours for the game (a no-op for VCS)
    ImU32 rgba(int r, int g, int b, float a = 1.0f);
    ImU32 withAlpha(ImU32 col, float a); // col with its alpha scaled
}
