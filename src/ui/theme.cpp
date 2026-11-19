#include "ui/theme.h"
#include "game/game_table.h"

#include <algorithm>

namespace ui::theme
{
    ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(a * 255.0f)); }

    ImU32 withAlpha(ImU32 col, float a)
    {
        const float k = std::clamp(a, 0.0f, 1.0f);
        const ImU32 alpha = static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * k);
        return (col & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    }

    namespace
    {
        ImVec4 c(int r, int g, int b, float a = 1.0f)
        {
            return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
        }

        // Vice City Stories: the values the overlay always had (the settings menu keeps ImGui's own dark style).
        const Palette kVcs = {rgba(255, 70, 170), rgba(110, 40, 210), rgba(255, 70, 170), rgba(60, 220, 240),
                              rgba(10, 8, 18), rgba(255, 255, 160),
                              rgba(8, 6, 14, 0.78f), rgba(245, 245, 245, 0.97f), rgba(240, 240, 240), rgba(15, 10, 20),
                              rgba(120, 120, 130), rgba(120, 120, 120), rgba(40, 200, 110), rgba(40, 200, 110),
                              rgba(230, 90, 110), rgba(150, 40, 60), false};

        // Liberty City Stories (docs/LCS.md "Overlay theme"): the pause menu's crimson
        // #AC0000 / amber #F9B035 / steel blue #74AEDA / navy, but made for READING: near-white body text (#E8EEF5) on
        // navy, crimson only as a thin accent (the F5 title bar, the settings tabs' overline), amber for stripes and
        // marks, lifted slate for frames / selected rows. Every text/background pair is >= 4.5:1 (NOTES.md).
        const Palette kLcs = {
            rgba(0xAC, 0x00, 0x00), rgba(0x30, 0x04, 0x10), // F5 title bar: crimson into a dark wine (white text)
            rgba(0xF9, 0xB0, 0x35),                         // stripes: amber
            rgba(0x74, 0xAE, 0xDA),                         // breadcrumb: steel blue
            rgba(0x01, 0x01, 0x1F),                         // notice background: navy
            rgba(0xF9, 0xB0, 0x35),                         // fps: amber
            rgba(0x08, 0x0C, 0x1C, 0.92f), rgba(0x2A, 0x3F, 0x66, 0.98f), // F5 rows: navy / lifted slate (selected)
            rgba(0xE8, 0xEE, 0xF5), rgba(0xFF, 0xFF, 0xFF),               // text, selected text
            rgba(0x9D, 0xAA, 0xBB), rgba(0xC0, 0xCB, 0xDA),               // disabled text, selected disabled
            rgba(0x4C, 0xDF, 0x94), rgba(0x6A, 0xEB, 0xAA),               // ON, selected ON
            rgba(0xFF, 0x7F, 0x8E), rgba(0xFF, 0x9F, 0xAA),               // OFF, selected OFF
            true,
            c(0x0B, 0x14, 0x26, 0.97f), // windowBg: navy
            c(0x16, 0x22, 0x3C),        // title bar (inactive and active alike): dark slate
            c(0x22, 0x32, 0x4A),        // frame (combo boxes, fields): lighter slate than the panel
            c(0x2D, 0x42, 0x63),        // frameHover
            c(0x36, 0x50, 0x7A),        // frameActive
            c(0x2F, 0x4A, 0x75),        // button: slate blue, light text
            c(0x3C, 0x5C, 0x90),        // buttonHover
            c(0x42, 0x66, 0x9C),        // buttonActive
            c(0x2A, 0x3F, 0x66),        // header (selected row): lifted slate
            c(0x34, 0x50, 0x7F),        // headerHover
            c(0x3E, 0x5F, 0x96),        // headerActive
            c(0x18, 0x25, 0x3F),        // tab
            c(0x2F, 0x4A, 0x75),        // tabHover
            c(0x24, 0x3A, 0x62),        // tabActive
            c(0xD0, 0x18, 0x18),        // tab overline: crimson, thin
            c(0xF9, 0xB0, 0x35),        // check mark / slider grab: amber
            c(0xE8, 0xEE, 0xF5),        // text: near white
            c(0x9D, 0xAA, 0xBB),        // disabled text
            c(0xF9, 0xB0, 0x35),        // accent text (the "changed" markers): amber
            c(0x3A, 0x4E, 0x70),        // border
            c(0x3A, 0x4E, 0x70),        // separator
            c(0x0F, 0x1A, 0x30),        // scrollbar background
            c(0x5D, 0x7A, 0xA3),        // scrollbar grab
            c(0x7B, 0x99, 0xC2),        // grab hover
            c(0xF9, 0xB0, 0x35)         // grab active: amber
        };
    } // namespace

    const Palette &palette() { return game::isLcs() ? kLcs : kVcs; }

    void applyImGuiStyle()
    {
        const Palette &p = palette();
        if (!p.styleImGui)
            return;
        ImVec4 *col = ImGui::GetStyle().Colors;
        col[ImGuiCol_WindowBg] = p.windowBg;
        col[ImGuiCol_PopupBg] = p.windowBg;
        col[ImGuiCol_Border] = p.border;
        col[ImGuiCol_TitleBg] = p.title;
        col[ImGuiCol_TitleBgActive] = p.title;
        col[ImGuiCol_FrameBg] = p.frame;
        col[ImGuiCol_FrameBgHovered] = p.frameHover;
        col[ImGuiCol_FrameBgActive] = p.frameActive;
        col[ImGuiCol_Button] = p.button;
        col[ImGuiCol_ButtonHovered] = p.buttonHover;
        col[ImGuiCol_ButtonActive] = p.buttonActive;
        col[ImGuiCol_Header] = p.header;
        col[ImGuiCol_HeaderHovered] = p.headerHover;
        col[ImGuiCol_HeaderActive] = p.headerActive;
        col[ImGuiCol_Tab] = p.tab;
        col[ImGuiCol_TabHovered] = p.tabHover;
        col[ImGuiCol_TabSelected] = p.tabActive;
        col[ImGuiCol_TabDimmed] = p.tab;
        col[ImGuiCol_TabDimmedSelectedOverline] = p.tabOverline;
        col[ImGuiCol_TabDimmedSelected] = p.tabActive;
        col[ImGuiCol_CheckMark] = p.check;
        col[ImGuiCol_SliderGrab] = p.check;
        col[ImGuiCol_SliderGrabActive] = p.buttonHover;
        col[ImGuiCol_Text] = p.textColor;
        col[ImGuiCol_TextDisabled] = p.textDisabled;
        col[ImGuiCol_Separator] = p.separator;
        col[ImGuiCol_TabSelectedOverline] = p.tabOverline;
        col[ImGuiCol_ScrollbarBg] = p.scrollbarBg;
        col[ImGuiCol_ScrollbarGrab] = p.scrollbarGrab;
        col[ImGuiCol_ScrollbarGrabHovered] = p.scrollbarGrabHover;
        col[ImGuiCol_ScrollbarGrabActive] = p.scrollbarGrabActive;
        col[ImGuiCol_NavCursor] = p.check;
    }
} // namespace ui::theme
