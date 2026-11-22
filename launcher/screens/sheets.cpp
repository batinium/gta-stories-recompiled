// Sheets: Saves, Settings, Help and the build folder slide in from the right over a dimmed window, on a spring.
// Escape, the scrim or the title bar's button close them.
#include "shell.h"

#include "core/png.h"
#include "model/os.h"
#include "model/system.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <vector>

namespace screens
{
    using namespace render;
    using core::alphaOf;
    using core::mix;
    using core::withAlpha;
    using core::rgb;
    using ui::ButtonStyle;

    namespace
    {
        void section(Rect r, float &y, std::string_view title)
        {
            ui::eyebrow(r.x, y, theme::kFaint, title);
            y += px(24);
        }

        void text(Rect r, float &y, std::string_view t, Color c = theme::kDim)
        {
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, c, t) + px(8);
        }

        bool action(Rect r, float &y, std::string_view label, ButtonStyle style, char32_t icon = 0, bool enabled = true)
        {
            const bool clicked = ui::button(label, {r.x, y, ui::buttonWidth(label, icon), px(38)}, style, icon, enabled);
            y += px(48);
            return clicked;
        }

        void divider(Rect r, float &y)
        {
            y += px(10);
            rect({r.x, y, r.w, 1}, theme::ink(0.06f));
            y += px(22);
        }

        // a save's time: 2026-11-18 21:04 (the PC's time zone)
        std::string when(double unixTime)
        {
            const std::time_t tt = std::time_t(unixTime);
            char text[32] = {};
            if (const std::tm *tm = std::localtime(&tt))
                std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", tm);
            return text;
        }

        // ---- the saves' pictures (<data>/save_shots/slot<N>.png, written by the game as it saves) ----------------------
        // The renderer draws rectangles, not textures, so a picture is a small grid of them: the PNG cut to 16:9 and
        // averaged to kThumbCols x kThumbRows colours, equal neighbours in a row drawn as one rectangle.
        constexpr int kThumbCols = 64, kThumbRows = 36;
        struct Thumb
        {
            std::string path;
            double time = -1;
            bool ok = false;
            std::vector<uint8_t> rgb; // kThumbCols x kThumbRows x 3
        };
        std::map<int, Thumb> g_thumbs; // by slot

        const Thumb *thumbFor(int slot, const std::string &path, double time)
        {
            if (path.empty())
            {
                g_thumbs.erase(slot);
                return nullptr;
            }
            Thumb &t = g_thumbs[slot];
            if (t.path != path || t.time != time)
            {
                t = {};
                t.path = path;
                t.time = time;
                int w = 0, h = 0;
                std::vector<uint8_t> px8;
                if (core::readPng(std::filesystem::u8path(path), w, h, px8))
                {
                    // the 16:9 part in the middle
                    double sw = w, sh = h, sx = 0, sy = 0;
                    if (sw * 9 > sh * 16)
                        sw = sh * 16 / 9, sx = (w - sw) / 2;
                    else
                        sh = sw * 9 / 16, sy = (h - sh) / 2;
                    t.rgb.assign(size_t(kThumbCols) * kThumbRows * 3, 0);
                    for (int cy = 0; cy < kThumbRows; ++cy)
                        for (int cx = 0; cx < kThumbCols; ++cx)
                        {
                            const int x0 = int(sx + sw * cx / kThumbCols), x1 = std::max(x0 + 1, int(sx + sw * (cx + 1) / kThumbCols));
                            const int y0 = int(sy + sh * cy / kThumbRows), y1 = std::max(y0 + 1, int(sy + sh * (cy + 1) / kThumbRows));
                            uint32_t sum[3] = {}, n = 0;
                            const int stepX = std::max(1, (x1 - x0) / 6), stepY = std::max(1, (y1 - y0) / 6);
                            for (int y = y0; y < y1 && y < h; y += stepY)
                                for (int x = x0; x < x1 && x < w; x += stepX, ++n)
                                    for (int k = 0; k < 3; ++k)
                                        sum[k] += px8[(size_t(y) * w + x) * 4 + k];
                            for (int k = 0; k < 3; ++k)
                                t.rgb[(size_t(cy) * kThumbCols + cx) * 3 + k] = uint8_t(n ? sum[k] / n : 0);
                        }
                    t.ok = true;
                }
            }
            return t.ok ? &t : nullptr;
        }

        void drawThumb(const Thumb &t, Rect r)
        {
            const auto gx = [&](int i) { return std::floor(r.x + r.w * float(i) / kThumbCols + 0.5f); };
            const auto gy = [&](int j) { return std::floor(r.y + r.h * float(j) / kThumbRows + 0.5f); };
            for (int j = 0; j < kThumbRows; ++j)
            {
                const uint8_t *row = &t.rgb[size_t(j) * kThumbCols * 3];
                for (int i = 0; i < kThumbCols;)
                {
                    int e = i + 1;
                    while (e < kThumbCols && std::abs(int(row[e * 3]) - row[i * 3]) + std::abs(int(row[e * 3 + 1]) - row[i * 3 + 1]) +
                                                     std::abs(int(row[e * 3 + 2]) - row[i * 3 + 2]) <= 10)
                        ++e;
                    const float x0 = gx(i), x1 = gx(e), y0 = gy(j), y1 = gy(j + 1);
                    rect({x0, y0, x1 - x0, y1 - y0}, core::rgb(uint32_t(row[i * 3]) << 16 | uint32_t(row[i * 3 + 1]) << 8 | row[i * 3 + 2]));
                    i = e;
                }
            }
        }

