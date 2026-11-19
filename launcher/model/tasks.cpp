#include "tasks.h"

#include "os.h"
#include "system.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        constexpr const char *kEventPrefix = "@vcs "; // tools/toolchain.py EVENT_PREFIX
        // what the launcher runs (tools/release.py's self-test makes sure a release has them)
        constexpr const char *kTools[] = {"doctor.py", "build.py", "toolchain.py", "recompile.py", "vcs_disc.py",
                                          "setup_vs.py", "backup.py", "report.py", "update.py", "rip_disc.py"};
        constexpr size_t kLogLines = 4000;

        std::string findOnPath(const char *name)
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH * 2];
            const DWORD n = SearchPathW(nullptr, widen(name).c_str(), nullptr, DWORD(std::size(buffer)), buffer, nullptr);
            if (n == 0 || n >= std::size(buffer))
                return {};
            return narrow(buffer);
#else
            const char *path = std::getenv("PATH");
            std::string_view rest = path ? path : "/usr/local/bin:/usr/bin:/bin";
            while (!rest.empty())
            {
                const size_t colon = rest.find(':');
                const std::string_view dir = rest.substr(0, colon);
                rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
                if (dir.empty())
                    continue;
                const fs::path candidate = pathFrom(std::string(dir)) / name;
                std::error_code ec;
                if (fs::is_regular_file(candidate, ec) &&
                    (fs::status(candidate, ec).permissions() & fs::perms::others_exec) != fs::perms::none)
                    return pathText(candidate);
            }
            return {};
#endif
        }

        std::string toolPath(const fs::path &root, const char *name) { return pathText(root / "tools" / name); }

        std::string readFile(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }

        std::string trim(std::string s)
        {
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                ++i;
            return s.substr(i);
        }

        const std::map<std::string, ErrorHelp> kErrors = {
            {"no_vs", {"Visual Studio's C++ tools aren't installed",
                       "The game is compiled on your PC. Use \"Install build tools\" on the Build tools step (a free "
                       "download from Microsoft, about 6 GB)."}},
            {"no_cpp_tools", {"Visual Studio has no C++ tools",
                              "Use \"Install build tools\", or in the Visual Studio Installer choose Modify and tick "
                              "\"Desktop development with C++\" and \"C++ Clang tools for Windows\"."}},
            {"no_clang", {"The C++ Clang tools are missing",
                          "Use \"Add the Clang tools\", or in the Visual Studio Installer choose Modify and tick "
                          "\"C++ Clang tools for Windows\"."}},
            {"old_clang", {"Visual Studio's clang is too old",
                           "Update Visual Studio: open the Visual Studio Installer and choose Update."}},
            {"no_cmake", {"CMake couldn't be found or downloaded",
                          "The build downloads CMake when none is installed, and that failed: check the internet "
                          "connection and build again, or install CMake (cmake.org) or Visual Studio's \"C++ CMake "
                          "tools for Windows\" (Visual Studio Installer, Modify)."}},
            {"vcvars_failed", {"Visual Studio's environment could not be set up",
                               "Repair Visual Studio from the Visual Studio Installer (More > Repair)."}},
            {"no_disc", {"No disc image", "Choose your game's disc image (.iso, USA) on the Disc step."}},
            {"disc_not_found", {"The disc image is gone", "It was moved or deleted: choose it again on the Disc step."}},
            {"wrong_disc", {"This is not the supported disc",
                            "Only the USA discs work: GTA: Vice City Stories (SLUS-21590) and GTA: Liberty City "
                            "Stories (SLUS-21423). Choose another image."}},
            {"download_failed", {"A download failed",
                                 "The first build downloads libraries. Check the internet connection and try again "
                                 "(a release with a deps folder needs none)."}},
            {"configure_failed", {"Setting up the build failed", "Open the log for the details, or copy a report for help."}},
            {"disk_full", {"The drive is full", "Free some space (a build needs about 3 GB) and build again."}},
            {"path_too_long", {"A path is too long for Windows",
                               "Move the whole folder somewhere short, like C:\\Games\\VCS, and build again."}},
            {"file_locked", {"A file was locked",
                             "Often an antivirus scanning the build, or a cloud-synced folder. Build again; if it keeps "
                             "happening, add the folder to Windows Security's exclusions or move it out of OneDrive."}},
            {"out_of_memory", {"The PC ran out of memory",
                               "Close other programs and build again; it continues where it stopped."}},
            {"file_dates", {"Files are dated in the future",
                            "Some files in the folder are newer than the PC's clock, so the build keeps starting over. "
                            "Check that Windows' clock and time zone are right, then build again."}},
            {"build_failed", {"The build failed", "Open the log for the details, or copy a report for help."}},
            {"recompiler_failed", {"Recompiling the game failed", "Open the log for the details, or copy a report for help."}},
            {"cpu_unsupported", {"This processor is too old for the game",
                                 "The game needs a processor with SSE4.2: an Intel Core i (2008) or AMD Bulldozer (2011) "
                                 "or newer."}},
            {"illegal_instruction", {"A program used an instruction this processor doesn't have",
                                     "It was built for a newer processor. Build again: the build is set up for this "
                                     "PC's processor. If it happens again, copy a report for help."}},
            {"missing_dll", {"A program is missing a library",
                             "Install the latest Microsoft Visual C++ Redistributable (x64) from Microsoft, then build "
                             "again."}},
            {"elevation_denied", {"The installer wasn't allowed to run",
                                  "Installing the build tools needs administrator rights: accept the Windows prompt."}},
            {"install_failed", {"The installer didn't finish",
                                "Run it again, or install \"Build Tools for Visual Studio\" yourself from "
                                "visualstudio.microsoft.com with \"Desktop development with C++\" and \"C++ Clang tools "
                                "for Windows\"."}},
            {"reboot_needed", {"Restart Windows to finish", "The installer needs a restart before the tools work."}},
            {"python_failed", {"A tool could not run", "Python stopped with an error: open the log."}},
            {"git_checkout", {"This is a git checkout", "Update it with git pull, then build again."}},
            {"build_dir_refused", {"That folder can't be used", "Choose another folder for the build."}},
            {"network", {"No connection", "Check the internet connection and try again."}},
            {"update_incomplete", {"The update couldn't replace every file",
                                   "Something held some of them open (often an antivirus or a cloud-synced folder); "
                                   "the folder is still the old release. Close the game and update again, or extract "
                                   "the new release's zip into a new folder."}},
        };
#ifndef _WIN32
        // the same codes where Linux needs other words, and its own (tools/build.py --install-deps, doctor.py)
        const std::map<std::string, ErrorHelp> kLinuxErrors = {
            {"missing_packages", {"Packages the build needs are missing",
                                  "Use \"Install packages\" on the Build tools step (it asks for your password), or "
                                  "\"Install in a terminal\"."}},
            {"no_polkit", {"No password prompt could be shown",
                           "Use \"Install in a terminal\" on the Build tools step: the command runs there and asks "
                           "for your password."}},
            {"elevation_denied", {"The password prompt was cancelled",
                                  "Installing packages needs your password: use \"Install packages\" again and enter it."}},
            {"install_failed", {"The package manager didn't finish",
                                "Open the log for its message. Another update running at the same time locks it: try "
                                "again once that is done, or use \"Install in a terminal\"."}},
            {"file_locked", {"A file was locked", "Something else held a file of the build. Build again."}},
            {"file_dates", {"Files are dated in the future",
                            "Some files in the folder are newer than the PC's clock, so the build keeps starting over. "
                            "Check the clock and time zone, then build again."}},
            {"out_of_memory", {"The PC ran out of memory",
                               "Close other programs and build again; it continues where it stopped."}},
        };
