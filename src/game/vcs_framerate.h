// VCS frame rate and game timing (docs/NATIVE_PORT_ROADMAP.md milestone 4, docs/game-internals.md 4).
#pragma once

#include "ps2_guest_cc.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace vcs
{
    void installFrameRateHooks(PS2Runtime &runtime);

    // Game timing: Original = paced by the emulated VBlank as on the PS2 (30, or 60 with a cap of 60 or more);
    // Independent = paced by the host clock to the frame-rate cap, game time from the host clock. Set before the game
    // runs (a switch while it runs would jump the game's clock).
    enum class Timing
    {
        Original = 0,
        Independent = 1,
    };
    void setTiming(Timing timing);
    Timing timing();

    // Frames per second: 30, 60 ... 240 (the most, vcs_framerate.cpp kMaxFps); kCapRefresh = the monitor's refresh rate; kCapUnlimited = no limit
    // (independent timing). Any thread, takes effect at the next frame.
    constexpr int kCapRefresh = -1;
    constexpr int kCapUnlimited = 0;
    void setFrameCap(int fps);
    int frameCap();

    // Fast forward (as an emulator's): the game's clock runs `speed` times real time and the frame cap rises with it, so
    // the game steps as usual, only more often; where the device can't keep up each step covers more time. Independent
    // timing only (the game's time comes from the host clock there); 1 = off. Sound plays at its own speed. Any thread.
    void setFastForward(float speed);
    float fastForward();

    // The pre-independence switch: 60 = setFrameCap(60), else 30.
    void setFrameRate60(bool on);
    bool frameRate60();

    // Even pacing (independent timing with a cap): when the game can't hold the cap, pace it at the highest rate it can
    // hold steadily instead - the cap, or a whole fraction of the display's refresh below it (120 -> 80 -> 60 -> 30 on a
    // 240 Hz display) - and come back up when the frames are short again. 0 off, 1 on; VCS_EVEN_PACE overrides. Any thread.
    void setEvenPace(int mode);
    int evenPace();
    double evenPaceFps(); // the rate even pacing holds now (0: the cap itself)

    // [lcs] Liberty City Stories' frame-rate hooks (game/lcs_framerate.cpp) on VCS's: its limiter, VBlank counter and
    // CTimer have VCS's shape.
    namespace framerate_seam
    {
        void setMaxFps(double fps); // the highest cap (VCS 240; at start-up)
        double effectiveFps();      // the cap the limiter paces to (0 = none)
        // VCS's hooks themselves: the VBlank counter (a0 = the graphics object), the limiter's VBlank wait in
        // independent timing ($s2 = the graphics object)
        PS2_GUEST_CC void countVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        PS2_GUEST_CC void limiterWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    }
}