        float saves(App &app, Rect r)
        {
            float y = r.y;
            if (!shell.backupsAsked && !app.busy())
            {
                shell.backupsAsked = true;
                app.startBackupList();
            }
            // [lcs] the shown game's card: Vice City Stories' in GTAStoriesRecomp/VCS, Liberty City Stories' in GTAStoriesRecomp/LCS
            section(r, y, app.gameId == "lcs" ? "MEMORY CARD  \xC2\xB7  LIBERTY CITY STORIES" : "MEMORY CARD  \xC2\xB7  VICE CITY STORIES");
            text(r, y, "Your saves are kept apart from the game, so updates and rebuilds never touch them. Each game "
                       "has a card of its own.");
            y += row({r.x, y, r.w, 0}, "Folder", launcher::pathText(app.savesFolder()));
            y += px(6);
            const float w1 = ui::buttonWidth("Back up now", theme::icon::kSave);
            if (ui::button("Back up now", {r.x, y, w1, px(38)}, ButtonStyle::Primary, theme::icon::kSave, !app.busy()))
            {
                app.startBackup();
                shell.backupsAsked = false; // the list again afterwards
            }
            if (ui::button("Open the folder", {r.x + w1 + px(8), y, ui::buttonWidth("Open the folder", theme::icon::kFolder), px(38)},
                           ButtonStyle::Ghost, theme::icon::kFolder))
                launcher::openFolder(app.savesFolder());
            y += px(48);
            divider(r, y);

            // [lcs] the saved games on the card (the game's save folder: BASLUS-21590GTA40001, BASLUS-21423GTA30001),
            // each playable from its slot (VCS_LOAD_SLOT: the boot's load takes it)
            section(r, y, "SAVED GAMES");
            if (!app.saves.size())
                text(r, y, app.busy() && app.task->kind == "backup_list" ? "Loading..." : "None yet: save in the game "
                                                                                         "and it shows here.", theme::kFaint);
            for (size_t i = 0; i < app.saves.size(); ++i)
            {
                const launcher::Json &s = app.saves[i];
                const int slot = int(s["slot"].num());
                const Rect item{r.x - px(10), y, r.w + px(20), px(66)};
                ui::pushId(1000 + int(i));
                rect(item, theme::ink(ui::mouseOver(item) ? 0.035f : 0.0f), px(10));
                // its picture (a game that has not saved here with this build has none: the slot number instead)
                const Rect shot{item.x + px(10), item.y + px(6), px(96), px(54)};
                if (const Thumb *t = thumbFor(slot, s["shot"].str(), s["shot_time"].num()))
                {
                    drawThumb(*t, shot);
                    outline(shot, theme::ink(0.12f), 1);
                }
                else
                {
                    rect(shot, theme::ink(0.05f), px(6));
                    circle(shot.cx(), shot.cy(), px(13), withAlpha(theme::kPink, 0.14f));
                    ui::label(kFontSemibold, px(theme::kSmall), {shot.cx() - px(13), shot.y, px(26), shot.h}, theme::kPink,
                              std::to_string(slot), ui::Align::Centre);
                }
                const float tx = shot.right() + px(14);
                ui::label(kFontSemibold, px(theme::kBody), {tx, item.y + px(12), item.right() - tx - px(110), px(22)},
                          theme::kText, s["name"].str().empty() ? "Slot " + std::to_string(slot) : s["name"].str());
                ui::label(kFontBody, px(theme::kSmall), {tx, item.y + px(35), item.right() - tx - px(110), px(20)},
                          theme::kFaint, "Slot " + std::to_string(slot) + "  ·  " + when(s["time"].num()));
                const float bw = ui::buttonWidth("Play", theme::icon::kPlay);
                const Rect pb{item.right() - bw - px(10), item.y + px(15), bw, px(36)};
                if (ui::button("Play", pb, ButtonStyle::Ghost, theme::icon::kPlay, canPlay(app)))
                {
                    shell.sheet = Sheet::None;
                    play(app, slot);
                }
                ui::tooltip(ui::id("Play"), pb, "Start the game and load this save");
                ui::popId();
                y += item.h + px(4);
            }
            y += px(4);
            divider(r, y);

            section(r, y, "BACKUPS");
            if (app.busy() && (app.task->kind == "backup_list" || app.task->kind == "backup"))
            {
                ui::spinner(r.x + px(9), y + px(11), px(7), theme::kDim, px(1.8f));
                ui::label(kFontBody, px(theme::kBody), {r.x + px(26), y, r.w, px(22)}, theme::kDim, "Loading...");
                y += px(34);
            }
            else if (!app.backups.size())
                text(r, y, "None yet. A backup is a zip of the card, with the order the game needs.", theme::kFaint);
            else
                for (size_t i = 0; i < app.backups.size(); ++i)
                {
                    const launcher::Json &b = app.backups[i];
                    const std::string name = b["name"].str();
                    const Rect item{r.x - px(10), y, r.w + px(20), px(56)};
                    ui::pushId(int(i));
                    const bool over = ui::mouseOver(item);
                    rect(item, theme::ink(over ? 0.035f : 0.0f), px(10));
                    drawIcon(kFontIcons, px(15), item.x + px(26), item.cy(), theme::kPink, theme::icon::kSave);
                    ui::label(kFontSemibold, px(theme::kBody), {item.x + px(48), item.y + px(8), item.w - px(170), px(22)}, theme::kText, name);
                    ui::label(kFontBody, px(theme::kSmall), {item.x + px(48), item.y + px(29), item.w - px(170), px(20)}, theme::kFaint, b["saves"].str());
                    const float bw = ui::buttonWidth("Restore", theme::icon::kUndo);
                    if (ui::button("Restore", {item.right() - bw - px(10), item.y + px(10), bw, px(36)}, ButtonStyle::Ghost, theme::icon::kUndo,
                                   !app.busy() && !app.gameRunning))
                    {
                        shell.restoreName = name;
                        openDialog(Dialog::Restore);
                    }
                    ui::popId();
                    y += item.h + px(4);
                }
            if (app.gameRunning)
                text(r, y, "Quit the game to restore a backup.", theme::kAmber);
            y += px(4);
            if (action(r, y, "Open the backups folder", ButtonStyle::Ghost, theme::icon::kFolder))
                launcher::openFolder(app.savesFolder() / "backups");
            return y;
        }

