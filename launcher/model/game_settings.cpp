// See game_settings.h.
#include "game_settings.h"

#include "system.h"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        double seconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::string trim(std::string s)
        {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                ++i;
            return s.substr(i);
        }

        // the game's own check (src/settings/settings.cpp valid())
        bool valid(const Json &s, int v)
        {
            const std::string type = s["type"].str();
            if (type == "toggle")
                return v == 0 || v == 1;
            if (type == "integer")
                return v >= int(s["min"].num()) && v <= int(s["max"].num());
            if (type == "choice")
            {
                for (size_t i = 0; i < s["choices"].size(); ++i)
                    if (int(s["choices"][i]["value"].num()) == v)
                        return true;
                return false;
            }
            return false; // keys and actions are the game's (F2)
        }
    }

    void GameSettings::load(const fs::path &gameExe, bool again)
    {
        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(gameExe, ec);
        if (!again && gameExe == exe && (loading() || (ready() && time == m_exeTime) || (!ready() && !error.empty() && time == m_exeTime)))
            return;
        m_process.reset();
        m_lines.clear();
        schema = Json();
        m_values.clear();
        error.clear();
        exe = gameExe;
        ini = gameExe.parent_path() / "vcs.ini";
        m_exeTime = time;
        if (!fs::is_regular_file(gameExe, ec))
        {
            error = "missing";
            return;
        }
        Process::Options options;
        options.cwd = gameExe.parent_path();
        m_process = std::make_unique<Process>();
        std::string why;
        if (!m_process->start({pathText(gameExe), "--dump-settings-json"}, options, why))
        {
            m_process.reset();
            error = "Could not start " + pathText(gameExe.filename()) + ": " + why;
            return;
        }
        m_started = seconds();
    }

    void GameSettings::poll()
    {
        if (!m_process)
            return;
        for (std::string &line : m_process->takeLines())
            m_lines.push_back(std::move(line));
        if (m_process->running())
        {
            if (seconds() - m_started > 20) // it never needs that long: something is wrong with the build
            {
                m_process->kill();
                m_process.reset();
                error = pathText(exe.filename()) + " gave no answer about its settings";
            }
            return;
        }
        for (std::string &line : m_process->takeLines())
            m_lines.push_back(std::move(line));
        const int code = m_process->exitCode();
        m_process.reset();
        // the JSON starts at the line with the opening brace (anything the game printed before it is left out)
        std::string text;
        bool in = false;
        for (const std::string &line : m_lines)
        {
            if (!in && !line.empty() && line[0] == '{')
                in = true;
            if (in)
                text += line + "\n";
        }
        m_lines.clear();
        bool ok = false;
        Json parsed = Json::parse(text, &ok);
        if (!ok || !parsed["settings"].size())
        {
            error = code != 0 ? pathText(exe.filename()) + " ended with code " + std::to_string(code) +
                                    " when asked for its settings: build the game again (this build may be older than "
                                    "the launcher)"
                              : pathText(exe.filename()) + " doesn't list its settings: build the game again";
            return;
        }
        schema = std::move(parsed);
        if (!schema["ini"].str().empty())
            ini = pathFrom(schema["ini"].str());
        const Json &list = schema["settings"];
        for (size_t i = 0; i < list.size(); ++i)
            m_values[list[i]["key"].str()] = int(list[i]["value"].num());
    }

    int GameSettings::value(const std::string &key) const
    {
        const auto it = m_values.find(key);
        return it != m_values.end() ? it->second : 0;
    }

    bool GameSettings::write(const std::map<std::string, int> &changes, std::string &why)
    {
        std::vector<std::string> lines;
        {
            std::ifstream in(ini, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
                lines.push_back(line);
        }
        std::map<std::string, bool> written;
        for (std::string &line : lines)
        {
            const std::string t = trim(line);
            const size_t eq = t.find('=');
            if (t.empty() || t[0] == '#' || t[0] == ';' || eq == std::string::npos)
                continue;
            const std::string key = trim(t.substr(0, eq));
            if (const auto it = changes.find(key); it != changes.end())
            {
                line = key + "=" + std::to_string(it->second);
                written[key] = true;
            }
        }
        for (const auto &[key, v] : changes)
            if (!written[key])
                lines.push_back(key + "=" + std::to_string(v));
        std::error_code ec;
        fs::create_directories(ini.parent_path(), ec);
        fs::path tmp = ini;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            for (const std::string &line : lines)
                out << (line.empty() || line.back() != '\r' ? line : line.substr(0, line.size() - 1)) << "\n";
            if (!out)
            {
                why = "Could not write " + pathText(ini);
                return false;
            }
        }
        fs::rename(tmp, ini, ec);
        if (ec)
        {
            fs::remove(tmp, ec);
            why = "Could not write " + pathText(ini) + " - is the folder read-only?";
            return false;
        }
        for (const auto &[key, v] : changes)
            m_values[key] = v;
        return true;
    }

    bool GameSettings::set(const std::string &key, int v, std::string &why)
    {
        const Json &list = schema["settings"];
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i]["key"].str() == key)
            {
                if (!valid(list[i], v))
                {
                    why = "The game doesn't take " + std::to_string(v) + " for " + list[i]["label"].str();
                    return false;
                }
                if (value(key) == v)
                    return true;
                return write({{key, v}}, why);
            }
        why = "The game has no setting " + key;
        return false;
    }

    bool GameSettings::resetAll(const std::vector<std::string> &keys, std::string &why)
    {
        std::map<std::string, int> changes;
        const Json &list = schema["settings"];
        for (size_t i = 0; i < list.size(); ++i)
        {
            const std::string key = list[i]["key"].str();
            if (std::find(keys.begin(), keys.end(), key) != keys.end() && value(key) != int(list[i]["default"].num()))
                changes[key] = int(list[i]["default"].num());
        }
        return changes.empty() || write(changes, why);
    }
}
