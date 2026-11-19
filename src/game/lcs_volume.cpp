// [lcs] The Audio settings' Music / Sound effects volume for Liberty City Stories, kept in step with its pause menu
// (game/vcs_volume.cpp does it for VCS with VCS's addresses). LCS keeps each level, 0..127, in cSampleManager (+4
// effects, +5 music: the bytes its sound code uses, as VCS's) and in two prefs globals the pause menu draws as bars and
// saves carry.
//
// Both ways, as VCS: a changed setting goes into both places at the next game frame, and every other call of the game's
// setters (a save load, the defaults) gets the setting's level; a step on the pause menu's Audio page (the setters'
// calls from it, left to the game) is taken into the setting and vcs.ini at the next host frame
// (lcs::volumeHostFrame, from main's UI callback). A change that comes through neither (cSampleManager::Initialise
// putting 100 back when the sound starts) gets the setting's level again at the next frame. A
// setting that shows the game's level in whole percent keeps that level, and the Sound effects setting's default in an
// LCS run is LCS's own 96 (76 %; VCS's 112 = 88 % stays VCS's): a player who never changed it hears LCS's default.
// VCS_GAME_VOLUME=0 leaves the levels alone.
//
// Game side on the EE thread: lcs::volumeFrame from the native sound driver's per-frame voice block (game/lcs_sound.cpp).
#include "game/lcs_audio.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lcs
{
    namespace
    {
        struct Addrs
        {
            uint32_t sampleManager; // cSampleManager: +4 effects, +5 music (u8)
            uint32_t prefsEffects;  // the menu's prefs (u32 globals)
            uint32_t prefsMusic;
            uint32_t setEffects;    // the menu-side setters (a1 = level; a negative byte becomes 0x7F)
            uint32_t setMusic;
            uint32_t menuEffects;   // their calls from the pause menu's Audio page (return addresses)
            uint32_t menuMusic;
        };
        // Evidence: the only one-store setters "jr ra; sb a1, 4(a0)" / "sb a1, 5(a0)" next to each other are 0x114D50 /
        // 0x114D58 (VCS's cSampleManager setters 0x10E2B8 / 0x10E2C0 have the same shape); their only callers (DMAudio
        // 0x238558 / 0x238580) pass a0 = 0x3DF580. Those are called only from 0x2A1EC8 / 0x2A1F08, which clamp a1 the way
        // VCS's menu setters 0x2FA538 / 0x2FA5B0 do. Their callers load a1 from gp+0x1C88 / gp+0x1C8C (gp 0x3D6EF0) - the
        // globals the Audio page steps by 16 between 0 and 127 before calling them at 0x343078 / 0x343008, and the menu's
        // "restore defaults" sets to 0x60 / 0x50 (0x342010; also their initial values in .data).
        constexpr Addrs kLcs{0x003DF580u, 0x003D8B78u, 0x003D8B7Cu, 0x002A1EC8u, 0x002A1F08u, 0x00343080u, 0x00343010u};
        constexpr int kLcsEffectsDefault = 0x60; // LCS's own default (VCS's is 0x70; music 0x50 in both)

        struct Level
        {
            const char *key;
            const char *name;
            uint32_t byte;  // offset in cSampleManager
            uint32_t prefs; // the prefs global
            uint32_t setter;
            uint32_t menuReturn;
            PS2Runtime::RecompiledFunction original = nullptr;
            int percent = -1;                // the setting the game's level stands for (EE thread; -1: none yet)
            int seen = -1;                   // the game's level at the last frame (EE thread)
            std::atomic<int> adopted{-1};    // a pause-menu step, for the host thread to take into the setting
            std::atomic<int> seenShared{-1}; // `seen` for the control channel
        };
        std::array<Level, 2> g_levels{{
            {"audio.music_volume", "music", 5, kLcs.prefsMusic, kLcs.setMusic, kLcs.menuMusic},
            {"audio.sfx_volume", "effects", 4, kLcs.prefsEffects, kLcs.setEffects, kLcs.menuEffects},
        }};
        bool g_enabled = false;

        int toGame(int percent) { return (percent * 127 + 50) / 100; }
        int toPercent(int level) { return (level * 100 + 63) / 127; }
        // The level for a setting: `current` when the setting shows it (a pause-menu step, 16 apart, and LCS's default
        // aren't whole percents), else the setting's.
        int levelFor(int percent, int current)
        {
            return current >= 0 && toPercent(current) == percent ? current : toGame(percent);
        }

        void put(uint8_t *rdram, Level &l, int level)
        {
            rdram[kLcs.sampleManager + l.byte] = static_cast<uint8_t>(level);
            const uint32_t v = static_cast<uint32_t>(level);
            std::memcpy(rdram + l.prefs, &v, 4);
            l.seen = level;
            l.seenShared.store(level);
        }

        template <int I>
        PS2_GUEST_CC void setter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Level &l = g_levels[I];
            if (GPR_U32(ctx, 31) == l.menuReturn) // a step on the pause menu's Audio page: the game's, into the setting
            {
                const int asked = static_cast<int>(GPR_U32(ctx, 5) & 0xFF);
                const int level = asked > 0x7F ? 0x7F : asked; // as the setter clamps it
                l.seen = level;
                l.seenShared.store(level);
                l.percent = toPercent(level);
                l.adopted.store(level);
            }
            else
            {
                const int asked = static_cast<int>(GPR_U32(ctx, 5) & 0xFF);
                const int percent = settings::value(l.key);
                const int level = levelFor(percent, asked > 0x7F ? 0x7F : asked);
                if (asked != level)
                    std::fprintf(stderr, "[volume] LCS sets the %s level %d (ra=0x%x): the player's %d instead\n", l.name,
                                 asked, GPR_U32(ctx, 31), level);
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(level));
                put(rdram, l, level);
                l.percent = percent;
            }
            l.original(rdram, ctx, runtime);
        }
    }

    void volumeDefaults()
    {
        // before settings::load: without the key in vcs.ini (shared by both games' runs) LCS gets its own level
        settings::setDefaultForRun("audio.sfx_volume", toPercent(kLcsEffectsDefault));
    }

    void installVolume(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_GAME_VOLUME");
        g_enabled = !(env && env[0] == '0');
        if (!g_enabled)
        {
            std::fprintf(stderr, "[volume] LCS's music / effects levels left alone (VCS_GAME_VOLUME=0)\n");
            return;
        }
        auto hook = [&](Level &l, PS2Runtime::RecompiledFunction fn) {
            l.original = runtime.lookupFunction(l.setter);
            if (l.original)
                runtime.replaceFunction(l.setter, fn);
            else
                std::fprintf(stderr, "[volume] LCS's %s setter 0x%x not found\n", l.key, l.setter);
        };
        hook(g_levels[0], &setter<0>);
        hook(g_levels[1], &setter<1>);
        // soundstate -> ok music=<0..127> effects=<0..127>: the game's levels at the last frame (as VCS's command)
        ps2control::addCommand("soundstate", [](const std::string &, ps2control::Reply reply) {
            char text[64];
            std::snprintf(text, sizeof(text), "ok music=%d effects=%d", g_levels[0].seenShared.load(),
                          g_levels[1].seenShared.load());
            reply(text);
        });
    }

    void volumeFrame(uint8_t *rdram)
    {
        if (!g_enabled)
            return;
        for (Level &l : g_levels)
        {
            const int now = rdram[kLcs.sampleManager + l.byte];
            if (l.adopted.load() >= 0) // a pause-menu step on its way into the setting
                continue;
            const int percent = settings::value(l.key);
            if (percent != l.percent) // the setting changed (or the first frame)
            {
                const int level = levelFor(percent, now);
                if (level != now)
                    put(rdram, l, level);
                l.seen = level;
                l.seenShared.store(level);
                l.percent = percent;
            }
            else if (now != l.seen)
            {
                // Not through a setter: cSampleManager::Initialise (0x1142E8, 100 / 100) runs again when the sound
                // starts, after the first frames. The setting's level again.
                std::fprintf(stderr, "[volume] LCS's %s level became %d by itself: the player's %d again\n", l.name, now,
                             l.seen);
                put(rdram, l, l.seen);
            }
        }
    }

    void volumeHostFrame()
    {
        for (Level &l : g_levels)
        {
            int level = l.adopted.load();
            if (level < 0)
                continue;
            if (settings::set(l.key, toPercent(level)))
                std::fprintf(stderr, "[volume] LCS's pause menu set the %s level %d: setting %d%%\n", l.name, level,
                             toPercent(level));
            l.adopted.compare_exchange_strong(level, -1); // after the setting: the next game frame sees it
        }
    }
}