        float settings(App &app, Rect r)
        {
            float y = r.y;
            bool changed = false;
            section(r, y, "WHEN THE GAME STARTS");
            const char *values[] = {"minimize", "close", "stay"};
            const char *titles[] = {"Minimize the launcher", "Close the launcher", "Keep the launcher open"};
            const char *details[] = {"It comes back when the game ends", "", ""};
            for (int i = 0; i < 3; ++i)
            {
                const float h = details[i][0] ? px(56) : px(44);
                if (ui::choiceRow(values[i], {r.x - px(10), y, r.w + px(20), h}, titles[i], details[i], app.settings.afterLaunch == values[i]))
                {
                    app.settings.afterLaunch = values[i];
                    changed = true;
                }
                y += h + px(2);
            }
            divider(r, y);

            section(r, y, "UPDATES");
            if (!app.canUpdate())
            {
                // a Linux clone (an AppImage updates itself): git
                y += row({r.x, y, r.w, 0}, "This version", app.sourceVersion);
                text(r, y, "Update this folder with git pull, then build again (Update the build on Home).", theme::kFaint);
            }
            else if (!app.updatedTo.empty())
            {
                // updated, the restart put off ("Later"): this window is still the old launcher
                const std::string tag = "VERSION " + app.updatedTo + " INSTALLED";
                ui::pill({r.x, y, ui::pillWidth(tag), px(22)}, tag, theme::kTeal);
                y += px(34);
                text(r, y, "Restart the launcher to use the new version, then update the build.", theme::kFaint);
                if (action(r, y, "Restart", ButtonStyle::Primary, theme::icon::kRefresh))
                    restartLauncher(app);
            }
            else if (app.updateInfo["newer"].boolean())
            {
                const std::string tag = "VERSION " + app.updateInfo["latest"].str() + " AVAILABLE";
                ui::pill({r.x, y, ui::pillWidth(tag), px(22)}, tag, theme::kPink);
                y += px(34);
                const float w1 = ui::buttonWidth("Update now", theme::icon::kDownload);
                if (ui::button("Update now", {r.x, y, w1, px(38)}, ButtonStyle::Primary, theme::icon::kDownload, !app.busy() && !app.gameRunning))
                {
                    shell.sheet = Sheet::None;
                    app.startUpdate();
                }
                if (ui::button("What's new", {r.x + w1 + px(8), y, ui::buttonWidth("What's new", theme::icon::kOpen), px(38)}, ButtonStyle::Ghost, theme::icon::kOpen))
                    launcher::openUrl(app.updateInfo["url"].str());
                y += px(52);
            }
            else
                y += row({r.x, y, r.w, 0}, "This version", app.sourceVersion);
            if (app.canUpdate())
            {
                const Rect line{r.x, y, r.w, px(30)};
                ui::label(kFontBody, px(theme::kBody), {line.x, line.y, line.w - px(60), line.h}, theme::kText, "Check for a new release at start");
                changed |= ui::toggle("#check-updates", {line.right() - px(44), line.y, px(44), line.h}, app.settings.checkUpdates);
                y += px(34);
            }
            if (app.canUpdate())
            {
                text(r, y, "One request to GitHub; nothing about you or your PC is sent.", theme::kFaint);
                if (action(r, y, app.busy() && app.task->kind == "update_check" ? "Checking..." : "Check now", ButtonStyle::Secondary, theme::icon::kSync, !app.busy()))
                    app.startUpdateCheck(false);
            }
            divider(r, y);

            section(r, y, "APPEARANCE");
            {
                static const char *themes[] = {"system", "light", "dark"};
                int selected = 0;
                for (int i = 0; i < 3; ++i)
                    if (app.settings.theme == themes[i])
                        selected = i;
                if (ui::segmented("#theme", {r.x, y, std::min(r.w, px(340)), px(38)}, {"System", "Light", "Dark"}, selected))
                {
                    app.settings.theme = themes[selected];
                    changed = true;
                }
                y += px(48);
                text(r, y, selected == 0 ? std::string(launcher::os::kWindows ? "Follows Windows' app mode, now " : "Follows the desktop's style, now ") + (platform::window.darkMode ? "dark." : "light.")
                                         : std::string("Always ") + (selected == 1 ? "light." : "dark."),
                     theme::kFaint);
            }
            divider(r, y);

            // [stories] both cities at once is the title bar's third tab now (GTA Stories), not a switch here
            section(r, y, "EXPERIMENTAL");
            text(r, y, "GTA Stories - Liberty City and Vice City at once - is the third tab at the top. Its Home says "
                       "what each city still needs; Play there starts both.",
                 theme::kFaint);
            if (action(r, y, "Open GTA Stories", ButtonStyle::Ghost, theme::icon::kChevronRight, app.canSwitchGame() && !app.stories))
            {
                shell.sheet = Sheet::None;
                switchGame(app, "stories");
            }
            divider(r, y);

            section(r, y, "THIS LAUNCHER");
            {
                const Rect line{r.x, y, r.w, px(30)};
                ui::label(kFontBody, px(theme::kBody), {line.x, line.y, line.w - px(60), line.h}, theme::kText, "Reduce motion");
                bool reduce = reduceMotion(app);
                if (ui::toggle("#reduce-motion", {line.right() - px(44), line.y, px(44), line.h}, reduce))
                {
                    app.settings.reduceMotion = reduce;
                    changed = true;
                }
                y += px(34);
                text(r, y, "Everything moves at once instead of animating, and Home's scene stands still.", theme::kFaint);
            }
            if (action(r, y, "Add a shortcut to the desktop", ButtonStyle::Secondary, theme::icon::kLink))
            {
                std::string error;
                toast(app.shortcut(error) ? "Shortcut \"GTA Stories Recompiled\" added to the desktop" : "No shortcut: " + error,
                      error.empty() ? 0 : theme::kRed);
            }
            divider(r, y);
            section(r, y, "THE GAME'S SETTINGS");
            text(r, y, std::string("Draw distance, resolution, frame rate, sound and controls are ") + app.title() +
                           "'s own: change them here before you play, or press F2 in the game to change them live.",
                 theme::kFaint);
            if (action(r, y, "Game settings", ButtonStyle::Secondary, theme::icon::kSettings))
                openSheet(Sheet::Game);
            if (changed)
                app.saveSettings();
            return y;
        }

        float help(App &app, Rect r)
        {
            float y = r.y;
            section(r, y, "REPORT A PROBLEM");
            text(r, y, "Makes a zip of the build's and the game's logs and what this PC has - no game files, no saves - to attach to your bug report.");
            if (action(r, y, "Make a report", ButtonStyle::Primary, theme::icon::kBug, !app.busy()))
            {
                shell.sheet = Sheet::None;
                app.startReport();
            }
            divider(r, y);
            // [lcs] who maintains what (a neutral note, shown for either game)
            section(r, y, "WHO MAINTAINS WHAT");
            text(r, y, "Vice City Stories' PC port is maintained upstream (gothamor/gta-vcs-recomp): report problems with it there. "
                       "Liberty City Stories, on every platform, and the Android app are maintained in this repository.", theme::kFaint);
            divider(r, y);
            section(r, y, "FILES");
            struct File
            {
                const char *label;
                std::filesystem::path path;
            };
            const File files[] = {{"The game's last log", app.gameBuild() / "run" / "launcher_game.log"},
                                  {"Build log", app.gameBuild() / "build.log"},
                                  {"README", app.root / "README.md"}};
            for (const File &f : files)
            {
                std::error_code ec;
                if (ui::button(f.label, {r.x - px(4), y, r.w + px(8), px(36)}, ButtonStyle::Ghost, theme::icon::kDocument,
                               std::filesystem::is_regular_file(f.path, ec)))
                    launcher::openFile(f.path);
                y += px(40);
            }
            divider(r, y);
            section(r, y, "KEYBOARD");
            const char *keys[][2] = {{"Enter", "The highlighted action (Play, Build...)"},
                                     {"Esc", "Close a sheet or a dialog"},
                                     {"F1", "This help"},
                                     {"Ctrl+L", "The log of a running task"},
                                     {"Tab", "Move between controls"}};
            const auto keyRows = [&](const auto &list) {
                for (const auto &k : list)
                {
                    const float kw = measure(kFontSemibold, px(12), k[0]) + px(16);
                    rectEx({r.x, y + px(3), kw, px(22)}, theme::kSurfaceHi, theme::kSurfaceHi, px(5), theme::ink(0.10f), ui::hairline());
                    ui::label(kFontSemibold, px(12), {r.x, y + px(3), kw, px(22)}, theme::kText, k[0], ui::Align::Centre);
                    ui::label(kFontBody, px(theme::kSmall), {r.x + px(84), y, r.w - px(84), px(28)}, theme::kDim, k[1]);
                    y += px(32);
                }
            };
            keyRows(keys);
            divider(r, y);
            // the game's overlay menus: on the PC its settings and trainer exist only there (the defaults; rebind them in
            // F2 > Keyboard > Menus)
            section(r, y, "IN THE GAME");
            const char *gameKeys[][2] = {{"F2", "Settings: graphics, draw distance, sound, controls (live)"},
                                         {"F5", "Trainer: health, vehicles, teleport, missions, save anywhere"},
                                         {"F6", "Free camera"},
                                         {"F3", "Performance overlay (F4 records a CSV)"},
                                         {"F8", "Screenshot"},
                                         {"Alt+Enter", "Full screen / window"}};
            keyRows(gameKeys);
            text(r, y, "On a gamepad: Back + Start for the settings, Back + RB for the trainer, Back + LB for the free camera.",
                 theme::kFaint);
            if (action(r, y, "Game settings", ButtonStyle::Ghost, theme::icon::kSettings))
                openSheet(Sheet::Game);
            divider(r, y);
            section(r, y, "ABOUT");
            y += row({r.x, y, r.w, 0}, "App", "GTA Stories Recompiled - Liberty City Stories, Vice City Stories, GTA Stories");
            y += row({r.x, y, r.w, 0}, "Version", app.sourceVersion);
            y += row({r.x, y, r.w, 0}, "Folder", launcher::pathText(app.root));
            y += row({r.x, y, r.w, 0}, "Python", app.pythonText);
            y += px(6);
            text(r, y, "Unofficial. Grand Theft Auto is property of Rockstar Games / Take-Two Interactive; this port contains no game code or data.", theme::kFaint);
            return y;
        }