#endif
#if defined(__APPLE__)
        // macOS' own words (checked before Linux's)
        const std::map<std::string, ErrorHelp> kMacErrors = {
            {"no_clt", {"Apple's Command Line Tools aren't installed",
                        "Use \"Install the Command Line Tools\" on the Build tools step: macOS opens Apple's installer. "
                        "Choose Check again when it has finished."}},
            {"install_failed", {"The Command Line Tools didn't install",
                                "Install them from developer.apple.com/download (Command Line Tools for Xcode), or "
                                "install Xcode, then choose Check again."}},
            {"app_translocated", {"Move the launcher to Applications first",
                                  "macOS runs an app straight from a download in a read-only copy, which can't be "
                                  "updated. Drag GTAStoriesLauncher.app to Applications in Finder, start it from there, then "
                                  "update again."}},
            {"cpu_unsupported", {"This Mac has an Intel processor",
                                 "The macOS version of the game is for Apple Silicon Macs (M1 and newer)."}},
        };
#endif
        const ErrorHelp kUnknown = {"Something went wrong", "Open the log for the details, or copy a report for help."};
    }

    const ErrorHelp &errorHelp(const std::string &code)
    {
#ifndef _WIN32
#if defined(__APPLE__)
        if (const auto it = kMacErrors.find(code); it != kMacErrors.end())
            return it->second;
#endif
        if (const auto it = kLinuxErrors.find(code); it != kLinuxErrors.end())
            return it->second;
#endif
        const auto it = kErrors.find(code);
        return it == kErrors.end() ? kUnknown : it->second;
    }

#if defined(__APPLE__)
    // Apple's Command Line Tools (or Xcode) are installed: xcode-select knows a developer folder
    bool macCommandLineTools()
    {
        Process p;
        Process::Options options;
        std::string ignored;
        if (!p.start({"/usr/bin/xcode-select", "-p"}, options, ignored))
            return false;
        return p.wait() == 0;
    }
