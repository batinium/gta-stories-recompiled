// [lcs] Vice City Stories' shared globals (game/game_addrs.h), SLUS-21590 v1.00.
#include "game/game_addrs.h"

namespace game
{
    const Addrs kVcsAddrs = [] {
        Addrs a;
        a.timer.timeMs = 0x004CD104u;
        a.timer.timeStep = 0x004CD110u;
        a.timer.timeScale = 0x004CD168u;
        a.timer.frameCounter = 0x004CD124u;
        a.timer.userPause = 0x004CD2ACu;
        a.timer.codePause = 0x004CD11Cu;
        a.timer.update = 0x002F4960u;
        a.world.playerPed = 0x004E4910u;
        a.world.theCamera = 0x006F44D0u;
        a.world.pad0 = 0x005147A8u;
        a.script.space = 0x0048741Cu;
        a.script.missionRunning = 0x004CD3E7u;
        a.off.pedVehicle = 0x450u;
        a.off.padDisableControls = 0x96u;
        a.off.padMode = 0x92u;
        return a;
    }();
}
