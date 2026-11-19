// See player_control.h.
#include "game/player_control.h"

#include "game/game_addrs.h"

#include <cstring>

namespace player
{
    using game::addrs;
    using game::Addrs;
    using game::isLcs;
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        uint16_t rd16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }

        // The active CCam's mode (u16), per game:
        // - VCS: TheCamera + 0x70 + ActiveCam x 0x270, ActiveCam = byte TheCamera + 0x50, mode at the CCam's +0
        //   (game-internals.md "The camera update": CCamera::Process FUN_00378dd8 indexes it so; 4 follow ped on foot,
        //   18 behind a vehicle, 15 / 17 during a cutscene).
        // - LCS: TheCamera + 0x220 + ActiveCam x 0x2B0, ActiveCam = byte TheCamera + 0x10C, mode at the CCam's +0x40
        //   (tools/lcs_test.py cam_mode, used by its newgame / mission flows: 4 on foot, ~17 in the opening cutscene).
        // 0xFFFF when ActiveCam is out of range (0..2) or the camera is not known.
        uint16_t cameraMode(const uint8_t *ram)
        {
            const uint32_t cam = addrs().world.theCamera;
            if (!cam)
                return 0xFFFF;
            const bool lcs = isLcs();
            const uint32_t active = ram[(cam + (lcs ? 0x10Cu : 0x50u)) & kRamMask];
            if (active > 2)
                return 0xFFFF;
            return lcs ? rd16(ram, cam + 0x220u + active * 0x2B0u + 0x40u) : rd16(ram, cam + 0x70u + active * 0x270u);
        }
    }

    bool hasControl(const uint8_t *ram)
    {
        const Addrs &a = addrs();
        if (!ram || !a.script.space || !a.world.playerPed || !a.world.pad0 || !a.off.padDisableControls)
            return false;
        if (rd32(ram, a.script.space) == 0)
            return false; // the menus before a game starts
        const uint32_t ped = rd32(ram, a.world.playerPed);
        if (ped == 0)
            return false;
        if (rd16(ram, a.world.pad0 + a.off.padDisableControls) != 0)
            return false; // a script holds the controls (cutscenes, scripted scenes)
        if (a.timer.userPause && ram[a.timer.userPause & kRamMask] != 0)
            return false; // the pause menu
        // in a vehicle: VCS has only the pointer (+0x450), LCS a flag (+0x538) gating its pointer (+0x534)
        const bool inVehicle = a.off.pedVehicle && rd32(ram, ped + a.off.pedVehicle) != 0 &&
                               (!a.off.pedInVehicle || rd32(ram, ped + a.off.pedInVehicle) != 0);
        const uint16_t mode = cameraMode(ram);
        if (mode == 4)
            return true; // following the player on foot
        return inVehicle && mode != 0xFFFF && (mode < 14 || mode > 17);
    }
}
