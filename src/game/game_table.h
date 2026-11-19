#pragma once
// [lcs] Which game runs, and per-game address tables (internal notes: LCS bring-up; docs/ANDROID.md "Games").
//
// One binary may run either game (Vice City Stories or Liberty City Stories, the latter in bring-up): the game is known
// at run time, when the runtime matches the ELF and calls that game's override (vcs_overrides.cpp / lcs_overrides.cpp),
// which calls game::setCurrent first.
//
// Upstream's VCS feature files (vcs_*.cpp) keep their constexpr addresses untouched: they change often upstream and an
// in-place migration would conflict on every merge. LCS features live in new lcs_*.cpp files that read their
// addresses from tables and reuse upstream's game-independent logic (extracted into callable functions with small
// [lcs] edits where needed):
//
//  - globals several features share (CTimer, the player, TheCamera, CPad 0, the script space): game/game_addrs.h,
//    one table per game in game/vcs_addrs.cpp / game/lcs_addrs.cpp;
//  - a feature's own addresses: a small struct in its own file, one constant per game, chosen with game::pick:
//
//        struct Addrs { uint32_t hookA; uint32_t globalB; };
//        constexpr Addrs kVcs{0x00220C68u, 0x004CD3E7u};
//        constexpr Addrs kLcs{};                    // 0 = not found yet for that game
//        const Addrs &A = game::pick(kVcs, kLcs);   // after setCurrent (in install, not at static init)
//
// 0 means "not known for this game": a feature installs only when every address it needs is known
// (game::all(...)), otherwise it logs once and stays off for that game.
//
// A hook calls a game function by an address from a table with game::call (or a game::Fn resolved once at install for
// hot paths): VCS_GAME_FN / ps2GameFunction<address> (game/game_functions.h) still work but only for a constant address.

#include "ps2_runtime.h"

#include <cstdint>
#include <initializer_list>

namespace game
{
    enum class Id : uint8_t
    {
        None, // before any game's override ran (or an unknown ELF)
        Vcs,  // GTA: Vice City Stories, SLUS-21590
        Lcs,  // GTA: Liberty City Stories, SLUS-21423 (bring-up)
    };

    Id current();
    void setCurrent(Id id);
    const char *name(Id id);
    inline bool isVcs() { return current() == Id::Vcs; }
    inline bool isLcs() { return current() == Id::Lcs; }

    // The current game's entry of a per-game table (VCS when no game was set: VCS's own hooks and tests ran that way
    // before the table existed).
    template <class T>
    const T &pick(const T &vcs, const T &lcs)
    {
        return current() == Id::Lcs ? lcs : vcs;
    }

    // Every address given is known (non-zero) for the current game.
    inline bool all(std::initializer_list<uint32_t> addresses)
    {
        for (const uint32_t a : addresses)
            if (a == 0u)
                return false;
        return true;
    }

    // A feature that stays off for the current game (some address unknown): one log line, "[game] <feature> not
    // available for LCS (address table incomplete)".
    void logUnavailable(const char *feature);

    // The generated function at a guest address (the original after a replaceFunction), or abort with a message when
    // there is none: resolve once (install time) for hot paths.
    PS2Runtime::RecompiledFunction resolve(uint32_t address);

    // Calls the game's function at a run-time address (looked up on every call: fine for rare calls; hot paths keep a
    // game::Fn).
    void call(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // A game function resolved on first use and cached.
    class Fn
    {
    public:
        void bind(uint32_t address)
        {
            m_address = address;
            m_fn = nullptr;
        }
        uint32_t address() const { return m_address; }
        explicit operator bool() const { return m_address != 0u; }
        void operator()(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!m_fn)
                m_fn = resolve(m_address);
            m_fn(rdram, ctx, runtime);
        }

    private:
        uint32_t m_address = 0u;
        PS2Runtime::RecompiledFunction m_fn = nullptr;
    };
}
