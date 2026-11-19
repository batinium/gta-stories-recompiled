// The Game settings sheet: the shown game's own settings (draw distance, resolution, frame rate...) before it starts.
// Everything shown comes from the game's settings registry (model/game_settings.h: `lcs.exe --dump-settings-json`);
// this file only decides how the game's pages and sections fall into the sheet's groups - the Android side panel's
// (Display, World, Audio, Controls) plus Game for the rest - and draws them. Key bindings and the presets / reset buttons
// stay in the game's F2 menu.
#include "shell.h"

#include "model/game_settings.h"
#include "model/system.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace screens
{
    using namespace render;
    using core::withAlpha;
    using launcher::Json;
    using ui::ButtonStyle;

    namespace
    {
        launcher::GameSettings g_game;
        int g_group = 0;          // the shown group (kGroups)
        bool g_confirmReset = false;
        std::string g_note;       // the last change's outcome
        Color g_noteColor = 0;

        const char *const kGroups[] = {"Display", "World", "Audio", "Controls", "Game"};
        constexpr int kGroupCount = 5;

        // Which group a setting of the game's registry shows in (-1: not here). By the game's page and section, never by
        // key, so a setting the game adds shows up by itself.
        int groupOf(const Json &s)
        {
            const std::string type = s["type"].str(), page = s["page"].str(), section = s["section"].str();
            if (type == "action" || type == "key" || s["hidden"].boolean() || s["otherGame"].boolean())
                return -1; // buttons and key bindings: the game's F2 menu; the other game's settings: nothing here
            if (section == "World")
                return 1;
            if (page == "Display" || page == "Graphics" || section == "Effects")
                return 0;
            if (page == "Audio")
                return 2;
            if (page == "Gamepad" && (section == "Buttons" || section.rfind("Shortcuts", 0) == 0))
                return -1; // the gamepad's button bindings (choices of buttons): the game's F2 menu, as the keys
            if (page.rfind("Keyboard", 0) == 0 || page == "Gamepad")
                return 3;
            return 4; // Game, the rest of Enhancements, Advanced
        }

        // the game's format ("%d%%", "%d") without handing it to printf
        std::string formatted(const Json &s, int v)
        {
            std::string f = s["format"].str("%d"), out;
            for (size_t i = 0; i < f.size(); ++i)
            {
                if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == 'd')
                    out += std::to_string(v), ++i;
                else if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == '%')
                    out += '%', ++i;
                else
                    out += f[i];
            }
            return out;
        }

        // the help's first sentence under the row; the whole of it in the title's tooltip
        std::string firstSentence(const std::string &help)
        {
            for (size_t i = 0; i + 1 < help.size(); ++i)
                if (help[i] == '.' && help[i + 1] == ' ' && i > 12)
                    return help.substr(0, i + 1);
            return help;
        }

        void change(App &app, const std::string &key, int v, const std::string &label, const std::string &shown)
        {
            std::string why;
            if (g_game.set(key, v, why))
            {
                g_note = label + ": " + shown + ". Applies when " + app.title() + " starts next.";
                g_noteColor = theme::kTeal;
            }
            else
            {
                g_note = why;
                g_noteColor = theme::kRed;
            }
        }

        // a row of chips, one per choice, wrapping; returns the height used
        float chips(App &app, const Json &s, Rect r, int current, bool enabled)
        {
            const Json &choices = s["choices"];
            const float h = px(28), gap = px(6);
            float x = r.x, y = r.y;
            for (size_t i = 0; i < choices.size(); ++i)
            {
                const std::string label = choices[i]["label"].str();
                const int v = int(choices[i]["value"].num());
                const float w = std::min(r.w, std::ceil(measure(kFontSemibold, px(theme::kSmall), label) + px(22)));
                if (x > r.x && x + w > r.right())
                {
                    x = r.x;
                    y += h + gap;
                }
                const Rect chip{x, y, w, h};
                const ui::Interaction in = ui::interact(ui::id(label, int(i)), chip, true, enabled);
                const bool on = v == current;
                render::pushOpacity(enabled ? 1.0f : 0.5f);
                if (on)
                    rectEx(chip, withAlpha(theme::kPink, 0.16f), withAlpha(theme::kPink, 0.12f), h * 0.5f,
                           withAlpha(theme::kPink, 0.55f), ui::hairline());
                else
                    rectEx(chip, theme::ink(0.035f + 0.05f * in.w->hover), theme::ink(0.025f + 0.05f * in.w->hover), h * 0.5f,
                           theme::ink(0.08f), ui::hairline());
                ui::label(kFontSemibold, px(theme::kSmall), chip, on ? theme::kPink : core::mix(theme::kDim, theme::kText, in.w->hover),
                          label, ui::Align::Centre);
                render::popOpacity();
                if (in.w->focus > 0.01f)
                    outline(chip.inset(-px(3)), withAlpha(theme::kPink, 0.85f * in.w->focus), px(2), h * 0.5f + px(3));
                if (in.clicked && enabled && !on)
                    change(app, s["key"].str(), v, s["label"].str(), label);
                x += w + gap;
            }
            return y + h - r.y;
        }

        // a slider for an Integer setting: dragged, clicked, or the arrow keys when focused; written on release
        float slider(App &app, const Json &s, Rect r, int current, bool enabled)
        {
            const int lo = int(s["min"].num()), hi = int(s["max"].num());
            const int range = std::max(1, hi - lo);
            const int step = range >= 500 ? 10 : range >= 100 ? 5 : 1;
            const Rect hit{r.x, r.y, r.w, px(28)};
            const ui::Interaction in = ui::interact(ui::id(s["key"].str() + "#slider"), hit, true, enabled);
            ui::Widget &w = *in.w;
            int shown = current;
            const Rect track{hit.x + px(8), hit.cy() - px(2), hit.w - px(16), px(4)};
            if (in.held)
            {
                const float mx = platform::input.mouseX - render::draw.dx;
                const float t = core::clamp01((mx - track.x) / track.w);
                int v = lo + int(std::lround(t * range / step)) * step;
                v = std::clamp(v, lo, hi);
                w.previous = v;
                w.t2 = 1;
                shown = v;
            }
            else if (w.t2 > 0)
            {
                w.t2 = 0;
                if (w.previous != current)
                    change(app, s["key"].str(), w.previous, s["label"].str(), formatted(s, w.previous));
                shown = g_game.value(s["key"].str());
            }
            if (in.focused && enabled && !in.held)
            {
                int v = current;
                if (platform::input.key(platform::kKeyLeft))
                    v = std::max(lo, current - step);
                if (platform::input.key(platform::kKeyRight))
                    v = std::min(hi, current + step);
                if (v != current)
                {
                    change(app, s["key"].str(), v, s["label"].str(), formatted(s, v));
                    shown = v;
                }
            }
            const float t = core::clamp01(float(shown - lo) / float(range));
            render::pushOpacity(enabled ? 1.0f : 0.5f);
            rect(track, theme::ink(0.10f), track.h * 0.5f);
            gradient({track.x, track.y, std::max(track.h, track.w * t), track.h}, theme::kPink, theme::kPinkHi, track.h * 0.5f,
                     Gradient::Horizontal);
            // the default's place: a small tick
            const float d = core::clamp01(float(int(s["default"].num()) - lo) / float(range));
            rect({std::round(track.x + track.w * d) - px(1), track.y - px(5), px(2), track.h + px(10)}, theme::ink(0.22f), px(1));
            const float knob = px(16) * (1.0f + 0.12f * w.press);
            const float kx = track.x + track.w * t;
            shadow({kx - knob * 0.5f, track.cy() - knob * 0.5f + px(1), knob, knob}, theme::shade(0.67f), px(3), knob * 0.5f);
            circle(kx, track.cy(), knob * 0.5f, core::mix(theme::kSurfaceTop, core::rgb(0xFFFFFF), 0.6f + 0.4f * w.hover));
            outline({kx - knob * 0.5f, track.cy() - knob * 0.5f, knob, knob}, withAlpha(theme::kPink, 0.6f), px(1.5f), knob * 0.5f);
            render::popOpacity();
            if (w.focus > 0.01f)
                outline(hit.inset(-px(3)), withAlpha(theme::kPink, 0.85f * w.focus), px(2), px(8));
            // the range's ends under it
            ui::label(kFontBody, px(11.5f), {track.x - px(4), hit.bottom() - px(2), px(80), px(16)}, theme::kFaint, formatted(s, lo));
            ui::label(kFontBody, px(11.5f), {track.right() - px(76), hit.bottom() - px(2), px(80), px(16)}, theme::kFaint,
                      formatted(s, hi), ui::Align::Right);
            return hit.h + px(16);
        }

        float settingRow(App &app, const Json &s, Rect r, bool gameRunning)
        {
            const std::string key = s["key"].str(), type = s["type"].str();
            const bool locked = s["locked"].boolean(), overridden = s["overridden"].boolean();
            const bool enabled = !gameRunning && !locked && !overridden;
            const int current = locked ? int(s["lockedValue"].num()) : g_game.value(key);
            const bool changed = !locked && current != int(s["default"].num());
            ui::pushId(key);
            float y = r.y;
            const float rightW = type == "toggle" ? px(52) : type == "integer" ? px(110) : 0;
            // the title: its whole help on hover
            const Rect title{r.x, y, r.w - rightW, px(24)};
            ui::interact(ui::id("#title"), title, false);
            if (changed) // changed from the game's default: a dot before the name
                circle(r.x - px(11), title.cy(), px(3), theme::kPink);
            ui::label(kFontSemibold, px(theme::kBody), title, enabled || gameRunning ? theme::kText : theme::kDim, s["label"].str());
            ui::tooltip(ui::id("#title"), title, s["help"].str());
            if (type == "toggle")
            {
                bool on = current != 0;
                if (ui::toggle("#toggle", {r.right() - px(44), y - px(3), px(44), px(30)}, on, enabled))
                    change(app, key, on ? 1 : 0, s["label"].str(), on ? "on" : "off");
            }
            else if (type == "integer")
                ui::label(kFontSemibold, px(theme::kBody), {r.right() - rightW, y, rightW, px(24)}, theme::kPink, formatted(s, current),
                          ui::Align::Right);
            y += px(26);
            y += ui::paragraph(kFontBody, px(theme::kSmall), {r.x, y, r.w - rightW, 0}, theme::kFaint, firstSentence(s["help"].str()), 1.35f);
            if (locked && !s["lockedNote"].str().empty())
                y += ui::paragraph(kFontBody, px(theme::kSmall), {r.x, y + px(2), r.w, 0}, theme::kAmber, s["lockedNote"].str(), 1.35f) + px(2);
            if (overridden)
                y += ui::paragraph(kFontBody, px(theme::kSmall), {r.x, y + px(2), r.w, 0}, theme::kAmber,
                                   "Set by " + s["env"].str() + " in the launcher's environment.", 1.35f) + px(2);
            if (type == "choice")
                y += px(8) + chips(app, s, {r.x, y + px(8), r.w, 0}, current, enabled);
            else if (type == "integer")
                y += px(6) + slider(app, s, {r.x, y + px(6), r.w, 0}, current, enabled);
            ui::popId();
            return y - r.y + px(20);
        }

        void sectionTitle(Rect r, float &y, const std::string &title)
        {
            std::string caps;
            for (const char c : title)
                caps += char(c >= 'a' && c <= 'z' ? c - 32 : c);
            ui::eyebrow(r.x, y, theme::kFaint, caps);
            y += px(24);
        }
    }

    bool gameSettingsLoading() { return g_game.loading(); }

    void showGameSettingsGroup(const std::string &name)
    {
        for (int i = 0; i < kGroupCount; ++i)
        {
            std::string lower = kGroups[i];
            for (char &c : lower)
                c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
            if (name == lower)
                g_group = i;
        }
    }

    float drawGameSettings(App &app, Rect r)
    {
        float y = r.y;
        // asked again each time the sheet opens and when the game ends (it rewrites vcs.ini when F2 changes something)
        static uint64_t lastFrame = 0;
        static bool wasRunning = false;
        const bool fresh = ui::state.frame > lastFrame + 1 || (wasRunning && !app.gameRunning);
        lastFrame = ui::state.frame;
        wasRunning = app.gameRunning;
        g_game.load(app.gameBuild() / app.gameExe(), fresh && !g_game.loading());
        g_game.poll();
        if (g_game.loading())
            ui::keepAnimating(); // the answer comes within a moment: keep frames coming (the wake hook also does)

        std::string upper = app.title();
        for (char &c : upper)
            c = char(c >= 'a' && c <= 'z' ? c - 32 : c);
        ui::eyebrow(r.x, y, theme::kPink, upper);
        y += px(24);
        y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, theme::kDim,
                           std::string("The game's own settings, as ") + app.gameExe() +
                               " lists them. Changes apply the next time the game starts. While playing, press F2 to "
                               "change them live (F5 trainer, F6 free camera); key bindings and presets are there too.") +
             px(10);

        if (g_game.error == "missing")
        {
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, theme::kAmber,
                               std::string("Build ") + app.title() + " first: the settings come from the game itself (" +
                                   launcher::pathText(g_game.exe) + ").") + px(8);
            return y;
        }
        if (!g_game.error.empty())
        {
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, theme::kRed, g_game.error) + px(8);
            if (ui::button("Ask the game again", {r.x, y, ui::buttonWidth("Ask the game again", theme::icon::kRefresh), px(38)},
                           ButtonStyle::Secondary, theme::icon::kRefresh, !app.gameRunning))
                g_game.load(g_game.exe, true);
            return y + px(48);
        }
        if (!g_game.ready())
        {
            ui::spinner(r.x + px(9), y + px(11), px(7), theme::kDim, px(1.8f));
            ui::label(kFontBody, px(theme::kBody), {r.x + px(26), y, r.w, px(22)}, theme::kDim, "Asking the game for its settings...");
            return y + px(34);
        }
        // VCS_GAME_SETTING=<key>=<value>: one change made as the sheet makes it, once its answer is in (tests,
        // screenshots: the value then read back from the game over its control port)
        static bool fromEnv = false;
        if (const char *e = std::getenv("VCS_GAME_SETTING"); e && *e && !fromEnv)
        {
            fromEnv = true;
            const std::string kv = e;
            if (const size_t eq = kv.find('='); eq != std::string::npos)
                change(app, kv.substr(0, eq), std::atoi(kv.c_str() + eq + 1), kv.substr(0, eq), kv.substr(eq + 1));
        }
        if (app.gameRunning)
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, theme::kAmber,
                               "The game is running: change its settings there with F2 (it would overwrite changes made "
                               "here). Quit it to change them here.") + px(8);

        // the groups, as on Android's side panel
        {
            std::vector<std::string> names(kGroups, kGroups + kGroupCount);
            ui::segmented("#game-groups", {r.x, y, r.w, px(38)}, names, g_group);
            y += px(54);
        }

        const Json &list = g_game.schema["settings"];
        // sections in the order they first appear in the registry; the rows of each in registry order
        std::vector<std::string> sections;
        for (size_t i = 0; i < list.size(); ++i)
            if (groupOf(list[i]) == g_group)
            {
                const std::string sec = list[i]["page"].str() == "Display" || list[i]["page"].str() == "Graphics" ||
                                                list[i]["section"].str() == "World"
                                            ? list[i]["section"].str()
                                            : list[i]["page"].str() + "  \xC2\xB7  " + list[i]["section"].str();
                if (std::find(sections.begin(), sections.end(), sec) == sections.end())
                    sections.push_back(sec);
            }
        std::vector<std::string> shownKeys;
        for (const std::string &sec : sections)
        {
            sectionTitle(r, y, sec);
            for (size_t i = 0; i < list.size(); ++i)
            {
                const Json &s = list[i];
                if (groupOf(s) != g_group)
                    continue;
                const std::string mine = s["page"].str() == "Display" || s["page"].str() == "Graphics" || s["section"].str() == "World"
                                             ? s["section"].str()
                                             : s["page"].str() + "  \xC2\xB7  " + s["section"].str();
                if (mine != sec)
                    continue;
                shownKeys.push_back(s["key"].str());
                y += settingRow(app, s, {r.x, y, r.w, 0}, app.gameRunning);
            }
            y += px(6);
        }
        if (sections.empty())
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, theme::kFaint, "Nothing here for this game.") + px(8);

        if (!g_note.empty())
            y += ui::paragraph(kFontBody, px(theme::kSmall), {r.x, y, r.w, 0}, g_noteColor, g_note, 1.35f) + px(8);
        y += px(4);
        rect({r.x, y, r.w, 1}, theme::ink(0.06f));
        y += px(18);
        {
            const std::string group = kGroups[g_group];
            const std::string reset = g_confirmReset ? "Reset " + group + ": sure?" : "Reset " + group + " to defaults";
            const float w1 = ui::buttonWidth(reset, theme::icon::kUndo);
            if (ui::button(reset, {r.x, y, w1, px(38)}, g_confirmReset ? ButtonStyle::Danger : ButtonStyle::Ghost, theme::icon::kUndo,
                           !app.gameRunning))
            {
                if (g_confirmReset)
                {
                    std::string why;
                    if (g_game.resetAll(shownKeys, why))
                    {
                        g_note = group + ": back to the game's defaults.";
                        g_noteColor = theme::kTeal;
                    }
                    else
                    {
                        g_note = why;
                        g_noteColor = theme::kRed;
                    }
                }
                g_confirmReset = !g_confirmReset;
            }
            std::error_code ec;
            if (ui::button("Open vcs.ini", {r.x + w1 + px(8), y, ui::buttonWidth("Open vcs.ini", theme::icon::kDocument), px(38)},
                           ButtonStyle::Ghost, theme::icon::kDocument, std::filesystem::is_regular_file(g_game.ini, ec)))
                launcher::openFile(g_game.ini);
            y += px(48);
        }
        y += ui::paragraph(kFontBody, px(theme::kSmall), {r.x, y, r.w, 0}, theme::kFaint,
                           "Saved in " + launcher::pathText(g_game.ini) + ", the file the game reads and writes. A dot marks "
                           "a setting changed from the game's default; hover a name for its whole description.", 1.35f);
        return y;
    }
}
