// [stories] The game as a library for the one-process host on Android (src/android/stories_host.cpp; docs/ANDROID.md
// "Both cities on Android"): libstories_<game>.so, built with VCS_ANDROID_STORIES_GUEST. The ELF twin of game_dll.cpp.
//
// The library holds a whole runtime of its own (two games, two runtimes, as two DLLs on the PC). Everything in it is
// local (game_so.map: stories_game_main is the only export), so none of its names meets the host's libmain.so, which is
// in the global group (-z global) and would otherwise capture them. SDL and the JNI glue (touch pad, stats, gamepads,
// power) are NOT in it: those imports resolve to libmain.so's, the copies Java talks to.
//
// Each game its own environment (control port, data folder, role in the dual world, the settings' own writes): the
// library is linked with -Wl,--wrap=getenv,--wrap=setenv,--wrap=unsetenv, so every read in it goes through an overlay
// (the host's NAME=value pairs for this game, then this library's own writes, then the process environment) and a write
// never reaches the process (the other game). Its stderr / stdout lines get the game's tag ("[LCS] ", "[VCS] ") through
// --wrap=fprintf,vfprintf,fputs (the host already pipes both into logcat).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" char *__real_getenv(const char *name);
extern "C" int __real_vfprintf(FILE *stream, const char *format, va_list args);
extern "C" int __real_fputs(const char *text, FILE *stream);
int vcs_game_main(int argc, char *argv[]); // main.cpp (renamed)

namespace
{
    struct Overlay
    {
        std::mutex lock;
        // name -> value (null: unset here, the process value hidden too). Old values are kept: a pointer getenv handed
        // out stays valid for the library's life
        std::map<std::string, std::unique_ptr<std::string>> values;
        std::vector<std::unique_ptr<std::string>> retired;
    };
    Overlay &overlay()
    {
        static Overlay *o = new Overlay(); // never destroyed: getenv may be called during the process's exit
        return *o;
    }

    void put(const char *name, const char *value)
    {
        Overlay &o = overlay();
        std::lock_guard<std::mutex> g(o.lock);
        auto &slot = o.values[name];
        if (slot)
            o.retired.push_back(std::move(slot));
        slot = value ? std::make_unique<std::string>(value) : nullptr;
    }
}

extern "C" char *__wrap_getenv(const char *name)
{
    if (!name)
        return nullptr;
    Overlay &o = overlay();
    {
        std::lock_guard<std::mutex> g(o.lock);
        const auto it = o.values.find(name);
        if (it != o.values.end())
            return it->second ? it->second->data() : nullptr;
    }
    return __real_getenv(name);
}

extern "C" int __wrap_setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || std::strchr(name, '='))
        return -1;
    if (!overwrite && __wrap_getenv(name))
        return 0;
    put(name, value ? value : "");
    return 0;
}

extern "C" int __wrap_unsetenv(const char *name)
{
    if (!name || !*name || std::strchr(name, '='))
        return -1;
    put(name, nullptr);
    return 0;
}

namespace
{
    std::mutex g_logLock;
    bool g_atLineStart[2] = {true, true}; // stdout, stderr: this library's last write there ended a line

    int stdIndex(FILE *stream) { return stream == stdout ? 0 : stream == stderr ? 1 : -1; }

    const char *logTag()
    {
        static const char *tag = [] {
            const char *off = __wrap_getenv("STORIES_LOG_TAG");
            if (off && off[0] == '0')
                return "";
            const char *g = __wrap_getenv("STORIES_GAME");
            return g && g[0] == 'l' ? "[LCS] " : g && g[0] == 'v' ? "[VCS] " : "";
        }();
        return tag;
    }

    // the text with the tag before each line start, in one write per line (the other game's lines can't come between)
    int emitTagged(int index, FILE *stream, const char *p, size_t n)
    {
        const char *tag = logTag();
        const size_t tagLen = std::strlen(tag);
        std::lock_guard<std::mutex> g(g_logLock);
        std::string line;
        const size_t total = n;
        while (n)
        {
            const char *nl = static_cast<const char *>(std::memchr(p, '\n', n));
            const size_t chunk = nl ? static_cast<size_t>(nl - p) + 1 : n;
            line.clear();
            if (g_atLineStart[index] && tagLen)
                line.assign(tag, tagLen);
            line.append(p, chunk);
            if (std::fwrite(line.data(), 1, line.size(), stream) != line.size())
                return -1;
            g_atLineStart[index] = nl != nullptr;
            p += chunk;
            n -= chunk;
        }
        return static_cast<int>(total);
    }
}

extern "C" int __wrap_vfprintf(FILE *stream, const char *format, va_list args)
{
    const int index = stdIndex(stream);
    if (index < 0 || !*logTag())
        return __real_vfprintf(stream, format, args);
    char small[2048];
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(small, sizeof(small), format, copy);
    va_end(copy);
    if (n < 0)
        return __real_vfprintf(stream, format, args);
    if (static_cast<size_t>(n) < sizeof(small))
        return emitTagged(index, stream, small, static_cast<size_t>(n));
    std::vector<char> big(static_cast<size_t>(n) + 1);
    va_copy(copy, args);
    n = std::vsnprintf(big.data(), big.size(), format, copy);
    va_end(copy);
    return n >= 0 ? emitTagged(index, stream, big.data(), static_cast<size_t>(n)) : -1;
}

extern "C" int __wrap_fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = __wrap_vfprintf(stream, format, args);
    va_end(args);
    return n;
}

extern "C" int __wrap_fputs(const char *text, FILE *stream)
{
    const int index = stdIndex(stream);
    if (index < 0 || !*logTag())
        return __real_fputs(text, stream);
    return emitTagged(index, stream, text, std::strlen(text)) >= 0 ? 0 : EOF;
}

// env: "NAME=value" strings for this game, a null pointer ends the list ("NAME=" with nothing after: unset here).
// Called by the host on the thread the game is to run on; returns when the game ends (main.cpp usually _Exits first).
extern "C" __attribute__((visibility("default"))) int stories_game_main(int argc, char *argv[], const char *const *env)
{
    for (const char *const *e = env; e && *e; ++e)
    {
        const char *eq = std::strchr(*e, '=');
        if (!eq || eq == *e)
            continue;
        put(std::string(*e, eq).c_str(), eq[1] ? eq + 1 : nullptr);
    }
    return vcs_game_main(argc, argv);
}
