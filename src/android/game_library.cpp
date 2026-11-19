// [android] Loading the player's game library into the runtime (game_library.h; docs/ANDROID.md "Runtime / game split")
#include "android/game_library.h"
#include "android/game_abi.h"

#include "ps2_runtime.h"

#include <dlfcn.h>
#include <elf.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define VCS_STR2(x) #x
#define VCS_STR(x) VCS_STR2(x)

namespace vcs_android
{
    namespace
    {
        struct FileCloser
        {
            void operator()(std::FILE *f) const { std::fclose(f); }
        };

        bool readAt(std::FILE *f, uint64_t offset, void *out, size_t size)
        {
            return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, f) == size;
        }

        std::string g_gameId = "vcs";
    }

    std::string stampGame(const std::string &stamp)
    {
        const size_t at = stamp.find(" " VCS_GAME_STAMP_GAME);
        if (at == std::string::npos)
            return "vcs";
        const size_t start = at + 1 + std::strlen(VCS_GAME_STAMP_GAME);
        const size_t end = stamp.find(' ', start);
        const std::string id = stamp.substr(start, end == std::string::npos ? std::string::npos : end - start);
        return id.empty() ? "vcs" : id;
    }

    const char *loadedGameId()
    {
        return g_gameId.c_str();
    }

    std::string readGameLibraryStamp(const char *path)
    {
        std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path, "rb"));
        if (!f)
            return {};
        Elf64_Ehdr eh{};
        if (!readAt(f.get(), 0, &eh, sizeof(eh)) || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
            eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0 ||
            eh.e_shstrndx >= eh.e_shnum || eh.e_shnum > 4096)
            return {};
        std::vector<Elf64_Shdr> sections(eh.e_shnum);
        if (!readAt(f.get(), eh.e_shoff, sections.data(), sections.size() * sizeof(Elf64_Shdr)))
            return {};
        const Elf64_Shdr &names = sections[eh.e_shstrndx];
        if (names.sh_size == 0 || names.sh_size > (1u << 20))
            return {};
        std::vector<char> nameText(names.sh_size + 1, '\0');
        if (!readAt(f.get(), names.sh_offset, nameText.data(), names.sh_size))
            return {};
        for (const Elf64_Shdr &s : sections)
        {
            if (s.sh_name >= names.sh_size || std::strcmp(&nameText[s.sh_name], VCS_GAME_STAMP_SECTION) != 0)
                continue;
            if (s.sh_type == SHT_NOBITS || s.sh_size == 0 || s.sh_size > 4096)
                return {};
            std::vector<char> text(s.sh_size + 1, '\0');
            if (!readAt(f.get(), s.sh_offset, text.data(), s.sh_size))
                return {};
            return std::string(text.data());
        }
        return {};
    }

    bool loadGameLibrary(const char *path, std::string &error)
    {
        const std::string stamp = readGameLibraryStamp(path);
        std::fprintf(stderr, "[game-library] %s: %s\n", path, stamp.empty() ? "(no stamp)" : stamp.c_str());
        const size_t prefix = std::strlen(VCS_GAME_STAMP_PREFIX);
        if (stamp.compare(0, prefix, VCS_GAME_STAMP_PREFIX) != 0)
        {
            error = "This is not a game library for this app (no interface stamp). Build it with "
                    "tools/android.py build-game and import it again.";
            return false;
        }
        const int abi = std::atoi(stamp.c_str() + prefix);
        if (abi != VCS_GAME_ABI)
        {
            error = "Game library built for another version of the app (game interface " + std::to_string(abi) +
                    ", this app has " VCS_STR(VCS_GAME_ABI) "): rebuild it with tools/android.py build-game from the "
                    "source of this version and import it again.";
            return false;
        }
#ifdef VCS_GAME_HEADERS_HASH
        const size_t hashAt = stamp.find(" headers=");
        const std::string hash = hashAt == std::string::npos ? std::string() : stamp.substr(hashAt + 9, 16);
        if (hash != VCS_GAME_HEADERS_HASH)
            std::fprintf(stderr, "[game-library] note: built against other runtime headers (%s, this app %s) with the same "
                                 "interface number %d: fine unless the interface changed without a VCS_GAME_ABI bump "
                                 "(src/android/game_abi.h)\n", hash.c_str(), VCS_GAME_HEADERS_HASH, VCS_GAME_ABI);
#endif
        // [lcs] the game the library is from (" game=<id>", game_abi.h); none: a library from before LCS, so VCS
        g_gameId = stampGame(stamp);
        std::fprintf(stderr, "[game-library] game: %s\n", g_gameId.c_str());
        // RTLD_GLOBAL: anything loaded later sees its symbols; its own references to the runtime resolve to this
        // library (its DT_NEEDED libmain.so, already loaded)
        void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!handle)
        {
            const char *why = dlerror();
            error = std::string("The game library doesn't load: ") + (why ? why : "unknown error");
            return false;
        }
        auto *table = static_cast<PS2Runtime::RecompiledFunction *>(dlsym(handle, "g_ps2RecompiledFunctionTable"));
        const auto *base = static_cast<const uint32_t *>(dlsym(handle, "g_ps2RecompiledFunctionTableBase"));
        const auto *end = static_cast<const uint32_t *>(dlsym(handle, "g_ps2RecompiledFunctionTableEnd"));
        const auto *slots = static_cast<const uint32_t *>(dlsym(handle, "g_ps2RecompiledFunctionTableSlotCount"));
        if (!table || !base || !end || !slots)
        {
            error = "The game library has no function table: rebuild it with tools/android.py build-game.";
            return false;
        }
        ps2xAttachGameTable(table, *base, *end, *slots);
        std::fprintf(stderr, "[game-library] loaded: table 0x%x-0x%x, %u slots\n", *base, *end, *slots);
        return true;
    }
}
