#pragma once
// [lcs] Globals several features share, per game (game/game_table.h): the values are in game/vcs_addrs.cpp and
// game/lcs_addrs.cpp. 0 = not known for that game yet. A field goes here once two or more features need it; an address
// only one feature uses stays in that feature's own table.

#include "game/game_table.h"

#include <cstdint>

namespace game
{
    struct Addrs
    {
        struct Timer
        {
            uint32_t timeMs = 0;       // CTimer::m_snTimeInMilliseconds (u32; stands still while paused)
            uint32_t timeStep = 0;     // CTimer::ms_fTimeStep (float, 1.0 = 1/50 s)
            uint32_t timeScale = 0;    // CTimer::ms_fTimeScale (float)
            uint32_t frameCounter = 0; // CTimer::m_FrameCounter (u32, runs in the menu too)
            uint32_t userPause = 0;    // CTimer::m_UserPause (u8: the pause menu)
            uint32_t codePause = 0;    // CTimer::m_CodePause (u8)
            uint32_t update = 0;       // CTimer::Update (function)
        } timer;

        struct World
        {
            uint32_t playerPed = 0;    // CWorld::Players[0].m_pPed (pointer)
            uint32_t theCamera = 0;    // TheCamera (CCamera): right +0, forward +0x10, up +0x20, position +0x30
            uint32_t pad0 = 0;         // CPad 0
        } world;

        struct Script
        {
            uint32_t space = 0;          // -> MAIN.SCM in memory (pointer; 0 until the script engine runs); an IP is an
                                         // offset from it
            uint32_t missionRunning = 0; // u8: a mission runs (set by LOAD_AND_LAUNCH_MISSION_INTERNAL)
        } script;

        // Struct field offsets that differ between the games
        struct Offsets
        {
            uint32_t pedVehicle = 0;      // CPed -> its vehicle (pointer)
            uint32_t pedInVehicle = 0;    // CPed: u32 "in a vehicle" flag gating pedVehicle (0 = the game has none: VCS)
            uint32_t padDisableControls = 0; // CPad: DisablePlayerControls (s16)
            uint32_t padMode = 0;         // CPad: the controller configuration (u16)
        } off;
    };

    extern const Addrs kVcsAddrs;
    extern const Addrs kLcsAddrs;

    // The current game's shared globals
    inline const Addrs &addrs()
    {
        return pick(kVcsAddrs, kLcsAddrs);
    }
}
