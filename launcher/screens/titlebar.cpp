// The title bar the launcher draws itself (the window has no Windows frame): the name, the sheets (Saves, Settings,
// Help) and the window's buttons. [lcs] The name is the game switch: Liberty City Stories | Vice City Stories, each
// with its own disc, build and saves (App::selectGame) | [stories] GTA Stories, both cities at once. It tells the platform where the caption is, so dragging, double-click, snapping
// and the system menu stay Windows' own. macOS: its own traffic lights stay (top left), so none are drawn here.
#include "shell.h"

#include "platform/platform.h"

namespace screens
{
    using namespace render;
    using core::mix;
    using core::withAlpha;
    using core::rgb;
    using ui::label;

    namespace
    {
        void logo(float cx, float cy, float r)
        {
            // the icon's sun, small: a gradient disc cut by two stripes, over a sea line
            const Rect disc{cx - r, cy - r, r * 2, r * 2};
            const float cut1 = cy + r * 0.18f, cut2 = cy + r * 0.48f;
            pushClip({disc.x, disc.y, disc.w, cut1 - disc.y});
            gradient(disc, theme::kPinkHi, theme::kPink, r);
            popClip();
            pushClip({disc.x, cut1 + r * 0.12f, disc.w, cut2 - cut1 - r * 0.12f});
            gradient(disc, theme::kPinkHi, theme::kPink, r);
            popClip();
            pushClip({disc.x, cut2 + r * 0.16f, disc.w, r});
            gradient(disc, theme::kPinkHi, theme::kPink, r);
            popClip();
        }

        bool windowButton(std::string_view idLabel, char32_t icon, Rect r, bool close)
        {
            const ui::Interaction in = ui::interact(ui::id(idLabel), r, false);
            const float hover = in.w->hover;
            if (close)
                rect(r, withAlpha(rgb(0xC42B1C), hover * (1.0f - 0.25f * in.w->press)));
            else
                rect(r, theme::ink(0.07f * hover + 0.05f * in.w->press));
            const Color c = close ? mix(theme::kDim, rgb(0xFFFFFF), hover) : mix(theme::kDim, theme::kText, hover);
            drawIcon(kFontIcons, px(10), r.cx(), r.cy(), platform::window.focused ? c : theme::kFaint, icon);
            return in.clicked;
        }

        bool navButton(std::string_view text, char32_t icon, Rect r, bool active, bool dot, std::vector<Rect> &hits)
        {
            hits.push_back(r);
            const ui::Interaction in = ui::interact(ui::id(text), r, true);
            ui::Widget &w = *in.w;
            if (w.fresh)
                w.t = active ? 1.0f : 0.0f;
            ui::animate(w.t, active ? 1.0f : 0.0f, 16);
            const Rect pillRect = r.inset(0, px(7));
            rect(pillRect, theme::ink(0.06f * w.hover + 0.09f * w.t), px(8));
            const Color c = mix(mix(theme::kDim, theme::kText, w.hover), theme::kText, w.t);
            drawIcon(kFontIcons, px(13), r.x + px(19), r.cy(), c, icon);
            label(kFontBody, px(theme::kSmall), {r.x + px(32), r.y, r.w - px(38), r.h}, c, text);
            if (dot)
            {
                circle(r.x + px(25), r.cy() - px(6), px(3.5f), theme::kNight);
                circle(r.x + px(25), r.cy() - px(6), px(2.5f), theme::kPink);
            }
            if (w.focus > 0.01f)
                outline(pillRect.inset(-px(2)), withAlpha(theme::kPink, w.focus * 0.85f), px(2), px(9));
            return in.clicked;
        }

        // [lcs] one of the two games: the shown one bright on a pill, the other quiet until hovered; a click switches
        // (not while a build runs or the game plays: the tooltip says why)
        bool gameTab(std::string_view text, Rect r, bool active, bool enabled, std::vector<Rect> &hits)
        {
            hits.push_back(r);
            const ui::Interaction in = ui::interact(ui::id(text), r, true);
            ui::Widget &w = *in.w;
            if (w.fresh)
                w.t = active ? 1.0f : 0.0f;
            ui::animate(w.t, active ? 1.0f : 0.0f, 16);
            const Rect pillRect = r.inset(0, px(8));
            rect(pillRect, theme::ink(0.10f * w.t + (enabled ? 0.05f * w.hover : 0.0f)), px(8));
            const bool focused = platform::window.focused;
            const Color idle = enabled ? mix(theme::kFaint, theme::kDim, w.hover) : theme::kFaint;
            const Color c = mix(idle, focused ? theme::kText : theme::kDim, w.t);
            label(kFontSemibold, px(12.5f), {r.x + px(10), r.y, r.w - px(12), r.h}, c, text);
            if (w.focus > 0.01f)
                outline(pillRect.inset(-px(2)), withAlpha(theme::kPink, w.focus * 0.85f), px(2), px(9));
            return in.clicked && enabled && !active;
        }
    }

