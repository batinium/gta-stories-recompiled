// [stories] The game as a DLL for the one-process host (stories.exe, src/stories/host.cpp; docs/STORIES.md "One
// process"). Built only with VCS_GAME_DLL (Windows): main.cpp's main is renamed vcs_game_main there, and this file's
// stories_game_main is the DLL's only export (a dllexport also stops lld's mingw auto-export of every generated
// function).
//
// Two games share one process environment, but each needs its own: its control port, data folder, role in the dual
// world, hidden mode, and the settings' own writes (main.cpp setEnv: PS2X_HW_SCALE, PS2X_GS ...). The DLL is linked with
// -Wl,--wrap=getenv,--wrap=_putenv_s (lld's COFF --wrap also redirects the __imp_ dllimport calls, std::getenv
// included), so every read in this DLL goes through an overlay: the host's NAME=value pairs for this game, then this
// DLL's own writes, then the process environment. A write never reaches the process environment (the other game).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
int vcs_game_main(int argc, char *argv[]); // main.cpp (C++ linkage once renamed)

namespace
{
    struct Overlay
    {
        std::mutex lock;
        // name -> value (null: unset here, the process value hidden too). Old values are kept: a pointer getenv handed
        // out stays valid for the DLL's life, as a CRT environment string would until the next write
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

extern "C" int __wrap__putenv_s(const char *name, const char *value)
{
    if (!name || !*name || !value)
        return 22; // EINVAL
    put(name, *value ? value : nullptr); // _putenv_s(name, "") removes the variable
    return 0;
}

// The two games' log lines go to one stdout / stderr: each line this DLL starts there gets its game's tag ("[LCS] ",
// "[VCS] "), so the one log reads as two. -Wl,--wrap of the UCRT's printf core (__stdio_common_vfprintf: every printf /
// fprintf of the headers), fwrite / fputs / puts / fputc (libc++'s streams, std::cerr / std::cout). Other files: as is.
// STORIES_LOG_TAG=0: no tags.
extern "C" int __cdecl __real___stdio_common_vfprintf(unsigned long long options, FILE *stream, const char *format,
                                                       _locale_t locale, va_list args);
extern "C" size_t __cdecl __real_fwrite(const void *data, size_t size, size_t count, FILE *stream);
extern "C" int __cdecl __real_fputs(const char *text, FILE *stream);
extern "C" int __cdecl __real_puts(const char *text);
extern "C" int __cdecl __real_fputc(int c, FILE *stream);

namespace
{
    std::mutex g_logLock;
    bool g_atLineStart[2] = {true, true}; // stdout, stderr: this DLL's last write there ended a line

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

    // the bytes written with the tag before each line start; false: a write failed
    bool emitTagged(int index, FILE *stream, const char *p, size_t n)
    {
        const char *tag = logTag();
        const size_t tagLen = std::strlen(tag);
        std::lock_guard<std::mutex> g(g_logLock);
        std::string line; // the tag and its line in ONE write: the other game's writes can't come between them
        while (n)
        {
            const char *nl = static_cast<const char *>(std::memchr(p, '\n', n));
            const size_t chunk = nl ? static_cast<size_t>(nl - p) + 1 : n;
            if (g_atLineStart[index] && tagLen)
            {
                line.assign(tag, tagLen);
                line.append(p, chunk);
                if (__real_fwrite(line.data(), 1, line.size(), stream) != line.size())
                    return false;
            }
            else if (__real_fwrite(p, 1, chunk, stream) != chunk)
                return false;
            g_atLineStart[index] = nl != nullptr;
            p += chunk;
            n -= chunk;
        }
        return true;
    }
}

extern "C" int __cdecl __wrap___stdio_common_vfprintf(unsigned long long options, FILE *stream, const char *format,
                                                       _locale_t locale, va_list args)
{
    const int index = stdIndex(stream);
    if (index < 0 || !*logTag())
        return __real___stdio_common_vfprintf(options, stream, format, locale, args);
    char small[2048];
    va_list copy;
    va_copy(copy, args);
    // (2: _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR - the length needed when it does not fit)
    int n = __stdio_common_vsprintf(options | 2ull, small, sizeof(small), format, locale, copy);
    va_end(copy);
    if (n < 0)
        return __real___stdio_common_vfprintf(options, stream, format, locale, args);
    if (static_cast<size_t>(n) < sizeof(small))
        return emitTagged(index, stream, small, static_cast<size_t>(n)) ? n : -1;
    std::vector<char> big(static_cast<size_t>(n) + 1);
    va_copy(copy, args);
    n = __stdio_common_vsprintf(options | 2ull, big.data(), big.size(), format, locale, copy);
    va_end(copy);
    return n >= 0 && emitTagged(index, stream, big.data(), static_cast<size_t>(n)) ? n : -1;
}

extern "C" size_t __cdecl __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    const int index = stdIndex(stream);
    if (index < 0 || !*logTag() || !size)
        return __real_fwrite(data, size, count, stream);
    return emitTagged(index, stream, static_cast<const char *>(data), size * count) ? count : 0;
}

extern "C" int __cdecl __wrap_fputs(const char *text, FILE *stream)
{
    const int index = stdIndex(stream);
    if (index < 0 || !*logTag())
        return __real_fputs(text, stream);
    return emitTagged(index, stream, text, std::strlen(text)) ? 0 : EOF;
}

extern "C" int __cdecl __wrap_puts(const char *text)
{
    if (!*logTag())
        return __real_puts(text);
    return emitTagged(0, stdout, text, std::strlen(text)) && emitTagged(0, stdout, "\n", 1) ? 0 : EOF;
}

extern "C" int __cdecl __wrap_fputc(int c, FILE *stream)
{
    const int index = stdIndex(stream);
    if (index < 0 || !*logTag())
        return __real_fputc(c, stream);
    const char ch = static_cast<char>(c);
    return emitTagged(index, stream, &ch, 1) ? static_cast<unsigned char>(ch) : EOF;
}

// A game leaves through _Exit (main.cpp after the run, crash.cpp). In one process that would still detach the OTHER
// game's DLL under its running threads (ExitProcess runs every DLL's exit table: its static std::threads, joinable,
// std::terminate -> a crash report on every normal quit). The process ends here at once instead (-Wl,--wrap=_Exit).
extern "C" [[noreturn]] void __wrap__Exit(int code)
{
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;)
        Sleep(1000);
}

// env: "NAME=value" strings for this game, a null pointer ends the list ("NAME=" with nothing after: unset here)
extern "C" __declspec(dllexport) int stories_game_main(int argc, char *argv[], const char *const *env)
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