#endif

    bool App::init(std::string &error)
    {
#if defined(__APPLE__)
        // a macOS release: GTAStoriesLauncher.app carries the release's files (Contents/MacOS -> Contents/Resources/release),
        // and the game is built in a folder of its own they are copied to, as from an AppImage
        if (const fs::path files = exePath().parent_path().parent_path() / "Resources" / "release";
            fs::is_regular_file(files / "RELEASE.json"))
        {
            payload = files;
            const char *dir = std::getenv("VCS_INSTALL_DIR");
            root = dir && *dir ? pathFrom(dir) : knownFolder(Folder::LocalAppData) / "GTAStoriesRecomp" / "install";
        }
#elif !defined(_WIN32)
        // a Linux AppImage: the release's files are in the image next to the launcher (usr/bin -> usr/share), and the
        // game is built in a folder of its own they are copied to
        if (const fs::path files = exePath().parent_path().parent_path() / "share" / "gta-stories-recomp" / "release";
            fs::is_regular_file(files / "RELEASE.json"))
        {
            payload = files;
            const char *dir = std::getenv("VCS_INSTALL_DIR");
            root = dir && *dir ? pathFrom(dir) : knownFolder(Folder::LocalAppData) / "GTAStoriesRecomp" / "install";
        }
#endif
        // the repository: the exe's folder (a release, or a clone after build.py --launcher), or one above it
        for (fs::path dir = exePath().parent_path(); root.empty() && !dir.empty(); dir = dir.parent_path())
        {
            if (fs::is_regular_file(dir / "tools" / "build.py"))
            {
                root = dir;
                break;
            }
            if (dir == dir.parent_path())
                break;
        }
        if (root.empty())
        {
            error = std::string(os::kLauncherExe) + " must stay in the game's folder, next to tools and src (it builds the "
                    "game from there). It was started from " + pathText(exePath().parent_path());
            return false;
        }
        // the build folder as the tools see it (toolchain.BUILD): VCS_BUILD_DIR, else .vcs-build-dir, else build/
        build = root / "build";
        if (const char *env = std::getenv("VCS_BUILD_DIR"); env && *env)
            build = pathFrom(env);
        else if (const std::string chosen = trim(readFile(root / ".vcs-build-dir")); !chosen.empty())
            build = pathFrom(chosen);
        {
            // an update replaces the running launcher by renaming it (tools/update.py set_aside): gone now
            std::error_code ec;
            fs::remove(root / (std::string(os::kLauncherExe) + ".old"), ec);
        }

        // a release's own python\ first; else the PC's, each tried (a py.exe can exist with no Python behind it, and
        // the Microsoft Store's python.exe stub only opens the Store). Linux: the distro's python3
        std::vector<std::vector<std::string>> candidates;
#ifdef _WIN32
        if (fs::path bundled = root / "python" / "python.exe"; fs::is_regular_file(bundled))
            candidates.push_back({pathText(bundled)});
        if (std::string exe = findOnPath("python.exe"); !exe.empty() && exe.find("WindowsApps") == std::string::npos)
            candidates.push_back({exe});
        if (std::string py = findOnPath("py.exe"); !py.empty())
            candidates.push_back({py, "-3"});
#else
        if (fs::path bundled = root / "python" / "bin" / "python3"; fs::is_regular_file(bundled))
            candidates.push_back({pathText(bundled)});
#if defined(__APPLE__)
        // without Apple's Command Line Tools /usr/bin/python3 is a stub that opens their installer: ask for them first
        if (candidates.empty() && !macCommandLineTools())
        {
            error = "Apple's Command Line Tools are needed (they bring the compiler, git and Python the build uses). "
                    "Install them - macOS opens its installer for: xcode-select --install - then start the launcher "
                    "again.";
            return false;
        }
#endif
        for (const char *name : {"python3", "python"})
            if (std::string exe = findOnPath(name); !exe.empty())
                candidates.push_back({exe});
#endif
        for (const auto &candidate : candidates)
        {
            std::vector<std::string> argv = candidate;
            argv.insert(argv.end(), {"-I", "-c", "import sys; print(sys.version_info >= (3, 8))"});
            Process probe;
            Process::Options options;
            std::string ignored;
            if (!probe.start(argv, options, ignored))
                continue;
            probe.wait();
            const auto lines = probe.takeLines();
            if (probe.exitCode() == 0 && !lines.empty() && lines.back() == "True")
            {
                python = candidate;
                break;
            }
        }
        if (python.empty())
        {
            error = os::kWindows
                        ? "Python 3 wasn't found. A release brings its own (the python folder next to the launcher); in a "
                          "git clone, install Python from python.org (tick \"Add python.exe to PATH\")."
                        : os::kMac ? "Python 3 wasn't found: install Apple's Command Line Tools (xcode-select --install)."
                                   : "Python 3 wasn't found: install your distribution's python3 package (Python 3.8 or newer).";
            return false;
        }
        pythonText = python[0];

        // an AppImage whose files aren't in the install folder yet, or are another version's: they go in place first
        // (tools/update.py install, the image's own copy; a task the window shows), then the folder opens
        if (!payload.empty() && readFile(payload / "RELEASE.json") != readFile(root / "RELEASE.json"))
        {
            std::error_code ec;
            fs::create_directories(root, ec);
            if (ec)
            {
                error = "Could not create the game's folder " + pathText(root) + " (" + ec.message() + ")";
                return false;
            }
            sourceVersion = trim(readFile(payload / "VERSION")); // the title bar's, before the folder opens
            runTool("install", "Putting the game's files in place",
                    {toolPath(payload, "update.py"), "install", "--from", pathText(payload), "--to", pathText(root)});
            return true;
        }
        loadFolder();
        return true;
    }

    void App::loadFolder()
    {
        // the scripts every button runs: all there, or the folder doesn't match this launcher - a release made from the
        // last commit while the launcher was built from newer, uncommitted code, or files lost on the way
        {
            std::string missing;
            int count = 0;
            for (const char *tool : kTools)
                if (!fs::is_regular_file(root / "tools" / tool))
                {
                    missing += std::string(count ? ", " : "") + tool;
                    ++count;
                }
            if (count)
                toolsProblem = "The launcher's scripts are missing from " + std::string(os::kTools) + " (" + missing +
                               "), so nothing can be checked. Download the release again. If you made this release "
                               "yourself, its scripts are older than the launcher: tools/release.py packages the last "
                               "commit - commit first, or make it with --worktree.";
        }

        // VERSION (the repository's, or the one a release wrote); a clone adds its commit, since the same number
        // covers many commits there
        sourceVersion = trim(readFile(root / "VERSION"));
        {
            std::string head = trim(readFile(root / ".git" / "HEAD"));
            if (head.rfind("ref:", 0) == 0)
                head = trim(readFile(root / ".git" / trim(head.substr(4))));
            if (head.size() >= 7)
                sourceVersion = sourceVersion.empty() ? "git " + head.substr(0, 7) : sourceVersion + " (" + head.substr(0, 7) + ")";
            else if (sourceVersion.empty())
                sourceVersion = "development";
        }
        loadSettings();
        m_games.clear();
        loadGame();
        if (stories) // [stories] Vice City's state for the GTA Stories tab
            refreshStoriesVcs();
        // [lcs] no game picked yet and none of the shown game's discs found: a disc of the other game decides
        if (!gameChosen && !disc.ok)
            for (const fs::path &candidate : discCandidates(root, gameId == "vcs" ? "lcs" : "vcs"))
                if (const DiscResult r = checkDisc(candidate); r.ok && r.game != gameId)
                {
                    selectGame(r.game);
                    break;
                }
    }

    fs::path App::gameBuild() const
    {
        // [lcs] tools/toolchain.py BUILD: VCS_BUILD_DIR is the game's folder itself; else the build folder's <game>/
        if (gameId == "vcs")
            return build;
        if (const char *env = std::getenv("VCS_BUILD_DIR"); env && *env)
            return build;
        return build / gameId;
    }

    std::string App::gameExe() const { return gameId + (os::kWindows ? ".exe" : ""); }

    const char *App::title() const { return gameTitle(gameId); }

    bool App::selectGame(const std::string &id)
    {
        // [stories] the GTA Stories tab: Liberty City Stories underneath (the game in front), Vice City's state read too
        if (id == "stories" || (id == "lcs" && stories))
        {
            if (id == shownId())
                return true;
            if (!canSwitchGame() || (gameId != "lcs" && !selectGame("lcs")))
                return false;
            stories = id == "stories";
            gameChosen = true;
            saveSettings();
            if (stories)
                refreshStoriesVcs();
            return true;
        }
        if (id == gameId && !stories)
            return true;
        if ((id != "vcs" && id != "lcs") || !canSwitchGame())
            return false;
        stories = false;
        // the shown game's state kept for when the player comes back; a check still out for it is dropped
        m_doctor.reset();
        doctorRunning = false;
        m_games[gameId] = {iso, disc, discChecked, discFound, doctor, backups, saves, doctorDone && !doctorCached,
                           doctorError};
        gameId = id;
        m_knownDiscs.clear();
        mods = Json(); // [mods] each game has its own: read again by the Mods sheet
        modsError.clear();
        modsNote.clear();
        gameChosen = true;
        gameEnded = false;
        saveSettings();
        if (const auto it = m_games.find(id); it != m_games.end())
        {
            const GameState &s = it->second;
            iso = s.iso;
            disc = s.disc;
            discChecked = s.discChecked;
            discFound = s.discFound;
            doctor = s.doctor;
            backups = s.backups;
            saves = s.saves;
            doctorDone = s.doctorDone;
            doctorCached = false;
            doctorError = s.doctorError;
            refresh(); // checked again: the other game's build may have changed the shared source
        }
        else
            loadGame();
        return true;
    }

    void App::loadGame()
    {
        iso.clear();
        disc = DiscResult();
        discChecked = discFound = false;
        doctor = backups = saves = Json();
        doctorDone = doctorCached = false;
        doctorError.clear();
        // the disc the game would use
        std::string iniIso;
        {
            std::ifstream in(gameBuild() / "vcs.ini");
            std::string line;
            while (std::getline(in, line))
                if (line.rfind("iso=", 0) == 0)
                    iniIso = trim(line.substr(4));
        }
        if (!iniIso.empty() && iniIso.front() == '"')
            iniIso = iniIso.substr(1, iniIso.size() - 2);
        if (!iniIso.empty())
        {
            // a relative iso= (written by hand) is the build folder's, or the source folder's, not the launcher's cwd
            fs::path isoPath = pathFrom(iniIso);
            std::error_code ec;
            if (isoPath.is_relative())
                for (const fs::path &base : {gameBuild(), build, root})
                    if (fs::is_regular_file(base / isoPath, ec))
                    {
                        isoPath = base / isoPath;
                        break;
                    }
            // [lcs] never switches games from here: another game's disc named in this game's vcs.ini is left out
            if (const DiscResult r = checkDisc(isoPath); !r.ok || r.game == gameId)
            {
                iso = isoPath;
                disc = r;
                discChecked = true;
            }
        }
        if (!disc.ok)
            scanForDisc();
        {
            // [lcs] the last answer for this game (one from before doctor named its game is VCS's)
            bool ok = false;
            Json cached = Json::parse(readFile(gameBuild() / "launcher_state.json"), &ok);
            const std::string cachedGame = ok && cached["game"]["id"].str().size() ? cached["game"]["id"].str() : "vcs";
            if (ok && cached.isObject() && !cached["next"].str().empty() && cachedGame == gameId)
            {
                doctor = cached;
                doctorDone = doctorCached = true;
            }
        }
        refresh();
    }

    bool App::canUpdate() const
    {
#ifdef _WIN32
        return true;
#else
        return !payload.empty();
#endif
    }

    void App::refresh()
    {
        if (m_doctor)
            return;
        if (!toolsProblem.empty()) // the scripts aren't there: nothing to run, say why
        {
            doctor = Json();
            doctorError = toolsProblem;
            doctorDone = true;
            return;
        }
        std::vector<std::string> argv = python;
        argv.insert(argv.end(), {"-I", toolPath(root, "doctor.py"), "--json", "--game", gameId});
        Process::Options options;
        options.cwd = root;
        options.env = {{"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"}};
        m_doctor = std::make_unique<Process>();
        m_doctorOut.clear();
        std::string error;
        if (!m_doctor->start(argv, options, error))
        {
            doctorError = error;
            m_doctor.reset();
            return;
        }
        doctorRunning = true;
    }

    void App::refreshStoriesVcs(bool useCache)
    {
        if (m_vcsDoctor || !toolsProblem.empty() || python.empty())
            return;
        // the last answer first (build/launcher_state.json names its game), then a fresh check
        if (useCache && !storiesVcsDone)
        {
            bool ok = false;
            Json cached = Json::parse(readFile(build / "launcher_state.json"), &ok);
            if (ok && cached.isObject() && cached["game"]["id"].str() == "vcs")
                storiesVcs = cached;
        }
        std::vector<std::string> argv = python;
        argv.insert(argv.end(), {"-I", toolPath(root, "doctor.py"), "--json", "--game", "vcs"});
        Process::Options options;
        options.cwd = root;
        options.env = {{"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"}};
        m_vcsDoctor = std::make_unique<Process>();
        m_vcsDoctorOut.clear();
        std::string error;
        if (!m_vcsDoctor->start(argv, options, error))
        {
            m_vcsDoctor.reset();
            storiesVcsDone = true;
            return;
        }
        storiesVcsRunning = true;
    }

    void App::say(const std::string &text, double now)
    {
        toast = text;
        toastTime = now < 0 ? m_now : now;
    }

    void App::update(double now)
    {
        m_now = now;
        if (m_doctor)
        {
            for (auto &line : m_doctor->takeLines())
                m_doctorOut.push_back(std::move(line));
            if (!m_doctor->running())
            {
                for (auto &line : m_doctor->takeLines())
                    m_doctorOut.push_back(std::move(line));
                bool ok = false;
                for (auto it = m_doctorOut.rbegin(); it != m_doctorOut.rend() && !ok; ++it)
                    if (!it->empty() && (*it)[0] == '{')
                    {
                        Json parsed = Json::parse(*it, &ok);
                        if (ok)
                        {
                            doctor = parsed;
                            std::error_code ec;
                            fs::create_directories(gameBuild(), ec);
                            std::ofstream(gameBuild() / "launcher_state.json", std::ios::binary | std::ios::trunc) << *it << '\n';
                        }
                    }
                doctorError.clear();
                if (!ok)
                {
                    doctor = Json(); // no answer: nothing below may read an old or empty one as "missing"
                    doctorError = "tools/doctor.py failed";
                    for (size_t i = m_doctorOut.size() > 12 ? m_doctorOut.size() - 12 : 0; i < m_doctorOut.size(); ++i)
                        doctorError += "\n" + m_doctorOut[i];
                }
                doctorRunning = false;
                doctorDone = true;
                doctorCached = false;
                m_doctor.reset();
            }
        }

        if (m_vcsDoctor) // [stories] Vice City Stories' answer for the GTA Stories tab
        {
            for (auto &line : m_vcsDoctor->takeLines())
                m_vcsDoctorOut.push_back(std::move(line));
            if (!m_vcsDoctor->running())
            {
                for (auto &line : m_vcsDoctor->takeLines())
                    m_vcsDoctorOut.push_back(std::move(line));
                storiesVcs = Json();
                for (auto it = m_vcsDoctorOut.rbegin(); it != m_vcsDoctorOut.rend(); ++it)
                    if (!it->empty() && (*it)[0] == '{')
                    {
                        bool ok = false;
                        Json parsed = Json::parse(*it, &ok);
                        if (ok)
                        {
                            storiesVcs = parsed;
                            break;
                        }
                    }
                storiesVcsRunning = false;
                storiesVcsDone = true;
                m_vcsDoctor.reset();
            }
        }

        if (task && task->process)
        {
            for (const auto &line : task->process->takeLines())
                handleLine(*task, line);
            if (!task->process->running())
            {
                for (const auto &line : task->process->takeLines())
                    handleLine(*task, line);
                task->exitCode = task->process->exitCode();
                task->process.reset();
                finishTask(*task);
            }
        }

        if (game && gameRunning && !game->running())
        {
            gameExit = game->exitCode();
            gameRunning = false;
            gameEnded = true;
            gameEnd = now;
#ifdef _WIN32
            game->kill(); // [launcherfix] what the game's own process left behind (Stories: a partner whose script ended)
#endif
            game.reset();
            gameStopAsked = 0;
            gameKilled = false;
        }
        else if (game && gameRunning && gameStopAsked > 0 && !gameKilled && now - gameStopAsked >= kGameStopGrace)
        {
            gameKilled = true; // [launcherfix] it did not close: the whole tree goes
            game->kill();
        }
    }

    void App::handleLine(Task &t, const std::string &line)
    {
        if (line.rfind(kEventPrefix, 0) == 0)
        {
            bool ok = false;
            const Json e = Json::parse(std::string_view(line).substr(5), &ok);
            if (!ok)
                return;
            if (e.has("stage"))
            {
                if (!t.stages.empty() && t.stages.back().end == 0)
                    t.stages.back().end = m_now;
                t.stages.push_back({e["stage"].str(), e["title"].str(), m_now, 0});
                t.done = t.total = 0;
                t.eta = -1;
                t.note.clear();
            }
            if (e.has("progress"))
            {
                t.done = int(e["progress"].num());
                t.total = int(e["total"].num());
                t.eta = e["eta"].type() == Json::Type::Number ? int(e["eta"].num()) : -1;
                t.note = e["note"].str();
            }
            if (e.has("error") && t.errorCode.empty())
            {
                t.errorCode = e["error"].str();
                t.errorMessage = e["message"].str();
                t.errorLog = e["log"].str();
            }
            if (e.has("result"))
                t.result = e;
            return;
        }
        t.log.push_back(line);
        if (t.log.size() > kLogLines)
            t.log.pop_front();
    }

    void App::finishTask(Task &t)
    {
        t.finished = true;
        t.endTime = m_now;
        if (!t.stages.empty() && t.stages.back().end == 0)
            t.stages.back().end = m_now;
        keepAwake(false);
        if (!t.cancelled && t.exitCode != 0 && t.errorCode.empty())
        {
            t.errorCode = t.kind == "install" ? "install_failed" : "python_failed";
            t.errorMessage = t.title + " stopped (exit code " + std::to_string(t.exitCode) + ")";
        }
        if (t.kind == "install" && t.ok())
        {
            task.reset(); // the files are in place: the folder opens as if they had always been there
            loadFolder();
            return;
        }
        if (t.kind == "build" && t.game == "vcs" && gameId != "vcs") // [storiesbuild] Vice City built from the GTA Stories tab
        {
            m_vcsDoctor.reset();
            storiesVcsRunning = false;
            refreshStoriesVcs(false);
        }
        if (t.kind == "build_dir_check" && t.result.has("path"))
        {
            buildDirCheck = t.result; // "can't use it" is an answer too (exit 1): the confirmation shows why
            task.reset();
            return;
        }
        if (t.kind == "build_dir_set" && t.ok())
        {
            build = pathFrom(t.result["path"].str());
            loadSettings(); // launcher.ini came along
            m_games.clear(); // [lcs] each game's folder moved with it: read again
            say(t.result["old_built"].boolean()
                    ? "Build folder changed. The game is built again there; the old folder can be deleted afterwards: " +
                          t.result["previous"].str()
                    : "Build folder changed: " + t.result["path"].str());
            task.reset();
            loadGame();
            return;
        }
        // [stories] the packs before Play: a failure is said, never a stop (the game plays without that pack)
        if (t.kind == "packs")
        {
            packsResult = t.result;
            packsNote.clear();
            const Json &failed = t.result["failed"];
            if (!t.ok() && !t.cancelled)
                packsNote = "The Stories packs couldn't be checked (" +
                            (t.errorMessage.empty() ? t.title + " stopped" : t.errorMessage) + "): playing without them.";
            else if (failed.size())
                packsNote = "Couldn't make " + failed[0]["title"].str() + " (" + failed[0]["message"].str() +
                            "): playing without it.";
            if (!packsNote.empty())
                say(packsNote);
            else if (const Json &built = t.result["built"]; built.size())
                say(built.size() == 1 ? "Made " + built[0]["title"].str() + " from your disc"
                                      : "Made " + std::to_string(built.size()) + " Stories packs from your discs");
            task.reset();
            return;
        }
        // [mods] a mod command: its answer, then the list again; the list: kept for the Mods sheet
        if (t.kind == "mods_list" || t.kind == "mods")
        {
            const bool change = t.kind == "mods";
            if (t.ok())
            {
                modsError.clear();
                if (!change)
                    mods = t.result;
                else
                {
                    const std::string cmd = t.result["command"].str();
                    modsNote = cmd == "import"     ? (t.result["storage"].str() == "in_place" ? "Added " : "Imported ") +
                                                         t.result["mod"]["name"].str() +
                                                         (t.result["storage"].str() == "in_place"
                                                              ? " (kept where it is: don't move or delete it): applies at the next start"
                                                              : ": applies at the next start")
                               : cmd == "relocate" || cmd == "store"
                                   ? t.result["name"].str() + (t.result["storage"].str() == "in_place"
                                                                   ? ": read where it is (the app's copy is deleted)"
                                                                   : ": the app keeps its own copy now")
                               : cmd == "remove"   ? "Removed " + t.result["name"].str()
                               : cmd == "retarget" ? "Now into " + t.result["targetName"].str() + " (picked by you)"
                               : cmd == "prefer"   ? std::string("Conflict settled: the other mod's line is set aside")
                               : t.result.has("animations") ? "Made " + t.result["name"].str() + ": applies at the next start"
                               : t.result.has("built") // [stories] stories_auto.py (the swim button)
                                   ? (t.result["failed"].size() ? std::string("Couldn't make it: ") + t.result["failed"][0]["message"].str()
                                      : t.result["built"].size() ? "Made " + t.result["built"][0]["title"].str() + " again: applies at the next start"
                                                                 : std::string("Nothing made: no Vice City Stories disc"))
                                                   : std::string("Saved: applies at the next start");
                }
            }
            else
                modsError = t.errorMessage.empty() ? t.title + " failed" : t.errorMessage;
            task.reset();
            if (change)
                startModsList();
            return;
        }
        if (t.kind == "backup_list" && t.ok())
        {
            backups = t.result["backups"];
            saves = t.result["saves"];
        }
        if (t.kind == "rip_list" && t.ok())
            drives = t.result["drives"];
        if ((t.kind == "update_check") && t.ok())
            updateInfo = t.result;
        if (t.kind == "update" && t.ok() && t.result["restart"].boolean())
        {
            updatedTo = t.result["version"].str(); // in place: this launcher is the old one until it restarts
            updateInfo = Json();
        }
        if (t.kind == "rip" && t.ok() && t.result.has("iso"))
            chooseDisc(pathFrom(t.result["iso"].str()));
        if (t.kind == "backup" && t.ok())
            say("Saves backed up: " + t.result["path"].str());
        if (t.kind == "restore" && t.ok())
            say("Saves restored (the card before it was backed up too)");
        if (t.kind == "update_check" && t.ok() && !updateQuiet && !t.result["newer"].boolean())
            say(t.result["message"].str().empty() ? "You have the latest release." : t.result["message"].str());
        // a quiet check that found nothing new, or failed, leaves no trace
        if (t.kind == "update_check" && updateQuiet && (!t.ok() || !t.result["newer"].boolean()))
            task.reset();
        else if ((t.kind == "backup_list" || t.kind == "rip_list" || t.kind == "update_check" || t.kind == "backup" ||
                  t.kind == "restore") && t.ok())
            task.reset(); // their results show elsewhere
        refresh();
    }

    void App::runTool(const std::string &kind, const std::string &title, const std::vector<std::string> &args)
    {
        if (busy())
            return;
        task = std::make_unique<Task>();
        task->kind = kind;
        task->title = title;
        task->startTime = m_now;
        std::vector<std::string> argv = python;
        argv.push_back("-I");
        // the release's own copy of the scripts (inside GTAStoriesLauncher.app, or the AppImage): no __pycache__ written
        // there - in the app bundle it breaks the signature (a sealed resource added), so macOS would refuse the app
        if (!payload.empty() && !args.empty() && args[0].rfind(pathText(payload), 0) == 0)
            argv.push_back("-B");
        argv.insert(argv.end(), args.begin(), args.end());
        Process::Options options;
        options.cwd = root;
        options.env = {{"VCS_PROGRESS", "json"}, {"PYTHONUTF8", "1"}, {"PYTHONIOENCODING", "utf-8"},
                       {"PYTHONUNBUFFERED", "1"}};
#if defined(__APPLE__)
        // a release's app bundle (Contents/MacOS/GTAStoriesLauncher): update.py apply replaces it
        if (!payload.empty())
            options.env.push_back({"VCS_APP_BUNDLE", pathText(exePath().parent_path().parent_path().parent_path())});
#endif
        task->process = std::make_unique<Process>();
        std::string error;
        if (!task->process->start(argv, options, error))
        {
            task->process.reset();
            task->errorCode = "python_failed";
            task->errorMessage = error;
            finishTask(*task);
        }
    }

    void App::startBuild() { startBuildFor(gameId); }

    // [storiesbuild] one game's build, whichever tab is shown: the shown game's own disc, or the other's from knownDisc()
    void App::startBuildFor(const std::string &game)
    {
        const fs::path gameIso = game == gameId ? iso : knownDisc(game);
        std::vector<std::string> args = {toolPath(root, "build.py"), "--game", game, "--recompile-if-needed",
                                         pathText(gameIso), "--progress", "json"};
        // [lcs] PGO is VCS's (its training run): another game gets ThinLTO instead
        const std::string opt = settings.opt == "pgo" && game != "vcs" ? "lto" : settings.opt;
        if (!opt.empty())
            args.insert(args.end(), {"--opt", opt});
        const bool wasBusy = busy();
        runTool("build", std::string("Building ") + gameTitle(game), args);
        if (!wasBusy && task)
            task->game = game;
        if (busy())
            keepAwake(true);
    }

    bool App::storiesCityNeedsBuild(const std::string &game) const
    {
        const Json &report = game == "vcs" ? storiesVcs : doctor;
        if (!report.isObject())
            return true; // no answer: building is quick when nothing changed (--recompile-if-needed)
        return !report["build"]["built"].boolean() || !report["build"]["current"].boolean();
    }

    bool App::storiesToolsNeeded() const
    {
        return doctor.isObject() && (!doctor["compiler"]["ok"].boolean() || !doctor["downloads"]["ok"].boolean());
    }

    bool App::startStoriesBuild(bool thenPlay)
    {
        if (busy() || gameRunning || storiesPlan.active || !stories)
            return false;
        if (knownDisc("lcs").empty() || knownDisc("vcs").empty())
            return false;
        StoriesPlan plan;
        plan.active = true;
        plan.play = thenPlay;
        const bool lcs = storiesCityNeedsBuild("lcs"), vcs = storiesCityNeedsBuild("vcs");
        if ((lcs || vcs) && storiesToolsNeeded())
            plan.steps.push_back("tools");
        if (lcs)
            plan.steps.push_back("lcs");
        if (vcs)
            plan.steps.push_back("vcs");
        storiesPlan = plan;
        dismissTask();
        return true;
    }

    bool App::stepStoriesBuild()
    {
        StoriesPlan &p = storiesPlan;
        if (!p.active)
            return false;
        if (task && !task->finished)
        {
            p.failed = false; // a retry from the error card is running
            return false;
        }
        if (task)
        {
            const bool ours = p.started && (task->kind == "build" || task->kind == "tools");
            if (!ours)
                return false; // another finished task is on screen: wait for it to go
            if (task->cancelled)
            {
                p = StoriesPlan(); // stopped: its card says so; Start again is that one build
                return false;
            }
            if (!task->ok())
            {
                p.failed = true; // its error card: Try again goes on from here
                return false;
            }
            dismissTask();
            p.started = false;
            ++p.index;
        }
        else if (p.started) // the error card was closed
        {
            p = StoriesPlan();
            return false;
        }
        if (p.index >= p.steps.size())
        {
            const bool play = p.play;
            p = StoriesPlan();
            return play;
        }
        const std::string &step = p.steps[p.index];
        if (step == "tools")
            startInstallTools();
        else
            startBuildFor(step);
        p.started = true;
        return false;
    }

    void App::startInstallTools()
    {
#ifdef _WIN32
        runTool("tools", "Installing the build tools", {toolPath(root, "setup_vs.py"), "install"});
#elif defined(__APPLE__)
        // Apple's Command Line Tools: build.py --install-deps runs xcode-select --install (macOS' own installer)
        runTool("tools", "Installing Apple's Command Line Tools", {toolPath(root, "build.py"), "--install-deps", "--progress", "json"});
#else
        // the distribution's packages, through pkexec (the desktop's password prompt): build.py --install-deps
        runTool("tools", "Installing the packages the build needs",
                {toolPath(root, "build.py"), "--install-deps", "--progress", "json"});
#endif
    }

    // [lcs] the shown game's card (backup.py --game: GTAStoriesRecomp/VCS, GTAStoriesRecomp/LCS)
    void App::startBackup()
    {
        runTool("backup", "Backing up your saves", {toolPath(root, "backup.py"), "--game", gameId, "create"});
    }

    void App::startBackupList()
    {
        runTool("backup_list", "Listing backups", {toolPath(root, "backup.py"), "--game", gameId, "list"});
    }

    // [mods] tools/mods.py for the shown game (its data folder: backup.py's, VCS_DATA_DIR)
    void App::startModsList()
    {
        runTool("mods_list", "Reading the mods", {toolPath(root, "mods.py"), "--game", gameId, "list"});
    }

    void App::startModsCommand(const std::vector<std::string> &args, const std::string &title)
    {
        modsNote.clear();
        std::vector<std::string> argv = {toolPath(root, "mods.py"), "--game", gameId};
        argv.insert(argv.end(), args.begin(), args.end());
        runTool("mods", title, argv);
    }

    fs::path App::knownDisc(const std::string &game) const
    {
        if (game == gameId)
            return disc.ok ? iso : fs::path();
        if (const auto it = m_knownDiscs.find(game); it != m_knownDiscs.end())
            return it->second;
        fs::path found;
        if (const auto it = m_games.find(game); it != m_games.end() && it->second.disc.ok && it->second.disc.game == game)
            found = it->second.iso;
        else
        {
            // that game's own vcs.ini (gameBuild() for it: build/ for VCS, build/<game>/ for the others)
            const char *env = std::getenv("VCS_BUILD_DIR");
            const fs::path folder = game == "vcs" || (env && *env) ? build : build / game;
            std::ifstream in(folder / "vcs.ini");
            std::string line, value;
            while (std::getline(in, line))
                if (line.rfind("iso=", 0) == 0)
                    value = trim(line.substr(4));
            if (!value.empty() && value.front() == '"')
                value = value.substr(1, value.size() - 2);
            if (!value.empty())
            {
                fs::path p = pathFrom(value);
                if (p.is_relative())
                    p = folder / p;
                if (const DiscResult r = checkDisc(p); r.ok && r.game == game)
                    found = p;
            }
        }
        // [storiesbuild] not in its vcs.ini: the environment's (VCS_ISO / LCS_ISO, as tools/stories.py reads them), else the
        // places players keep discs (the scan the game's own tab does) - so Play always has both discs to hand over
        if (found.empty())
        {
            const char *env = std::getenv(game == "vcs" ? "VCS_ISO" : "LCS_ISO");
            if (env && *env)
                if (const DiscResult r = checkDisc(pathFrom(env)); r.ok && r.game == game)
                    found = pathFrom(env);
        }
        if (found.empty())
            for (const fs::path &candidate : discCandidates(root, game))
                if (const DiscResult r = checkDisc(candidate); r.ok && r.game == game)
                {
                    found = candidate;
                    break;
                }
        m_knownDiscs[game] = found;
        return found;
    }

    void App::startSwimPack(const fs::path &vcsIso)
    {
        modsNote.clear();
        runTool("mods", "Making the swim animations from your Vice City Stories disc",
                {toolPath(root, "stories_auto.py"), "--for", "lcs", "--only", "swim", "--force", "--vcs-iso", pathText(vcsIso)});
    }

    bool App::packsWanted() const
    {
        // [stories] the other game's disc known: Liberty City (alone or in front) gets Vice City's packs; Vice City gets
        // Liberty City's when the tool supports that direction (stories_auto.py decides; quick when nothing is due)
        if (python.empty() || !fs::is_regular_file(root / "tools" / "stories_auto.py"))
            return false;
        return !knownDisc(gameId == "vcs" ? "lcs" : "vcs").empty();
    }

    void App::startPacks()
    {
        std::vector<std::string> args = {toolPath(root, "stories_auto.py"), "--for", packsMode()};
        if (const fs::path l = knownDisc("lcs"); !l.empty())
            args.insert(args.end(), {"--lcs-iso", pathText(l)});
        if (const fs::path v = knownDisc("vcs"); !v.empty())
            args.insert(args.end(), {"--vcs-iso", pathText(v)});
        runTool("packs", "Getting the Stories packs ready", args);
    }

    void App::startRestore(const std::string &name)
    {
        runTool("restore", "Restoring saves", {toolPath(root, "backup.py"), "--game", gameId, "restore", name});
    }

    void App::startReport() { runTool("report", "Making a report", {toolPath(root, "report.py"), "--game", gameId}); }

    void App::startUpdateCheck(bool quiet)
    {
        updateQuiet = quiet;
        runTool("update_check", "Checking for updates", {toolPath(root, "update.py"), "check"});
    }

    void App::startUpdate() { runTool("update", "Updating", {toolPath(root, "update.py"), "apply"}); }

    void App::startBuildDirCheck(const fs::path &path)
    {
        buildDirCheck = Json();
        runTool("build_dir_check", "Checking the folder", {toolPath(root, "build_dir.py"), "check", pathText(path)});
    }

    void App::startBuildDirSet(const fs::path &path)
    {
        runTool("build_dir_set", "Changing the build folder", {toolPath(root, "build_dir.py"), "set", pathText(path)});
    }

    bool App::buildCustom() const
    {
        std::error_code ec;
        return !fs::equivalent(build, root / "build", ec);
    }

    void App::startRipList() { runTool("rip_list", "Looking for discs", {toolPath(root, "rip_disc.py"), "list"}); }

    void App::startRip(const std::string &drive, const fs::path &out)
    {
        runTool("rip", "Copying the disc", {toolPath(root, "rip_disc.py"), "rip", drive, pathText(out)});
        if (busy())
            keepAwake(true);
    }

    void App::cancelTask()
    {
        if (!busy() || !task->process)
            return;
        task->cancelled = true;
        task->process->kill();
    }

    void App::chooseDisc(const fs::path &path, bool byScan)
    {
        DiscResult result = checkDisc(path);
        if (byScan && (!result.ok || result.game != gameId))
            return;
        // [stories] the GTA Stories tab takes Vice City's disc too, for Vice City (its own vcs.ini), staying on the tab
        if (stories && result.ok && result.game == "vcs" && !byScan)
        {
            // (with VCS_BUILD_DIR both games share one vcs.ini: Liberty City's iso= stays)
            if (const char *env = std::getenv("VCS_BUILD_DIR"); env && *env)
                say("Vice City Stories' disc is chosen on its own tab when VCS_BUILD_DIR is set");
            else if (!writeIsoToIni(path, build))
                say("Could not write " + pathText(build / "vcs.ini") + " - is the folder read-only?");
            else
                say("Vice City Stories disc verified: " + pathText(path.filename()));
            m_knownDiscs.clear();
            if (auto it = m_games.find("vcs"); it != m_games.end())
            {
                it->second.iso = path;
                it->second.disc = result;
                it->second.discChecked = true;
            }
            refreshStoriesVcs();
            return;
        }
        // [lcs] the other game's disc: that game is shown, with this disc (each game keeps its own)
        if (result.ok && result.game != gameId)
        {
            const std::string from = gameTitle(gameId);
            if (!selectGame(result.game))
            {
                result.ok = false;
                result.code = "other_game";
                result.message = std::string("this is ") + gameTitle(result.game) + "'s disc: switch games when " +
                                 (gameRunning ? "the game has ended" : "the task has finished");
            }
            else // said, not silent: a player who picked the wrong file sees why the window changed (2026-11-18)
                say(std::string("A ") + gameTitle(result.game) + " disc: switched to that game (" + from +
                    " is one click away, top left)");
        }
        iso = path;
        disc = result;
        discChecked = true;
        discFound = byScan;
        if (disc.ok && !writeIsoToIni(path))
            say("Could not write " + pathText(gameBuild() / "vcs.ini") + " - is the folder read-only?");
        if (!byScan)
        {
            // a check already out (a game switch starts one) didn't know this disc yet: again, with it
            m_doctor.reset();
            doctorRunning = false;
            refresh();
        }
    }

    DiscResult App::chooseStoriesDisc(const fs::path &path)
    {
        const DiscResult result = checkDisc(path);
        if (!result.ok)
        {
            say("That isn't a disc the game can use: " + result.message);
            return result;
        }
        chooseDisc(path);
        return result;
    }

    void App::scanForDisc()
    {
        for (const fs::path &candidate : discCandidates(root, gameId))
        {
            chooseDisc(candidate, true);
            if (disc.ok)
                return;
        }
    }

    bool App::writeIsoToIni(const fs::path &path, const fs::path &folderIn)
    {
        // vcs.exe's settings keep every line they don't own (src/settings/settings.cpp), iso= among them. [lcs] The
        // game's own vcs.ini (build/lcs/vcs.ini), where build.py and lcs.exe look
        const fs::path folder = folderIn.empty() ? gameBuild() : folderIn;
        const fs::path ini = folder / "vcs.ini";
        std::vector<std::string> lines;
        {
            std::ifstream in(ini);
            std::string line;
            while (std::getline(in, line))
                if (line.rfind("iso=", 0) != 0)
                    lines.push_back(line);
        }
        lines.push_back("iso=" + pathText(path));
        std::error_code ec;
        fs::create_directories(folder, ec);
        std::ofstream out(ini, std::ios::trunc | std::ios::binary);
        for (const auto &l : lines)
            out << (l.empty() || l.back() != '\r' ? l : l.substr(0, l.size() - 1)) << "\n";
        return bool(out);
    }

    bool App::storiesAvailable() const
    {
        const fs::path vcsExe = build / (os::kWindows ? "vcs.exe" : "vcs");
        const fs::path lcsExe = build / "lcs" / (os::kWindows ? "lcs.exe" : "lcs");
        return fs::is_regular_file(vcsExe) && fs::is_regular_file(lcsExe);
    }

    bool App::oneProgramAvailable() const
    {
        if (!os::kWindows)
            return false;
        const fs::path dir = build / "oneproc";
        return fs::is_regular_file(dir / "lcs" / "lcs_game.dll") && fs::is_regular_file(dir / "vcs" / "vcs_game.dll") &&
               fs::is_regular_file(dir / "lcs" / "stories.exe");
    }

    void App::stopGame()
    {
        if (!game || !gameRunning || gameStopAsked > 0)
            return;
        gameStopAsked = m_now > 0 ? m_now : 1e-3;
        gameKilled = false;
        game->requestClose();
    }

    bool App::play(std::string &error, int slot)
    {
        if (gameRunning)
            return true;
        // [lcs] the shown game's binary in its own folder (build/vcs.exe, build/lcs/lcs.exe); it picks its own data
        // folder (src/main.cpp: GTAStoriesRecomp/VCS, GTAStoriesRecomp/LCS - or VCS_DATA_DIR, which it inherits)
        // [storiesbuild] the GTA Stories tab never starts half a game: both cities built, both discs known
        if (stories && gameId == "lcs")
        {
            if (knownDisc("lcs").empty() || knownDisc("vcs").empty())
            {
                error = std::string(knownDisc("lcs").empty() ? "Liberty City Stories" : "Vice City Stories") +
                        "' disc is needed: choose it on the GTA Stories tab";
                return false;
            }
            if (!storiesAvailable())
            {
                error = "Build both cities first (Build and play on the GTA Stories tab)";
                return false;
            }
        }
        const fs::path exe = gameBuild() / gameExe();
        if (!fs::is_regular_file(exe))
        {
            error = pathText(exe) + " isn't there: build the game first";
            return false;
        }
        gameLog = gameBuild() / "run" / "launcher_game.log";
        Process::Options options;
        options.cwd = gameBuild();
        if (slot >= 1 && slot <= 8) // the boot's load takes that slot (src/game/lcs_save.cpp, vcs_save_anywhere.cpp)
            options.env.push_back({"VCS_LOAD_SLOT", std::to_string(slot)});
        // [hero] GTA Stories: one hero in both cities (src/game/stories_hero.h); both games get it (tools/stories.py
        // inherits it). A single game plays its own hero.
        if (storiesPlay() && (settings.storiesHero == "vic" || settings.storiesHero == "toni" || settings.storiesHero == "city"))
            options.env.push_back({"STORIES_HERO", settings.storiesHero});
        if (storiesPlay() && !settings.storiesSaveSync) // [savesync] default on in tools/stories.py
            options.env.push_back({"STORIES_SAVE_SYNC", "0"});
        if (storiesPlay()) // [missionborder] tools/stories.py inherits it
            options.env.push_back({"STORIES_DW_MISSION_BORDER", settings.missionBorder});
        options.outputFile = gameLog;
        options.killOnClose = false; // the game plays on when the launcher closes
        game = std::make_unique<Process>();
        // [dualworld] Stories: both cities (experimental, docs/STORIES.md): tools/stories.py starts LCS shown and VCS
        // hidden behind it, and ends when LCS does (the game process the launcher watches). [stories] The GTA Stories tab
        std::vector<std::string> argv = {pathText(exe), pathText(iso)};
        if (storiesPlay() && !python.empty())
        {
            argv = python;
            // [storiesbuild] both discs, always, and the environment's cannot stand in for one the launcher never had
            argv.insert(argv.end(), {"-I", pathText(root / "tools" / "stories.py"), "--lcs-iso", pathText(knownDisc("lcs")),
                                     "--vcs-iso", pathText(knownDisc("vcs"))});
            if (settings.oneProgram && oneProgramAvailable()) // [stories] both cities in one program (stories.exe)
                argv.push_back("--oneproc");
            options.cwd = root;
            gameLog = gameBuild() / "run" / "launcher_stories.log";
            options.outputFile = gameLog;
        }
        if (!game->start(argv, options, error))
        {
            game.reset();
            return false;
        }
        gameRunning = true;
        gameEnded = false;
        gameStopAsked = 0;
        gameKilled = false;
        gameStart = m_now;
        return true;
    }

    fs::path App::restartTarget() const
    {
        // Linux: the AppImage file (update.py renamed the new one over it; the old image stays mounted until we end)
        if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage && !payload.empty())
            return pathFrom(appimage);