        float folder(App &, Rect r)
        {
            float y = r.y;
            drawFolderBody(r, y);
            return y;
        }

        // ---- [mods] the mod manager (tools/mods.py does the work; docs/ANDROID.md "Mods", docs/LCS.md "Mods") ----
        struct ModsUi
        {
            std::string pickMod;   // the slot picker is up for this mod's entry
            int pickEntry = -1;
            std::string pickFamily, pickWhat;
            int pickCurrent = -1;
            std::string confirmRemove;   // Remove pressed once: the second press removes
            std::vector<std::string> open; // mods whose values are shown
            bool keepInPlace = false;      // how the next mod is added: Import (a copy) / Keep where it is
        };
        ModsUi g_mods;

        std::string sizeText(double bytes)
        {
            char t[32];
            if (bytes >= 1024 * 1024)
                std::snprintf(t, sizeof(t), "%.1f MB", bytes / (1024.0 * 1024.0));
            else
                std::snprintf(t, sizeof(t), "%.0f KB", std::max(1.0, bytes / 1024.0));
            return t;
        }

        bool isOpen(const std::string &id)
        {
            return std::find(g_mods.open.begin(), g_mods.open.end(), id) != g_mods.open.end();
        }

        // the kind of change, in the player's words: behaviour (handling / weapon lines) or looks (models)
        struct KindTag
        {
            const char *tag;
            Color color;
        };
        KindTag kindTag(const std::string &kind)
        {
            if (kind == "handling")
                return {"DRIVES", theme::kTeal};
            if (kind == "weapon")
                return {"FIRES", theme::kAmber};
            if (kind == "model")
                return {"LOOKS", theme::kPink};
            if (kind == "replace")
                return {"FILE", theme::kDim};
            return {"DATA", theme::kDim};
        }

        std::string stateText(const launcher::Json &e, bool modOn, bool loaderOn)
        {
            if (!loaderOn)
                return "the loader is off";
            if (!modOn)
                return "off";
            if (e["off"].boolean())
                return "set aside: another mod has this slot";
            if (e["target"].num(-1) < 0)
                return "not used until you pick a slot";
            const std::string s = e["state"].str();
            if (s == "missing") // [mods] kept where it is, and it isn't there
                return "skipped until its original is found";
            if (s == "applied")
                return "applied at the last start";
            if (s == "skipped")
            {
                const std::string why = e["applied"][0].str();
                const size_t colon = why.find(": ");
                return "skipped at the last start" + (colon != std::string::npos ? ": " + why.substr(colon + 2) : std::string());
            }
            return "applies at the next start";
        }

        float modsPicker(App &app, Rect r)
        {
            float y = r.y;
            const launcher::Json &list = app.mods[g_mods.pickFamily == "vehicle" ? "vehicleSlots" : "weaponSlots"];
            if (ui::button("Back", {r.x - px(4), y, ui::buttonWidth("Back", theme::icon::kBack), px(34)}, ButtonStyle::Ghost,
                           theme::icon::kBack))
                g_mods.pickMod.clear();
            y += px(46);
            section(r, y, g_mods.pickFamily == "vehicle" ? "PICK THE VEHICLE" : "PICK THE WEAPON");
            text(r, y, "Which " + std::string(g_mods.pickFamily == "vehicle" ? "vehicle" : "weapon") + " gets " +
                           g_mods.pickWhat + "? The list is " + app.title() + "'s own " +
                           (g_mods.pickFamily == "vehicle" ? "vehicles" : "weapons") +
                           ". The mod's line or model goes into the one you pick; the others stay stock.");
            for (size_t i = 0; i < list.size(); ++i)
            {
                const launcher::Json &s = list[i];
                const int id = int(s["id"].num());
                const Rect item{r.x - px(10), y, r.w + px(20), px(34)};
                ui::pushId(5000 + int(i));
                const ui::Interaction in = ui::interact(ui::id("slot"), item);
                const bool current = id == g_mods.pickCurrent;
                rect(item, current ? withAlpha(theme::kPink, 0.14f) : theme::ink(in.hovered ? 0.05f : 0.0f), px(8));
                ui::label(kFontSemibold, px(theme::kBody), {item.x + px(12), item.y, item.w - px(150), item.h},
                          current ? theme::kPink : theme::kText, s["label"].str());
                ui::label(kFontBody, px(theme::kSmall), {item.right() - px(150), item.y, px(138), item.h}, theme::kFaint,
                          s["class"].str() + "  \xC2\xB7  " + std::to_string(id), ui::Align::Right);
                if (in.clicked && !app.busy())
                {
                    app.startModsCommand({"retarget", g_mods.pickMod, std::to_string(g_mods.pickEntry), std::to_string(id)},
                                         "Changing the slot");
                    g_mods.pickMod.clear();
                }
                ui::popId();
                y += item.h + px(2);
            }
            return y;
        }

