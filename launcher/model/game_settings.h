// The game's own settings, for the Game settings sheet (screens/game_settings.cpp; docs/LAUNCHER.md "Game settings").
//
// One source: the game's settings registry (src/settings). The sheet asks the built game for it - `lcs.exe
// --dump-settings-json` (vcs.exe likewise) prints every setting's key, page, section, label, help, type, range,
// choices, this run's locks and the value vcs.ini holds, and exits without a window or a disc - so nothing here lists a
// setting by hand. A change is written to the same vcs.ini the game reads (next to its exe: build/lcs/vcs.ini,
// build/vcs.ini), the way the game writes it (settings.cpp save(): its line rewritten in place, else appended, every
// other line kept), and applies when the game starts next.
#pragma once

#include "json.h"
#include "process.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace launcher
{
    class GameSettings
    {
    public:
        // the game's exe (its folder holds vcs.ini): asks it again when it is another exe or a newer build than the last
        // answer's, or `again`
        void load(const std::filesystem::path &exe, bool again = false);
        void poll(); // the dump's output, every frame while loading
        bool loading() const { return m_process != nullptr; }
        bool ready() const { return schema.isObject(); }

        Json schema;             // the dump: {game, title, ini, data, settings: [...]}
        std::string error;       // why there is no schema (the exe missing, the dump failed)
        std::filesystem::path exe, ini;

        int value(const std::string &key) const; // vcs.ini's value as the dump read it, with the changes made here
        // writes key=value into vcs.ini; false (and `error` set) when the file can't be written or the value is refused
        bool set(const std::string &key, int value, std::string &error);
        // every listed setting back to its default (one rewrite)
        bool resetAll(const std::vector<std::string> &keys, std::string &error);

    private:
        std::unique_ptr<Process> m_process;
        std::vector<std::string> m_lines;
        std::filesystem::file_time_type m_exeTime{};
        double m_started = 0;
        std::map<std::string, int> m_values;
        bool write(const std::map<std::string, int> &changes, std::string &error);
    };
}
