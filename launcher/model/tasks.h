// The launcher's state and what it runs. Everything long is a Python tool from tools/ (the same scripts people run by
// hand): doctor.py for the checks, build.py for the build, setup_vs.py, backup.py, report.py, update.py, rip_disc.py.
// They print `@vcs {...}` events (tools/toolchain.py) that become a Task's stages, progress, error and result.
#pragma once

#include "disc_check.h"
#include "json.h"
#include "process.h"

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace launcher
{
    struct Task
    {
        std::string kind;  // build, tools, backup, restore, report, update_check, update, rip_list, rip
        std::string game;  // [storiesbuild] a build's game ("lcs", "vcs"): the GTA Stories tab builds both, whichever tab is shown
        std::string title;
        std::unique_ptr<Process> process;
        struct Stage
        {
            std::string id, title;
            double start = 0, end = 0;
        };
        std::vector<Stage> stages;
        int done = 0, total = 0, eta = -1;
        std::string note;
        std::deque<std::string> log;
        std::string errorCode, errorMessage, errorLog;
        Json result;
        bool finished = false, cancelled = false;
        int exitCode = 0;
        double startTime = 0, endTime = 0;

        bool ok() const { return finished && !cancelled && exitCode == 0 && errorCode.empty(); }
    };

    // What a failure code from the tools means and what the player can do (shown under the error).
    struct ErrorHelp
    {
        const char *title;
        const char *fix;
    };
    const ErrorHelp &errorHelp(const std::string &code);

    struct Settings
    {
        std::string afterLaunch = "minimize"; // minimize, close, stay
        std::string opt;                      // "" (keep the build's), lto, none, pgo
        bool checkUpdates = false;            // ask GitHub for a newer release when the launcher starts
        bool reduceMotion = false;            // the launcher's animations off; this alone decides (off until set)
        std::string theme = "system";         // system (Windows' app mode), light, dark
        bool storiesSaveSync = true;          // [savesync] Vice City saves made in Stories mode go back to your card (STORIES_SAVE_SYNC)
        std::string storiesHero = "vic";      // [hero] GTA Stories: the hero: vic or toni in both cities, or city = each city's own (STORIES_HERO)
        // [stories] GTA Stories: both cities in one program and one window (stories.exe, tools/build.py --oneproc;
        // docs/STORIES.md "One program"), when it is built; off: the two games as two programs
        bool oneProgram = false;
        std::string missionBorder = "fail"; // [missionborder] GTA Stories: crossing the border in a mission: fail (default) or block (STORIES_DW_MISSION_BORDER)
    };

    class App
    {
    public:
        bool init(std::string &error);
        void update(double now); // polls the processes; call every frame

        // the repository (where tools/build.py is) and its build folder (launcher.ini; Vice City Stories' game)
        std::filesystem::path root, build;

        // [lcs] The game shown: "vcs" (Vice City Stories) or "lcs" (Liberty City Stories); launcher.ini's game=. Each
        // has its own build folder (gameBuild(): build/, build/lcs - tools/toolchain.py's), disc (iso= in that
        // folder's vcs.ini), doctor answer (doctor.py --game) and saves (backup.py --game); switching keeps the
        // other's. Every tool the launcher runs for a game gets --game.
        // [lcs] Liberty City Stories is the default (no saved game=); a disc of the other game, when none of this one's
        // is found, still switches to it.
        std::string gameId = "lcs";
        // [stories] the title bar's third tab, "GTA Stories": both cities at once (docs/STORIES.md). Liberty City is the
        // game in front, so gameId stays "lcs" (its disc, build, saves and mods are the ones shown); Play runs
        // tools/stories.py. launcher.ini's game=stories.
        bool stories = false;
        std::string shownId() const { return stories ? "stories" : gameId; } // "lcs", "vcs" or "stories"
        bool gameChosen = false; // launcher.ini named one (else the first disc found or chosen decides)
        std::filesystem::path gameBuild() const;
        std::string gameExe() const;      // vcs.exe / lcs.exe (no .exe off Windows)
        const char *title() const;        // "Vice City Stories", "Liberty City Stories"
        bool canSwitchGame() const { return !busy() && !gameRunning; }
        bool selectGame(const std::string &id); // false while something runs; [stories] "stories" too
        // Linux AppImage (tools/release.py): the release's files inside the read-only image; root is then the install
        // folder they are copied to ($XDG_DATA_HOME/GTAStoriesRecomp/install, VCS_INSTALL_DIR) - empty otherwise
        std::filesystem::path payload;
        bool installing() const { return task && task->kind == "install" && !task->finished; }
        // a release that updates itself: Windows (a release folder or not: update.py says), a Linux AppImage
        bool canUpdate() const;
        std::vector<std::string> python; // the interpreter (+ args): a release's python\, else py -3 / python (Linux: python3)
        std::string pythonText;
        std::string sourceVersion;        // VERSION, or the clone's commit

        // tools/doctor.py --json
        Json doctor;
        bool doctorRunning = false, doctorDone = false;
        // shown from build/launcher_state.json (the last answer) until this run's check answers: the first frame
        // already has the right layout; nothing in it may act on what the cache says (the UI shows it as checking)
        bool doctorCached = false;
        std::string doctorError;
        std::string toolsProblem; // the scripts the launcher runs aren't all in tools/ (set by init)
        // the check ran and gave no answer: the steps it decides are unknown, never "missing"
        bool doctorFailed() const { return doctorDone && !doctorRunning && !doctor.isObject(); }
        void refresh();

        // the disc: checked here (instantly), remembered as iso= in build/vcs.ini
        std::filesystem::path iso;
        DiscResult disc;
        bool discChecked = false, discFound = false; // found = picked by the scan, not the player
        void chooseDisc(const std::filesystem::path &path, bool byScan = false);
        // [storiesbuild] the GTA Stories tab's picker (and a file dropped on it): the disc is checked first and a file that is
        // not a supported disc only says why (Liberty City's disc stays as it was; the tab stays). Either game's disc is
        // taken for its own game. Returns the check's answer.
        DiscResult chooseStoriesDisc(const std::filesystem::path &path);
        void scanForDisc();

        std::unique_ptr<Task> task; // the one long thing running (or finished, until dismissed)
        bool busy() const { return task && !task->finished; }
        void startBuild(); // the shown game's
        void startBuildFor(const std::string &game); // [storiesbuild] "lcs" or "vcs": that game's, its disc from knownDisc()
        void startInstallTools();
        void startAndroidPack(); // [android] "Prepare for Android": tools/android_pack.py (the shown disc -> android-export/<game>/)
        void startBackup();
        void startRestore(const std::string &backup);
        void startBackupList();
        void startReport();
        void startUpdateCheck(bool quiet);
        void startUpdate();
        void startRipList();
        // the build folder (tools/build_dir.py): check what a chosen folder means, then use it
        void startBuildDirCheck(const std::filesystem::path &path);
        void startBuildDirSet(const std::filesystem::path &path);
        Json buildDirCheck; // the last check's answer, for the confirmation
        bool buildCustom() const;
        void startRip(const std::string &drive, const std::filesystem::path &out);
        void cancelTask();
        void dismissTask() { if (task && task->finished) task.reset(); }
        Json backups, drives, updateInfo; // results kept from their tasks
        Json saves;                       // [lcs] the game's save slots on its card (backup.py list: saves)
        bool updateQuiet = false;
        // [mods] the shown game's mods (tools/mods.py --game: its data folder's mods/). `mods` is the last `list`
        // answer (mods, their entries, the slots they change, conflicts, the slot picker's lists); a change runs the
        // command, then the list again. Mod tasks never take the window: their failure is modsError (the sheet shows it).
        Json mods;
        std::string modsError, modsNote;
        bool modsBusy() const { return task && !task->finished && task->kind.rfind("mods", 0) == 0; }
        void startModsList();
        void startModsCommand(const std::vector<std::string> &args, const std::string &title);
        // [lcs] the other game's disc when the launcher knows one (this session's check, else its build folder's
        // vcs.ini), checked once (cached until the shown game changes); empty if none
        std::filesystem::path knownDisc(const std::string &game) const;
        // [lcs] the Mods sheet's button: Vic's swim animations made again (tools/stories_auto.py --only swim --force)
        void startSwimPack(const std::filesystem::path &vcsIso);
        // [stories] the other game's packs (tools/stories_auto.py: Vic's swim animations, the other city's vehicles),
        // made or refreshed before Play whenever the other game's disc is known: a quiet task (kind "packs") that
        // never takes the window; a pack that fails is said (packsNote) and the game plays without it
        std::string packsMode() const { return stories ? "stories" : gameId; }
        bool packsWanted() const;
        void startPacks();
        bool packsBusy() const { return task && !task->finished && task->kind == "packs"; }
        std::string packsNote; // the last run's failure, for Home (empty: none)
        Json packsResult;      // the last run's answer: built, current, failed, skipped
        // [stories] Vice City Stories' state while the GTA Stories tab shows (Liberty City is gameId): doctor.py --game vcs
        Json storiesVcs;
        bool storiesVcsRunning = false, storiesVcsDone = false;
        void refreshStoriesVcs(bool useCache = true);
        // [storiesbuild] the GTA Stories tab's "Build and play": the build tools if they are missing, then each city that is
        // not built or out of date (Liberty City, then Vice City) - the same build tasks the two tabs run, one after the
        // other - then Play. stepStoriesBuild() runs it from the frame (screens) or a command line loop: true once when
        // everything is built and Play was asked for. A failed build leaves its error card (Try again goes on); a cancel
        // or closing the card ends the plan.
        struct StoriesPlan
        {
            std::vector<std::string> steps; // "tools", "lcs", "vcs"
            size_t index = 0;               // the step running (started) or the next one
            bool active = false, started = false, failed = false, play = false;
        };
        StoriesPlan storiesPlan;
        bool storiesCityNeedsBuild(const std::string &game) const; // not built, out of date, or not known
        bool storiesToolsNeeded() const;                           // the build tools are missing (doctor's compiler / downloads)
        bool startStoriesBuild(bool thenPlay);                     // false: nothing started (busy, a disc missing)
        bool stepStoriesBuild();
        // an update put in place (update.py apply said restart): the version; this launcher is the old one until then
        std::string updatedTo;
        std::filesystem::path restartTarget() const; // the new launcher: root's GTAStoriesLauncher.exe, or $APPIMAGE
        // the new launcher started (--after-update <our pid>: it waits for us to end); the caller closes this one
        bool restart(std::string &error);

        // the game
        std::unique_ptr<Process> game;
        bool gameRunning = false, gameEnded = false;
        int gameExit = 0;
        double gameStart = 0, gameEnd = 0;
        // [launcherfix] Stop game: asks the game to close like a player's Alt+F4 (the Stories partner too), then ends the
        // whole tree if it is still there kGameStopGrace seconds later. gameStopAsked: when (0 = not asked)
        static constexpr double kGameStopGrace = 6;
        double gameStopAsked = 0;
        bool gameKilled = false;
        bool gameStopping() const { return gameRunning && gameStopAsked > 0; }
        void stopGame();
        std::filesystem::path gameLog;
        bool play(std::string &error, int slot = 0); // [lcs] slot 1-8: the boot loads it (VCS_LOAD_SLOT)
        // [dualworld] both games built (build/vcs.exe and build/lcs/lcs.exe): "Stories: both cities" can be offered
        bool storiesAvailable() const;
        bool storiesPlay() const { return stories && gameId == "lcs" && storiesAvailable(); }
        bool oneProgramAvailable() const; // [stories] build/oneproc holds stories.exe and both games' DLLs (Windows)
        std::string gameExitText() const;

        Settings settings;
        void saveSettings();
        bool keepSettings = false; // screenshots and the self-test: launcher.ini is never written (their game / theme)

        std::string toast; // a short message under the buttons
        double toastTime = 0;
        void say(const std::string &text, double now = -1);

        bool shortcut(std::string &error);
        std::filesystem::path savesFolder() const;
        std::string next() const; // doctor's "next" with the launcher's own disc check over it; "recheck" when it failed

    private:
        std::unique_ptr<Process> m_doctor;
        std::unique_ptr<Process> m_vcsDoctor; // [stories] doctor.py --game vcs
        std::vector<std::string> m_vcsDoctorOut;
        // [lcs] the game not shown: what it had when the player switched away (its disc, doctor answer, saves)
        struct GameState
        {
            std::filesystem::path iso;
            DiscResult disc;
            bool discChecked = false, discFound = false;
            Json doctor, backups, saves;
            bool doctorDone = false;
            std::string doctorError;
        };
        std::map<std::string, GameState> m_games;
        mutable std::map<std::string, std::filesystem::path> m_knownDiscs; // knownDisc()'s answers
        void loadGame(); // the shown game's disc (its vcs.ini, else a scan) and last doctor answer, then a check
        std::vector<std::string> gameArgs() const { return {"--game", gameId}; }
        std::unique_ptr<Process> m_restarted; // the new launcher (restart): never killed
        std::vector<std::string> m_doctorOut;
        double m_now = 0;
        void runTool(const std::string &kind, const std::string &title, const std::vector<std::string> &args);
        void handleLine(Task &t, const std::string &line);
        void finishTask(Task &t);
        void loadSettings();
        void loadFolder(); // root's scripts, version, settings, disc, the doctor - once root holds the files
        bool writeIsoToIni(const std::filesystem::path &path, const std::filesystem::path &folder = {});
    };
}
