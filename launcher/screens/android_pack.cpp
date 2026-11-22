// [android] The "Prepare for Android" sheet: your disc -> the game library for the phone, as one file in an export
// folder. The work is tools/android_pack.py (preflight, recompile, build-game, copy), run as the Task kind
// "android_pack" and shown by task.cpp; this sheet only picks the disc and starts it (docs/ANDROID.md, Windows).
#include "shell.h"

#include "model/system.h"

#include <string>

namespace screens
{
    using namespace render;
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
    }

    float drawAndroidPack(App &app, Rect r)
    {
        float y = r.y;
        const std::string game = app.disc.ok && !app.disc.game.empty() ? app.disc.game : app.gameId;

        section(r, y, "THE DISC");
        y += row({r.x, y, r.w, 0}, "Disc", app.iso.empty() ? "None chosen yet" : launcher::pathText(app.iso));
        if (app.disc.ok)
            y += row({r.x, y, r.w, 0}, "Game", launcher::gameTitle(game));
        else
            y += row({r.x, y, r.w, 0}, "Check", discLine(app), app.discChecked ? theme::kAmber : 0);
        y += px(6);
        const char *choose = app.iso.empty() ? "Choose the disc" : "Choose another disc";
        if (ui::button(choose, {r.x, y, ui::buttonWidth(choose, theme::icon::kFolder), px(38)}, ButtonStyle::Secondary,
                       theme::icon::kFolder, !app.busy()))
            chooseIso();
        y += px(56);

        section(r, y, "WHAT HAPPENS");
        text(r, y, "This PC recompiles the game from your disc and builds the game library for the phone (a long step, "
                   "and the phone stays cool). One file comes out.");
        // the same text on Windows and macOS (docs/ANDROID.md "PC builder: Windows vs macOS")
        text(r, y, "It needs the Android NDK and CMake, glslang and git. If one is missing, the next screen "
                   "lists it with the setup command (python tools/android.py setup), which shows the downloads and "
                   "their size, asks first and fetches them from their official sources.");
#ifdef _WIN32
        // tools/preflight.py: the build's deepest file is about 205 characters below the folder, Windows stops at 259
        const size_t len = launcher::pathText(app.root).size();
        if (len > 50)
            text(r, y, "This folder's path is long (" + std::to_string(len) + " characters). Move the whole folder to a short one, "
                       "such as C:\\gta, or the build can hit Windows' path limit.", theme::kAmber);
#endif
        y += px(4);

        const bool can = app.disc.ok && !app.busy() && !app.gameRunning;
        // [android] the button is grey while something else runs: say what, so it does not look dead
        if (!can)
        {
            const std::string why = app.busy()          ? "Waiting for \"" + app.task->title + "\" to finish."
                                    : app.gameRunning   ? "Close the game first."
                                    : app.iso.empty()   ? "Choose your disc above first."
                                                        : "The disc above did not pass the check: choose another.";
            text(r, y, why, theme::kAmber);
        }
        const char *go = "Prepare for Android";
        if (ui::button(go, {r.x, y, ui::buttonWidth(go, theme::icon::kDownload) + px(12), px(44)}, ButtonStyle::Primary,
                       theme::icon::kDownload, can))
        {
            shell.sheet = Sheet::None;
            app.startAndroidPack();
        }
        y += px(56);

        section(r, y, "THE RESULT");
        y += row({r.x, y, r.w, 0}, "File", "libvcs_game.so.gz");
        y += row({r.x, y, r.w, 0}, "Folder", launcher::pathText(app.root / "android-export" / game));
        text(r, y, "It holds your game's code: for your own phone only, never share it.", theme::kAmber);
        return y;
    }
}