        float modsSheet(App &app, Rect r)
        {
            float y = r.y;
            const launcher::Json &m = app.mods;
            if (!shell.modsAsked && !app.busy())
            {
                shell.modsAsked = true;
                app.startModsList();
            }
            if (m.isObject() && m["game"].str() != app.gameId)
                shell.modsAsked = false; // the other game's list: read this one's
            // VCS_MODS_PICK=<mod>:<entry>:<vehicle|weapon>: the picker up at start (screenshots)
            static bool pickFromEnv = false;
            if (const char *pick = std::getenv("VCS_MODS_PICK"); pick && !pickFromEnv && m.isObject())
            {
                pickFromEnv = true;
                const std::string p = pick;
                const size_t a = p.find(':'), b = p.rfind(':');
                if (a != std::string::npos && b > a)
                {
                    g_mods.pickMod = p.substr(0, a);
                    g_mods.pickEntry = std::atoi(p.substr(a + 1, b - a - 1).c_str());
                    g_mods.pickFamily = p.substr(b + 1);
                    g_mods.pickWhat = "this mod's line";
                }
            }
            if (!g_mods.pickMod.empty())
                return modsPicker(app, r);
            const bool busy = app.busy();
            const bool loaderOn = !m.isObject() || m["loader"].boolean(true);

            section(r, y, app.gameId == "lcs" ? "MODS  \xC2\xB7  LIBERTY CITY STORIES" : "MODS  \xC2\xB7  VICE CITY STORIES");
            text(r, y, "PC mods for GTA III, Vice City and San Andreas, into this game's own vehicle and weapon slots. "
                       "A handling.cfg line changes how a car drives and a weapon.dat line how a weapon fires; a .dff "
                       "model with its .txd changes how it looks. Each game has its own mods.");
            // [mods3] what converts, said plainly (docs/LCS.md "Mods: what converts")
            text(r, y, "Converted: every handling value with a match here (mass to lights, the flags bit by bit), weapon "
                       "range, rate, clip, damage and muzzle position, a car's body, doors, bonnet, boot, bumpers, "
                       "windscreen and damaged parts, its paint, dummies and wheel size, the mod's own wheels (LCS: on "
                       "this car only), a bike's forks, handlebars and wheels, each gun of a weapon pack. Not converted: "
                       "extras, collision, carcols numbers without a colour list, SA's skill levels and suspension "
                       "limits, animations, PC plugins (never run). Each entry lists what it skipped.",
                 theme::kFaint);
            text(r, y, "Changes apply when the game starts; your saves are never changed.", theme::kFaint);
            // [mods] how a mod is added: a copy, or read where it is (mods.py import --in-place)
            {
                const char *copyLabel = "Import";
                const char *placeLabel = "Keep where it is";
                const float c1 = ui::buttonWidth(copyLabel, 0, px(theme::kSmall)), c2 = ui::buttonWidth(placeLabel, 0, px(theme::kSmall));
                ui::label(kFontBody, px(theme::kBody), {r.x, y, px(150), px(30)}, theme::kText, "Add mods as");
                if (ui::button(copyLabel, {r.x + px(150), y, c1, px(30)}, g_mods.keepInPlace ? ButtonStyle::Ghost : ButtonStyle::Primary, 0,
                               !busy, px(theme::kSmall)))
                    g_mods.keepInPlace = false;
                if (ui::button(placeLabel, {r.x + px(150) + c1 + px(6), y, c2, px(30)},
                               g_mods.keepInPlace ? ButtonStyle::Primary : ButtonStyle::Ghost, 0, !busy, px(theme::kSmall)))
                    g_mods.keepInPlace = true;
                y += px(36);
                // both choices said plainly; the chosen one stands out
                text(r, y, "Import: the app keeps its own copy, so you can delete the zip or files you imported from.",
                     g_mods.keepInPlace ? theme::kFaint : theme::kText);
                text(r, y, "Keep where it is: the app only reads the file where it is now and keeps no copy (only its "
                           "converted models). Don't move, rename or delete it: if it's gone, the mod is skipped until "
                           "you find it again.",
                     g_mods.keepInPlace ? theme::kText : theme::kFaint);
            }
            {
                const bool place = g_mods.keepInPlace;
                const char *fileLabel = place ? "Add a file" : "Import a file";
                const char *folderLabel = place ? "Add a folder" : "Import a folder";
                const float w1 = ui::buttonWidth(fileLabel, theme::icon::kOpenFile);
                const float w2 = ui::buttonWidth(folderLabel, theme::icon::kFolder);
                if (ui::button(fileLabel, {r.x, y, w1, px(38)}, ButtonStyle::Primary, theme::icon::kOpenFile, !busy))
                    chooseModFile(place);
                ui::tooltip(ui::id(fileLabel), {r.x, y, w1, px(38)}, "A .zip, a .vcm (VC Mod Manager), handling.cfg, weapon.dat, a readme, .dff / .txd, .gxt");
                if (ui::button(folderLabel, {r.x + w1 + px(8), y, w2, px(38)}, ButtonStyle::Ghost, theme::icon::kFolder, !busy))
                    chooseModFolder(place);
                ui::tooltip(ui::id(folderLabel), {r.x + w1 + px(8), y, w2, px(38)}, "Loose files: a .dff with its .txd, handling lines...");
                const std::string folderPath = m["folder"].str();
                if (ui::button("Open the folder", {r.x + w1 + w2 + px(16), y, ui::buttonWidth("Open the folder", theme::icon::kOpen), px(38)},
                               ButtonStyle::Ghost, theme::icon::kOpen, !folderPath.empty()))
                    launcher::openFolder(launcher::pathFrom(folderPath));
                y += px(48);
            }
            text(r, y, ".rar and .7z open with 7-Zip or Windows' / macOS's tar when the computer has one; else unpack "
                       "them and import the folder.", theme::kFaint);
            if (app.modsBusy())
            {
                ui::spinner(r.x + px(9), y + px(11), px(7), theme::kDim, px(1.8f));
                ui::label(kFontBody, px(theme::kBody), {r.x + px(26), y, r.w, px(22)}, theme::kDim, app.task->title + "...");
                y += px(34);
            }
            else if (!app.modsError.empty())
                text(r, y, app.modsError, theme::kRed);
            else if (!app.modsNote.empty())
                text(r, y, app.modsNote, theme::kTeal);
            {
                const Rect line{r.x, y, r.w, px(30)};
                ui::label(kFontBody, px(theme::kBody), {line.x, line.y, line.w - px(60), line.h}, theme::kText, "Mod loader");
                bool on = loaderOn;
                if (ui::toggle("#mod-loader", {line.right() - px(44), line.y, px(44), line.h}, on, !busy && m.isObject()))
                    app.startModsCommand({"loader", on ? "on" : "off"}, on ? "Turning the loader on" : "Turning the loader off");
                y += px(34);
                text(r, y, loaderOn ? "On: the mods turned on below apply at the next start." : "Off: the next start is the stock game.",
                     theme::kFaint);
            }
            if (app.gameRunning)
                text(r, y, "The game is running: changes apply when it starts next.", theme::kAmber);
            // [mods3] LCS's sound mods (lcs_usertracks.cpp / lcs_sfxmod.cpp): the music folder next to mods/
            if (app.gameId == "lcs")
            {
                divider(r, y);
                section(r, y, "YOUR MUSIC AND SOUNDS");
                text(r, y, "Your own music as a radio station: put .mp3, .ogg, .flac or .wav files in the music folder "
                           "next to the mods folder (the station plays as usual while it is empty). In the game, F2 > "
                           "Audio > User tracks chooses the station they replace (LCFR by default); its DJ and adverts go "
                           "too. Applies at the next tune-in.");
                text(r, y, "A weapon's or a car's sound from a .wav: tools/mods.py --game lcs sound FILE --id N (pistol = "
                           "99 + 100, shotgun = 275 + 276; mods.py sounds <your iso> lists them). It plays at the original "
                           "sound's rate and is cut to its length; speech can't be replaced.",
                     theme::kFaint);
                const std::string folderPath = m["folder"].str();
                const char *musicLabel = "Open the music folder";
                if (ui::button(musicLabel, {r.x, y, ui::buttonWidth(musicLabel, theme::icon::kOpen), px(38)}, ButtonStyle::Ghost,
                               theme::icon::kOpen, !folderPath.empty()))
                {
                    const std::filesystem::path music = launcher::pathFrom(folderPath).parent_path() / "music";
                    std::error_code ec;
                    std::filesystem::create_directories(music, ec);
                    launcher::openFolder(music);
                }
                y += px(48);
                // [lcs] Vic's swim animations, made once from the player's own VCS disc (tools/stories_pack.py swim)
                divider(r, y);
                section(r, y, "SWIM ANIMATIONS  Â·  FROM YOUR VICE CITY STORIES DISC");
                text(r, y, "With Enhancements > Gameplay > Swimming on, Toni swims. Vice City Stories has real swimming "
                           "animations (treading water, breaststroke, crawl): this makes them into the mod \"Swim "
                           "animations (from your VCS disc)\" from your own disc, once. Without it he swims in a "
                           "stand-in pose. Turn it on or off below like any mod. The launcher makes it by itself "
                           "before Play once it knows your Vice City Stories disc (with Vice City's vehicles: the Stories "
                           "packs); this button makes it again.");
                const std::filesystem::path vcsDisc = app.knownDisc("vcs");
                bool have = false;
                for (size_t i = 0; i < m["mods"].size(); ++i)
                    have = have || m["mods"][i]["id"].str() == "stories_vcs_swim";
                const char *swimLabel = have ? "Make them again" : "Make the swim animations";
                if (ui::button(swimLabel, {r.x, y, ui::buttonWidth(swimLabel, 0), px(38)}, have ? ButtonStyle::Ghost : ButtonStyle::Primary,
                               0, !busy && !vcsDisc.empty()))
                    app.startSwimPack(vcsDisc);
                y += px(48);
                if (vcsDisc.empty())
                    text(r, y, "Needs your Vice City Stories disc: choose it once for Vice City Stories (the game "
                               "switch at the top), then come back here.", theme::kFaint);
                else
                    text(r, y, "From " + launcher::pathText(vcsDisc), theme::kFaint);
            }
            divider(r, y);

            // ---- installed ----
            const launcher::Json &mods = m["mods"];
            section(r, y, mods.size() ? "INSTALLED  \xC2\xB7  " + std::to_string(int(m["enabled"].num())) + " OF " +
                                            std::to_string(mods.size()) + " ON"
                                      : "INSTALLED");
            if (!m.isObject())
                text(r, y, app.modsBusy() ? "Reading..." : "", theme::kFaint);
            else if (!mods.size())
                text(r, y, "None yet: import a mod above.", theme::kFaint);
            for (size_t i = 0; i < mods.size(); ++i)
            {
                const launcher::Json &md = mods[i];
                const std::string id = md["id"].str();
                const bool on = md["enabled"].boolean();
                ui::pushId(2000 + int(i));
                const float top = y;
                // the card behind is drawn after its height is known: content first, at an inset
                const Rect inner{r.x + px(14), y + px(12), r.w - px(28), 0};
                float cy = inner.y;
                ui::label(kFontSemibold, px(theme::kBody + 1), {inner.x, cy, inner.w - px(60), px(24)}, on ? theme::kText : theme::kDim,
                          md["name"].str());
                bool value = on;
                if (ui::toggle("#on", {inner.right() - px(44), cy - px(2), px(44), px(28)}, value, !busy))
                    app.startModsCommand({value ? "enable" : "disable", id}, value ? "Turning a mod on" : "Turning a mod off");
                cy += px(26);
                // [mods] imported (the app's copy) or kept where it is (read there; "original missing" when gone)
                const bool inPlace = md["storage"].str() == "in_place";
                const std::string original = md["original"].str();
                std::string places;
                for (size_t p = 0; p < md["places"].size(); ++p)
                    places += (p ? ", " : "") + md["places"][p].str();
                std::string sub = inPlace ? "Kept where it is: " + places + "  \xC2\xB7  the app keeps " + sizeText(md["stored"].num())
                                          : "From " + md["source"].str() + "  \xC2\xB7  imported copy " + sizeText(md["stored"].num());
                if (md["converted"].num() > 0)
                    sub += " (converted models " + sizeText(md["converted"].num()) + ")";
                cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x, cy, inner.w, 0}, theme::kFaint, sub, 1.35f) + px(6);
                if (inPlace && (original == "missing" || original == "changed"))
                {
                    std::string which;
                    const launcher::Json &names = md[original == "missing" ? "missing" : "changed"];
                    for (size_t n = 0; n < names.size(); ++n)
                        which += (n ? ", " : "") + names[n].str();
                    cy += ui::paragraph(kFontSemibold, px(theme::kSmall), {inner.x, cy, inner.w, 0}, theme::kRed,
                                        original == "missing"
                                            ? "Original missing (" + which + "): moved, renamed or deleted. The game skips "
                                                                             "this mod until you find it again."
                                            : "Original changed (" + which + "): it isn't the file you added. Find the "
                                                                             "added one again, or add the new one as a new mod.",
                                        1.35f) + px(4);
                    const bool several = md["sources"].size() > 1;
                    const char *find = "Find it again...";
                    const char *copy = "Import a copy instead...";
                    const float f1 = ui::buttonWidth(find, 0, px(theme::kSmall)), f2 = ui::buttonWidth(copy, 0, px(theme::kSmall));
                    if (ui::button(find, {inner.x - px(6), cy, f1, px(30)}, ButtonStyle::Primary, 0, !busy, px(theme::kSmall)))
                        chooseModOriginal(id, several, false);
                    ui::tooltip(ui::id(find), {inner.x - px(6), cy, f1, px(30)},
                                several ? "Pick the folder that holds the mod's files now" : "Pick the file where it is now");
                    if (ui::button(copy, {inner.x + f1, cy, f2, px(30)}, ButtonStyle::Ghost, 0, !busy, px(theme::kSmall)))
                        chooseModOriginal(id, several, true);
                    ui::tooltip(ui::id(copy), {inner.x + f1, cy, f2, px(30)},
                                "Pick it, and the app keeps its own copy from now on (you may delete the original then)");
                    cy += px(38);
                }
                cy += px(4);
                const launcher::Json &entries = md["entries"];
                const bool open = isOpen(id) || std::getenv("VCS_MODS_SHOW_VALUES"); // every mod's values (screenshots)
                for (size_t k = 0; k < entries.size(); ++k)
                {
                    const launcher::Json &e = entries[k];
                    ui::pushId(int(k));
                    const KindTag tag = kindTag(e["kind"].str());
                    const float pw = ui::pillWidth(tag.tag);
                    ui::pill({inner.x, cy + px(2), pw, px(20)}, tag.tag, on && !e["off"].boolean() ? tag.color : theme::kFaint);
                    const std::string family = e["family"].str();
                    const bool slotted = family == "vehicle" || family == "weapon";
                    const std::string from = e["pcName"].str() == "VCMCAR" ? "this car" : e["pcName"].str();
                    const std::string headline = from + "  \xE2\x86\x92  " + e["targetName"].str();
                    const float bw = slotted ? ui::buttonWidth("Change slot", 0, px(theme::kSmall)) : 0;
                    ui::label(kFontSemibold, px(theme::kBody), {inner.x + pw + px(8), cy, inner.w - pw - bw - px(16), px(24)},
                              e["target"].num(-1) < 0 ? theme::kAmber : theme::kText, headline);
                    if (slotted && ui::button("Change slot", {inner.right() - bw, cy - px(2), bw, px(28)}, ButtonStyle::Ghost, 0,
                                              !busy && m.isObject(), px(theme::kSmall)))
                    {
                        g_mods.pickMod = id;
                        g_mods.pickEntry = int(e["index"].num());
                        g_mods.pickFamily = family;
                        g_mods.pickWhat = from;
                        // the picker marks the current slot: vehicles by model id, weapons by type (a weapon model's
                        // target is its model id)
                        g_mods.pickCurrent = -1;
                        const launcher::Json &table = m[family == "vehicle" ? "vehicleSlots" : "weaponSlots"];
                        for (size_t s = 0; s < table.size(); ++s)
                            if ((e["kind"].str() == "model" && family == "weapon" ? table[s]["modelId"].num() : table[s]["id"].num()) ==
                                e["target"].num(-1))
                                g_mods.pickCurrent = int(table[s]["id"].num());
                    }
                    cy += px(26);
                    std::string why = e["changes"].str();
                    if (slotted)
                        why += "  \xC2\xB7  " + e["matchText"].str() +
                               (e["match"].str() == "similar" && !e["slotClass"].str().empty() ? " (" + e["slotClass"].str() + ")" : "");
                    cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x + pw + px(8), cy, inner.w - pw - px(8), 0}, theme::kDim, why, 1.35f);
                    std::string state = stateText(e, on, loaderOn);
                    if (e["conflict"].boolean() && on)
                    {
                        std::string others;
                        for (size_t o = 0; o < e["others"].size(); ++o)
                            others += (o ? ", " : "") + e["others"][o].str();
                        state = (e["wins"].boolean() ? "conflict: wins over " : "conflict: loses to ") + others +
                                " (settle it under What is modified)";
                    }
                    const Color stateColor = state.rfind("skipped", 0) == 0 || state.rfind("not used", 0) == 0 ||
                                                     state.rfind("conflict", 0) == 0 ? theme::kAmber
                                             : state.rfind("applied", 0) == 0                                   ? theme::kTeal
                                                                                                                : theme::kFaint;
                    cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x + pw + px(8), cy, inner.w - pw - px(8), 0}, stateColor, state, 1.35f);
                    if (open)
                    {
                        std::string values;
                        const launcher::Json &fields = e["fields"];
                        for (size_t f = 0; f < fields.size(); ++f)
                        {
                            const launcher::Json &fv = fields[f];
                            if (!values.empty())
                                values += ",  ";
                            values += fv["name"].str() + " " + fv["value"].str();
                            if (fv["stock"].type() == launcher::Json::Type::String && fv["stock"].str() != fv["value"].str())
                                values += " (stock " + fv["stock"].str() + ")";
                        }
                        if (!values.empty())
                            cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x + pw + px(8), cy + px(2), inner.w - pw - px(8), 0},
                                                theme::kText, "New values: " + values, 1.35f) + px(2);
                        const launcher::Json &applied = e["applied"];
                        std::string game;
                        for (size_t a = 0; a < applied.size() && a < 8; ++a)
                            game += (game.empty() ? "" : "\n") + applied[a].str();
                        if (!game.empty())
                            cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x + pw + px(8), cy + px(2), inner.w - pw - px(8), 0},
                                                theme::kFaint, "The game at its last start (\"was\" = before the mod):\n" + game, 1.35f) + px(2);
                        if (e["skipped"].size())
                        {
                            std::string skipped;
                            for (size_t s = 0; s < e["skipped"].size(); ++s)
                                skipped += (s ? ", " : "") + e["skipped"][s].str();
                            cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x + pw + px(8), cy + px(2), inner.w - pw - px(8), 0},
                                                theme::kFaint, "Not used: " + skipped, 1.35f) + px(2);
                        }
                    }
                    cy += px(10);
                    ui::popId();
                }
                if (open && md["notes"].size())
                    for (size_t n = 0; n < md["notes"].size(); ++n)
                        cy += ui::paragraph(kFontBody, px(theme::kSmall), {inner.x, cy, inner.w, 0}, theme::kFaint,
                                            "\xE2\x80\xA2 " + md["notes"][n].str(), 1.35f) + px(2);
                // the mod's buttons
                {
                    const char *detail = open ? "Hide values" : "Show values";
                    const float w1 = ui::buttonWidth(detail, 0, px(theme::kSmall));
                    if (ui::button(detail, {inner.x - px(6), cy, w1, px(30)}, ButtonStyle::Ghost, 0, true, px(theme::kSmall)))
                    {
                        if (open)
                            g_mods.open.erase(std::find(g_mods.open.begin(), g_mods.open.end(), id));
                        else
                            g_mods.open.push_back(id);
                    }
                    const bool sure = g_mods.confirmRemove == id;
                    const char *remove = sure ? "Remove: sure?" : "Remove";
                    const float w2 = ui::buttonWidth(remove, 0, px(theme::kSmall));
                    if (ui::button(remove, {inner.x + w1, cy, w2, px(30)}, sure ? ButtonStyle::Danger : ButtonStyle::Ghost, 0, !busy,
                                   px(theme::kSmall)))
                    {
                        if (sure)
                        {
                            g_mods.confirmRemove.clear();
                            app.startModsCommand({"remove", id}, "Removing a mod");
                        }
                        else
                            g_mods.confirmRemove = id;
                    }
                    ui::tooltip(ui::id(remove), {inner.x + w1, cy, w2, px(30)},
                                "Deletes the app's copy and converted models (your own original files are never touched); the slot is stock again at the next start");
                    const std::string folderPath = md["folder"].str();
                    const float w3 = ui::buttonWidth("Files", 0, px(theme::kSmall));
                    if (ui::button("Files", {inner.x + w1 + w2 + px(4), cy, w3, px(30)}, ButtonStyle::Ghost, 0, true, px(theme::kSmall)))
                        launcher::openFolder(launcher::pathFrom(folderPath));
                    // [mods] switch: an imported mod reads its original again (the copy is deleted), a mod kept where
                    // it is gets a copy
                    const char *swap = inPlace ? "Import a copy" : "Use the original...";
                    const Rect swapR{inner.x + w1 + w2 + w3 + px(8), cy, ui::buttonWidth(swap, 0, px(theme::kSmall)), px(30)};
                    if (ui::button(swap, swapR, ButtonStyle::Ghost, 0, !busy && (!inPlace || original == "ok"), px(theme::kSmall)))
                    {
                        if (inPlace)
                            app.startModsCommand({"store", id, "copy"}, "Importing a copy");
                        else
                            chooseModOriginal(id, md["sources"].size() > 1, false);
                    }
                    ui::tooltip(ui::id(swap), swapR,
                                inPlace ? "The app keeps its own copy from now on: you may move or delete the original then"
                                        : "Pick the original file (or folder) you imported from: the app reads it there and "
                                          "deletes its copy. Don't move or delete the original afterwards.");
                    cy += px(34);
                }
                const Rect cardR{r.x, top, r.w, cy - top + px(6)};
                rectEx(cardR, theme::ink(on ? 0.035f : 0.015f), theme::ink(on ? 0.02f : 0.01f), px(12), theme::ink(0.07f), ui::hairline());
                ui::popId();
                y = cardR.bottom() + px(10);
            }

            // ---- what is modified, overall ----
            const launcher::Json &slots = m["slots"];
            if (slots.size())
            {
                divider(r, y);
                section(r, y, m["conflicts"].size() ? "WHAT IS MODIFIED  \xC2\xB7  " + std::to_string(m["conflicts"].size()) +
                                                          (m["conflicts"].size() == 1 ? " CONFLICT" : " CONFLICTS")
                                                    : "WHAT IS MODIFIED");
                text(r, y, "Every slot the mods change. DRIVES / FIRES lines change behaviour, LOOKS models change the "
                           "look: a car can take one of each. When two mods change the same thing, the one loaded later "
                           "wins, value by value: pick the one you want.",
                     theme::kFaint);
                for (size_t i = 0; i < slots.size(); ++i)
                {
                    const launcher::Json &s = slots[i];
                    const launcher::Json &by = s["by"];
                    ui::pushId(4000 + int(i));
                    const KindTag tag = kindTag(s["kind"].str());
                    const float pw = ui::pillWidth(tag.tag);
                    const bool conflict = s["conflict"].boolean();
                    ui::pill({r.x, y + px(2), pw, px(20)}, tag.tag, conflict ? theme::kAmber : tag.color);
                    std::string who;
                    for (size_t b = 0; b < by.size(); ++b)
                        if (by[b]["enabled"].boolean())
                            who += (who.empty() ? "" : ", ") + by[b]["name"].str() + (by[b]["off"].boolean() ? " (set aside)" : "");
                    if (who.empty())
                        who = "(mods off)";
                    ui::label(kFontSemibold, px(theme::kBody), {r.x + px(78), y, px(190), px(24)}, theme::kText, s["targetName"].str());
                    y += ui::paragraph(kFontBody, px(theme::kSmall), {r.x + px(276), y + px(3), r.w - px(276), 0},
                                       conflict ? theme::kAmber : theme::kDim, who, 1.35f) + px(4);
                    if (conflict || s["resolved"].boolean())
                    {
                        std::string winner;
                        for (size_t b = 0; b < by.size(); ++b)
                            if (by[b]["mod"].str() == s["winner"].str())
                                winner = by[b]["name"].str();
                        text({r.x + px(78), y, r.w - px(78), 0}, y,
                             conflict ? winner + " wins now (it is loaded later). Use only:" : "Settled: " + winner + " has it. Switch to:",
                             conflict ? theme::kAmber : theme::kFaint);
                        float bx = r.x + px(78);
                        for (size_t b = 0; b < by.size(); ++b)
                        {
                            if (!by[b]["enabled"].boolean() || (!conflict && by[b]["mod"].str() == s["winner"].str()))
                                continue;
                            ui::pushId(int(b));
                            const std::string label = by[b]["name"].str();
                            const float bw = std::min(ui::buttonWidth(label, 0, px(theme::kSmall)), r.w * 0.45f);
                            if (bx + bw > r.right())
                            {
                                bx = r.x + px(78);
                                y += px(34);
                            }
                            if (ui::button(label, {bx, y, bw, px(30)}, ButtonStyle::Secondary, 0, !busy, px(theme::kSmall)))
                                app.startModsCommand({"prefer", by[b]["mod"].str(), std::to_string(int(by[b]["entry"].num()))},
                                                     "Settling a conflict");
                            bx += bw + px(6);
                            ui::popId();
                        }
                        y += px(38);
                    }
                    ui::popId();
                    y += px(4);
                }
            }
            if (!m["applied"].str().empty())
            {
                divider(r, y);
                section(r, y, "AT THE LAST START");
                text(r, y, m["applied"].str() + "  \xC2\xB7  " + when(m["appliedTime"].num()), theme::kDim);
            }
            return y;
        }
    }

    void drawSheets(Rect window)
    {
        App &app = *shell.app;
        if (shell.sheet != Sheet::None)
            shell.sheetShown = shell.sheet;
        ui::Widget &w = ui::widget(ui::id("#sheet"));
        if (w.fresh)
            shell.sheetSpring.snap(0);
        ui::spring(shell.sheetSpring, shell.sheet != Sheet::None ? 1.0f : 0.0f, 340, 0.88f);
        const float open = shell.sheetSpring.value;
        if (open <= 0.002f && shell.sheet == Sheet::None)
        {
            shell.sheetShown = Sheet::None;
            return;
        }
        Rect area = window;
        core::cutTop(area, px(theme::kTitleBar));
        const Layer previous = setLayer(Layer::Overlay);
        ui::beginLayer(kLayerSheet, shell.sheet != Sheet::None);

        // the scrim: clicking it closes the sheet
        rect(area, withAlpha(theme::kScrim, alphaOf(theme::kScrim) * core::clamp01(open)));
        const ui::Interaction scrim = ui::interact(ui::id("#scrim"), area, false);
        if (scrim.clicked)
            shell.sheet = Sheet::None;

        // [mods] the Mods sheet is wider: a mod's lines, their slots and values
        // ... and the Game settings sheet: its choices in a row of chips
        const float width = std::min(px(shell.sheetShown == Sheet::Mods || shell.sheetShown == Sheet::Game ? 640 : 480), area.w * 0.9f);
        const Rect panel{area.right() - width * open, area.y, width, area.h};
        ui::blockInput(panel);
        shadow(panel.offset(-px(6), 0), theme::shade(0.8f * core::clamp01(open)), px(24), 0);
        rectEx(panel, theme::kBase, mix(theme::kBase, theme::kNight, 0.5f), {px(14), 0, 0, 0}, theme::ink(0.06f), ui::hairline());

        const char *title = "";
        const char *subtitle = "";
        switch (shell.sheetShown)
        {
        case Sheet::Saves: title = "Saves"; subtitle = "Back up and restore your memory card."; break;
        case Sheet::Settings: title = "Settings"; subtitle = "The launcher's own options. The game's are under Game settings."; break;
        case Sheet::Game: title = "Game settings"; subtitle = "Draw distance, resolution, frame rate, sound... before you play."; break;
        case Sheet::Help: title = "Help"; subtitle = "When something goes wrong, the logs say why."; break;
        case Sheet::Folder: title = "Build folder"; subtitle = "Where the game is built."; break;
        case Sheet::Android: title = "Prepare for Android"; subtitle = "Build the game library for your phone."; break;
        case Sheet::Mods: title = "Mods"; subtitle = "Import, turn on and off, choose slots, settle conflicts."; break;
        default: break;
        }
        Rect inner = panel.inset(px(32), 0);
        float y = panel.y + px(26);
        drawText(kFontSemibold, px(24), inner.x, y + metrics(kFontSemibold, px(24)).ascent, theme::kText, title);
        if (ui::iconButton("#closesheet", theme::icon::kClose, {panel.right() - px(56), y - px(2), px(34), px(34)}, "Close (Esc)", true, px(12)))
            shell.sheet = Sheet::None;
        y += px(38);
        ui::label(kFontBody, px(theme::kSmall), {inner.x, y, inner.w, px(20)}, theme::kDim, subtitle);
        y += px(36);

        const Rect view{panel.x, y, panel.w, panel.bottom() - y};
        const ui::Id scroll = ui::id("#sheet-scroll", int(shell.sheetShown));
        ui::beginScroll(scroll, view);
        const Rect content{inner.x, y + px(4), inner.w, 0};
        // the content follows a beat behind the panel
        render::pushOffset(std::round((1 - core::clamp01(open)) * px(40)), 0);
        float bottom = y;
        switch (shell.sheetShown)
        {
        case Sheet::Saves: bottom = saves(app, content); break;
        case Sheet::Settings: bottom = settings(app, content); break;
        case Sheet::Help: bottom = help(app, content); break;
        case Sheet::Folder: bottom = folder(app, content); break;
        case Sheet::Mods: bottom = modsSheet(app, content); break;
        case Sheet::Game: bottom = drawGameSettings(app, content); break;
        case Sheet::Android: bottom = drawAndroidPack(app, content); break; // [android]
        default: break;
        }
        render::popOffset();
        ui::endScroll(scroll, bottom + px(24));
        ui::endLayer();
        setLayer(previous);
    }
}
