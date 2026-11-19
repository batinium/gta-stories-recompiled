// [lcs] Liberty City Stories' shared globals (game/game_addrs.h), SLUS-21423 v1.01. 0 = not found yet.
// Found by matching LCS's code against VCS's (own xref scanner: lui/addiu + gp-relative, LCS gp = 0x3D6EF0; function
// alignment against VCS's named map); each value has two independent pieces of evidence. Not yet checked in a run.
#include "game/game_addrs.h"

namespace game
{
    const Addrs kLcsAddrs = [] {
        Addrs a;
        a.timer.update = 0x002B4CF8u;       // the only function with CTimer::Update's two 0x36E38E39 scalings; same body
        a.timer.timeMs = 0x003D9B70u;       // copied to the previous time (0x3D9BC4), then += delta in Update
        a.timer.timeStep = 0x003D9B8Cu;     // Update's clamp / 0.5 test; 5 aligned reads in Process_FollowPed
        a.timer.timeScale = 0x003D9CE4u;    // Update's multiplier; ref counts match VCS's
        a.timer.frameCounter = 0x003D9B6Cu; // ++ at the end of Update; ref mix matches VCS's
        a.timer.userPause = 0x003D9CB4u;    // Update's first pause test; same writer / reader counts as VCS
        a.timer.codePause = 0x003D9C68u;    // Update's second pause test (a third one, 0x3D8BA4, is LCS-only: unknown)
        a.world.playerPed = 0x00408E40u;    // FindPlayerPed 0x107368: PlayerInFocus (0x3D9BC8) * 0x190 + this
        a.world.theCamera = 0x0043BBF0u;    // passed to CCamera::Process 0x28FAC8 (VCS 0x378DD8); 678 loads
        a.world.pad0 = 0x00408C60u;         // CPad::GetPad 0x170BE8: n * 0xBC + this (VCS n * 0xD4)
        a.script.space = 0x003CF0F4u;       // CTheScripts::Init (found through "main.scm"), aligned with VCS's
        a.script.missionRunning = 0x003D9C29u; // set to 1 with the script's +0x20E / +0x219 by the mission launch
                                               // (0x268168), as VCS with +0x20A / +0x217; cleared at mission end
        a.off.pedVehicle = 0x534u;          // FindPlayerVehicle 0x107278: +0x538 ? +0x534 (one field for VCS's
        a.off.pedInVehicle = 0x538u;        // +0x450 and +0x480)
        a.off.padDisableControls = 0x82u;   // 20 pad getters start lh 0x82 then lh 0x7E (VCS: 0x96, 0x92)
        a.off.padMode = 0x7Eu;              // the same getters, and CPad expand-key's lhu 0x7E
        return a;
    }();
}
