// [lcs] Which game runs, per-game tables (game/game_table.h).
#include "game/game_table.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace game
{
    namespace
    {
        std::atomic<Id> g_current{Id::None};
    }

    Id current()
    {
        return g_current.load(std::memory_order_relaxed);
    }

    void setCurrent(Id id)
    {
        g_current.store(id, std::memory_order_relaxed);
        std::fprintf(stderr, "[game] %s\n", name(id));
    }

    const char *name(Id id)
    {
        switch (id)
        {
        case Id::Vcs:
            return "VCS";
        case Id::Lcs:
            return "LCS";
        default:
            return "none";
        }
    }

    void logUnavailable(const char *feature)
    {
        static std::mutex mutex;
        static std::set<std::string> logged;
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.insert(feature).second)
            std::fprintf(stderr, "[game] %s not available for %s (address table incomplete)\n", feature,
                         name(current()));
    }

    PS2Runtime::RecompiledFunction resolve(uint32_t address)
    {
        const PS2Runtime::RecompiledFunction fn = address ? ps2OriginalFunction(address) : nullptr;
        if (!fn)
        {
            std::fprintf(stderr, "[game] %s: no recompiled function at 0x%08x\n", name(current()),
                         static_cast<unsigned>(address));
            std::abort();
        }
        return fn;
    }

    void call(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resolve(address)(rdram, ctx, runtime);
    }
}