#if defined(__APPLE__)
        // macOS: the app bundle is replaced where it is (update.py), so the new one is at our own path
        return exePath();
#else
        // Windows: the release's launcher, written fresh by update.py (this one runs as GTAStoriesLauncher.exe.old)
        return root / os::kLauncherExe;
#endif
    }

    bool App::restart(std::string &error)
    {
        const fs::path exe = restartTarget();
        if (!fs::is_regular_file(exe))
        {
            error = "The new launcher isn't there: " + pathText(exe);
            return false;
        }
        Process::Options options;
        options.cwd = root;
        // its output to a file, not a pipe: the pipe's reader (us) is about to end
        options.outputFile = build / "run" / "launcher_restart.log";
        options.killOnClose = false; // it outlives us
        m_restarted = std::make_unique<Process>();
        if (!m_restarted->start({pathText(exe), "--after-update", std::to_string(processId())}, options, error))
        {
            m_restarted.reset();
            return false;
        }
        return true;
    }

    std::string App::gameExitText() const
    {
        const unsigned code = static_cast<unsigned>(gameExit);
        switch (gameExit)
        {
        case 0: return "";
        case 2: return "The game found no disc image. Choose your disc again.";
        case 3: return "The game could not read the disc image (moved, or on a drive that's gone?).";
        case 4: return std::string("The game says this is not the supported disc (GTA: ") + title() + ", USA).";
        case 5: return "The game could not start its window, graphics or sound. Update the graphics driver; "
                       "the log has the details.";
        default: break;
        }
#ifndef _WIN32
        // a death by signal N is 128 + N (model/process_posix.cpp)
        if (gameExit > 128 && gameExit < 128 + 32)
        {
            const int sig = gameExit - 128;
            const char *what = sig == 11 ? "segmentation fault" : sig == 4 ? "illegal instruction - built for a newer processor?"
                             : sig == 6  ? "aborted" : sig == 7 ? "bus error" : sig == 8 ? "arithmetic error"
                             : sig == 9  ? "killed - out of memory?" : nullptr;
            if (sig == 15 || sig == 2 || sig == 1)
                return "The game was stopped from outside (signal " + std::to_string(sig) + ").";
            const std::string name = what ? std::string(what) : "signal " + std::to_string(sig);
            return "The game crashed (" + name + "). The log has the details: please copy a report.";
        }
#endif
        if (code == 0xC0000005u)
            return "The game crashed (access violation). The log has the details: please copy a report.";
        if (code >= 0xC0000000u)
        {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08X", code);
            return std::string("The game crashed (") + hex + "). The log has the details: please copy a report.";
        }
        return "The game stopped with an error (code " + std::to_string(gameExit) + "). The log has the details.";
    }

    bool App::shortcut(std::string &error)
    {
        const fs::path desktop = knownFolder(Folder::Desktop);
        if (desktop.empty())
        {
            error = "no Desktop folder";
            return false;
        }
        // an AppImage starts from its own file, not from where it is mounted this time
        fs::path target = exePath();
        if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage && !payload.empty())
            target = pathFrom(appimage);
        return createShortcut(desktop / os::kShortcutName, target, root,
                              "GTA Stories Recompiled - launcher", error);
    }

    fs::path App::savesFolder() const
    {
        if (const char *dir = std::getenv("VCS_DATA_DIR"); dir && *dir)
            return pathFrom(dir);
        // [separate] each game's own folder (src/main.cpp): GTAStoriesRecomp/VCS and GTAStoriesRecomp/LCS
        return knownFolder(Folder::LocalAppData) / "GTAStoriesRecomp" / (gameId == "vcs" ? "VCS" : "LCS");
    }

    std::string App::next() const
    {
        if (!disc.ok)
            return "disc";
        if (doctorFailed())
            return "recheck";
        const std::string n = doctor["next"].str();
        if (n == "disc") // the doctor ran before the disc was chosen
            return doctorDone ? "build" : "";
        return n;
    }

    void App::loadSettings()
    {
        bool legacyBoth = false;
        std::ifstream in(build / "launcher.ini");
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "after_launch" && (value == "minimize" || value == "close" || value == "stay"))
                settings.afterLaunch = value;
            else if (key == "opt" && (value.empty() || value == "lto" || value == "none" || value == "pgo"))
                settings.opt = value;
            else if (key == "check_updates")
                settings.checkUpdates = value == "1";
            else if (key == "theme" && (value == "system" || value == "light" || value == "dark"))
                settings.theme = value;
            else if (key == "reduce_motion")
                settings.reduceMotion = value == "1";
            else if (key == "stories_hero" && (value == "vic" || value == "toni" || value == "city")) // [hero]
                settings.storiesHero = value;
            else if (key == "stories_save_sync") // [savesync]
                settings.storiesSaveSync = value != "0";
            else if (key == "stories_mission_border" && (value == "block" || value == "fail")) // [missionborder]
                settings.missionBorder = value;
            else if (key == "stories_one_program") // [stories] GTA Stories as one program
                settings.oneProgram = value == "1";
            else if (key == "stories_both") // [stories] the old Settings switch (Experimental): the GTA Stories tab now
                legacyBoth = value == "1";
            else if (key == "game" && (value == "vcs" || value == "lcs" || value == "stories")) // [lcs] the game shown last
            {
                gameId = value == "vcs" ? "vcs" : "lcs";
                stories = value == "stories"; // [stories] the GTA Stories tab: Liberty City underneath
                gameChosen = true;
            }
        }
        // [stories] launcher.ini from before the tab: "Stories: both cities" on, Liberty City shown -> the GTA Stories tab
        if (legacyBoth && !stories && gameId == "lcs")
            stories = true;
    }

    void App::saveSettings()
    {
        if (keepSettings)
            return;
        std::error_code ec;
        fs::create_directories(build, ec);
        std::ofstream out(build / "launcher.ini", std::ios::trunc | std::ios::binary);
        out << "# the launcher's own settings (the game's are in vcs.ini)\n"
            << "after_launch=" << settings.afterLaunch << "\n"
            << "opt=" << settings.opt << "\n"
            << "check_updates=" << (settings.checkUpdates ? 1 : 0) << "\n"
            << "theme=" << settings.theme << "\n"
            << "reduce_motion=" << (settings.reduceMotion ? 1 : 0) << "\n"
            << "stories_hero=" << settings.storiesHero << "\n"
            << "stories_save_sync=" << (settings.storiesSaveSync ? 1 : 0) << "\n"
            << "stories_mission_border=" << settings.missionBorder << "\n"
            << "stories_one_program=" << (settings.oneProgram ? 1 : 0) << "\n";
        if (gameChosen)
            out << "game=" << shownId() << "\n";
    }
}