    float drawTitleBar(Rect window)
    {
        App &app = *shell.app;
        const float h = px(theme::kTitleBar);
        Rect bar{window.x, window.y, window.w, h};
        ui::beginLayer(kLayerSheet, false); // usable under a sheet, not under a dialog
        struct EndLayer
        {
            ~EndLayer() { ui::endLayer(); }
        } endLayer;
        // a hairline under it once something scrolls or a sheet covers the content
        rect({bar.x, bar.bottom() - 1, bar.w, 1}, theme::ink(0.04f));

        Rect left = bar;
#if defined(__APPLE__)
        // macOS keeps its own window buttons (the traffic lights, top left over our title bar, centred on it by
        // platform_macos.mm's placeWindowButtons: 15 pt in, the last one ending at 69 pt): room for them
        core::cutLeft(left, px(86));
#else
        core::cutLeft(left, px(16));
#endif
        // [stories] the GTA Stories tab shows the mixed logo (both cities); a game's tab, the sun
        {
            const float st = theme::storiesness;
            if (st < 0.999f)
            {
                render::pushOpacity(1 - st);
                logo(left.x + px(9), bar.cy(), px(8));
                render::popOpacity();
            }
            if (st > 0.001f)
            {
                render::pushOpacity(st);
                storiesLogo({left.x, bar.cy() - px(9), px(18), px(18)});
                render::popOpacity();
            }
        }
        core::cutLeft(left, px(18));
        std::vector<Rect> hits;
        // [lcs] the game switch in place of the name; [stories] a third tab, GTA Stories: both cities at once
        const bool canSwitch = app.canSwitchGame();
        for (const char *id : {"lcs", "vcs", "stories"}) // [lcs] Liberty City Stories first (the default)
        {
            const bool both = std::string_view(id) == "stories";
            const char *name = both ? "GTA Stories" : launcher::gameTitle(id);
            const Rect tab = core::cutLeft(left, measure(kFontSemibold, px(12.5f), name) + px(22));
            const bool shown = app.shownId() == id;
            if (gameTab(name, tab, shown, canSwitch, hits))
                switchGame(app, id);
            if (!canSwitch && !shown)
                ui::tooltip(ui::id(name), tab, app.gameRunning ? "Stop the game to switch" : "Switch when the task has finished");
            else if (both && !shown)
                ui::tooltip(ui::id(name), tab, "Both cities in one game: Liberty City, with Vice City across the bridge and the sea");
        }
        if (window.w > px(1180)) // room for it next to the switch and the sheets' buttons
            label(kFontBody, px(12.5f), {left.x + px(6), bar.y, px(220), h}, theme::kFaint,
                  "GTA Stories Recompiled  \xC2\xB7  " + app.sourceVersion);

        Rect right = bar;
#if defined(__APPLE__)
        const Rect maxR{};
        core::cutRight(right, px(10));
#else
        const float bw = px(46);
        const Rect closeR = core::cutRight(right, bw), maxR = core::cutRight(right, bw), minR = core::cutRight(right, bw);
        hits.push_back(closeR);
        hits.push_back(minR);
        if (windowButton("#close", theme::icon::kClose, closeR, true))
            requestQuit(app);
        if (windowButton("#max", platform::window.maximized ? theme::icon::kRestore : theme::icon::kMaximize, maxR, false))
            platform::toggleMaximize();
        if (windowButton("#min", theme::icon::kMinimize, minR, false))
            platform::minimize();
        core::cutRight(right, px(10));
        rect({right.right(), bar.y + px(14), 1, h - px(28)}, theme::ink(0.08f));
        core::cutRight(right, px(10));
#endif

        struct Nav
        {
            const char *text;
            char32_t icon;
            Sheet sheet;
        };
        const Nav navs[] = {{"Help", theme::icon::kHelp, Sheet::Help},
                            {"Settings", theme::icon::kSettings, Sheet::Settings},
                            {"Saves", theme::icon::kSave, Sheet::Saves},
                            {"Mods", theme::icon::kList, Sheet::Mods}}; // [mods] the shown game's mods
        for (const Nav &n : navs)
        {
            const float w = measure(kFontBody, px(theme::kSmall), n.text) + px(46);
            const Rect r = core::cutRight(right, w);
            const bool dot = n.sheet == Sheet::Settings && app.updateInfo["newer"].boolean();
            if (navButton(n.text, n.icon, r, shell.sheet == n.sheet, dot, hits))
                openSheet(shell.sheet == n.sheet ? Sheet::None : n.sheet);
            core::cutRight(right, px(4));
        }

        platform::Chrome chrome;
        chrome.caption = bar;
        chrome.maximizeButton = maxR;
        chrome.buttons = hits;
        platform::setChrome(chrome);
        return h;
    }
}
