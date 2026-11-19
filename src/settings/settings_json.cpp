// settings::writeJson (settings.h): the registry as JSON, for the PC launcher's Game settings sheet
// (vcs.exe / lcs.exe --dump-settings-json; launcher/model/game_settings.cpp). One source: every key, label, help line,
// range and choice the F2 menu shows, with this run's locks, notes and the values vcs.ini holds.
#include "settings/settings.h"

#include <cstdio>
#include <ostream>

namespace settings
{
    namespace
    {
        std::string quoted(const std::string &s)
        {
            std::string out = "\"";
            for (const unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                        out += esc;
                    }
                    else
                        out += static_cast<char>(c); // UTF-8 passes through
                }
            }
            return out + "\"";
        }

        const char *typeName(Type t)
        {
            switch (t)
            {
            case Type::Toggle: return "toggle";
            case Type::Choice: return "choice";
            case Type::Integer: return "integer";
            case Type::Key: return "key";
            case Type::Action: return "action";
            }
            return "toggle";
        }
    }

    void writeJson(std::ostream &out, const std::vector<std::pair<std::string, std::string>> &header)
    {
        out << "{";
        for (const auto &[name, text] : header)
            out << quoted(name) << ": " << quoted(text) << ", ";
        out << "\"settings\": [";
        bool first = true;
        const std::vector<std::string> otherGame = lockedForRun(); // [lcs] does nothing in this game (lockForRun)
        for (const Spec &s : all())
        {
            out << (first ? "\n" : ",\n");
            first = false;
            const bool locked = s.locked && s.locked();
            bool other = false;
            for (const std::string &k : otherGame)
                other = other || k == s.key;
            out << "{\"otherGame\": " << (other ? "true" : "false") << ", ";
            out << "\"key\": " << quoted(s.key) << ", \"page\": " << quoted(s.page) << ", \"section\": " << quoted(s.section)
                << ", \"label\": " << quoted(s.label) << ", \"help\": " << quoted(s.help) << ", \"type\": \"" << typeName(s.type)
                << "\", \"default\": " << s.defaultValue << ", \"value\": " << value(s.key) << ", \"min\": " << s.min
                << ", \"max\": " << s.max << ", \"format\": " << quoted(s.format) << ", \"apply\": \""
                << (s.apply == Apply::Live ? "live" : "restart") << "\", \"env\": " << quoted(s.env)
                << ", \"overridden\": " << (overridden(s.key) ? "true" : "false") << ", \"hidden\": " << (s.hidden ? "true" : "false")
                << ", \"locked\": " << (locked ? "true" : "false") << ", \"lockedValue\": " << s.lockedValue
                << ", \"lockedNote\": " << quoted(s.lockedNote) << ", \"button\": " << quoted(s.button) << ", \"choices\": [";
            for (size_t i = 0; i < s.choices.size(); ++i)
                out << (i ? ", " : "") << "{\"label\": " << quoted(s.choices[i].label) << ", \"value\": " << s.choices[i].value << "}";
            out << "]}";
        }
        out << "\n]}\n";
    }
}
