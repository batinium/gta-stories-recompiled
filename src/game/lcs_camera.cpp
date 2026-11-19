// [lcs] See lcs_camera.h. The turning is vcs_input.cpp's (its mouse camera, game-internals.md 12b / 12c); LCS's camera
// layout was found by aligning LCS's camera code with VCS's (internal notes: LCS address report, LCS features notes):
// VCS Process_FollowPed 0x29DAB0 = LCS 0x281E90, VCS's scope / first-person update 0x297CB8 = LCS 0x27D440,
// CCam::Process VCS 0x294540 = LCS 0x27A380, CCamera::Process VCS 0x378DD8 = LCS 0x28FAC8.
#include "game/lcs_camera.h"

#include "game/game_addrs.h"
#include "game/lcs_carcam.h"
#include "game/lcs_driveby.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lcs::camera
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            // TheCamera (CCamera), offsets
            uint32_t activeCam;     // u8: which of Cams[3] is active
            uint32_t cams;          // Cams[0]
            uint32_t camSize;       // sizeof(CCam)
            uint32_t looksAtPlayer; // u32: 1 while no script camera has it
            // CCam, offsets
            uint32_t mode;          // u16
            uint32_t alpha;         // float: pitch (up +)
            uint32_t beta;          // float: heading, 0..2 pi
            uint32_t fov;           // float, degrees
            uint32_t target;        // vec3: the point it looks at (the source points are placed around it)
            uint32_t source[2];     // vec3: where it stands (the heading / pitch are measured from these next update)
            uint32_t targetEntity;  // the entity it follows (the player's ped on foot)
            // globals
            uint32_t stickLocked;   // the follow camera ignores the stick's turn while set
            uint32_t camTable;      // the follow camera's tuning outdoors (+0x3C indoors): +0x34 / +0x38 pitch limits
            uint32_t camIndoors;    // the indoor table when neither 0 nor 4
            uint32_t wallSkip;      // > 0: the follow camera's update takes the stick's path at walls (pulls in)
            uint32_t scopeZoom;     // the FOV a zooming scope eases toward
            // CPed
            uint32_t pedPos;        // vec3: the matrix's position
            uint32_t pedAttached;   // the entity he is attached to (a gun in a vehicle's door), 0 none
        };

        // LCS SLUS-21423 v1.01
        constexpr Addrs kLcs{
            0x10Cu,  // 80 lbu, each times 0x2B0 + 0x220 before CCam::Process 0x27A380 (VCS +0x50 / 0x270 / +0x70)
            0x220u,  // "
            0x2B0u,  // "
            0x9Cu,   // CCamera::Process: VCS's 5 lw +0x888 align to +0x9C; the script camera setters 0x294B10 /
                     // 0x294B88 = VCS 0x37DEA8 / 0x37DF20 store 0 there
            0x40u,   // CCam::Process 0x27A380 switches on lhu +0x40 (VCS lhu +0)
            0xE0u,   // 12 aligned accesses in FollowPed, 8 in the scope update (VCS +0x78)
            0xF0u,   // 21 aligned in FollowPed, 16 in the scope update (VCS +0x7C)
            0xE8u,   // 6 aligned in FollowPed, 9 in the scope update, clamped to 70 there (VCS +0x134)
            0x160u,  // FollowPed's one store aligned with VCS's +0x1A0 (same code shape around it)
            {0x220u, 0x230u}, // unchanged: 6 aligned accesses in FollowPed
            0x274u,  // FollowPed / the scope update: lw +0x274 where VCS has lw +0x70, then ped fields (+0x760, +0x4E0)
            0x003D6C98u, // LCS 0x286F88 = VCS 0x2A2E30 (lw flag; mode 4?; ped state driving), 12 refs as VCS's
            0x003936D0u, // 0x78 bytes identical to VCS's 0x487B30; FollowPed 0x281E90 its only user
            0x003D8430u, // 3 lw aligned in FollowPed + 1 in the scope update (VCS 0x489F7C)
            0x003D6EA8u, // = 2 after the line-of-sight test (VCS 0x29F1FC / LCS 0x2834D0), 3 aligned
            0x003D6DE4u, // its only refs: 3 swc1 + 1 lwc1 in the scope update 0x27D440, the FOV eased toward it (VCS
                         // 0x487E2C)
            0x30u,   // the matrix's position (RenderWare layout, as VCS)
            0x760u,  // 7 aligned reads of the camera's ped in the scope update (VCS +0x858)
        };

        constexpr uint32_t kModeFollowPed = 4u;
        constexpr uint32_t kNoMode = 0xFFFFu;
        // the scope update's zoom (0x27D440): 7 (sniper) and 46 zoom, 46 down to FOV 3, the others to 15; at most 70
        constexpr float kZoomMin = 15.0f, kZoomMin46 = 3.0f, kZoomMax = 70.0f;
        constexpr uint32_t kWallTailMs = 300; // vcs_input.cpp's: the wall path kept this long after the last turn
        constexpr float kTwoPi = 6.2831855f;

        bool scopeMode(uint32_t m) { return m == 7 || m == 8 || m == 34 || m == 45 || m == 46; } // the jump table's
        bool zoomMode(uint32_t m) { return m == 7 || m == 46; }                                    // cases, decoded

        bool inRam(uint32_t a, uint32_t n = 4) { return a >= 0x100000u && (a & kRamMask) + n <= 0x02000000u; }
        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            if (inRam(a))
                std::memcpy(&v, rdram + (a & kRamMask), 4);
            return v;
        }
        float rdf(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (inRam(a))
                std::memcpy(&v, rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *rdram, uint32_t a, float v)
        {
            if (inRam(a))
                std::memcpy(rdram + (a & kRamMask), &v, 4);
        }
        void wr32(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (inRam(a))
                std::memcpy(rdram + (a & kRamMask), &v, 4);
        }

        // game thread
        bool g_footTurn = false;
        uint32_t g_footTurned = 0;
    }

    uint32_t activeCam(const uint8_t *rdram)
    {
        const uint32_t camera = game::addrs().world.theCamera;
        if (!rdram || !camera)
            return 0;
        const uint8_t active = rdram[(camera + kLcs.activeCam) & kRamMask];
        return active < 3 ? camera + kLcs.cams + active * kLcs.camSize : 0;
    }

    uint32_t mode(const uint8_t *rdram)
    {
        const uint32_t cam = activeCam(rdram);
        if (!cam)
            return kNoMode;
        uint16_t m;
        std::memcpy(&m, rdram + ((cam + kLcs.mode) & kRamMask), 2);
        return m;
    }

    bool footCamera(const uint8_t *rdram, bool driving, bool paused)
    {
        const game::Addrs &A = game::addrs();
        if (!rdram || driving || paused || !game::all({A.world.playerPed, A.world.theCamera, A.world.pad0}))
            return false;
        int16_t disabled;
        std::memcpy(&disabled, rdram + ((A.world.pad0 + A.off.padDisableControls) & kRamMask), 2);
        return rd32(rdram, A.world.playerPed) && mode(rdram) == kModeFollowPed &&
               rd32(rdram, A.world.theCamera + kLcs.looksAtPlayer) == 1 && disabled == 0 &&
               rd32(rdram, kLcs.stickLocked) == 0;
    }

    bool mouse(uint8_t *rdram, float turn, float tilt, float carTurn, float carTilt, bool driving, bool paused)
    {
        const uint32_t cam = activeCam(rdram);
        if (!cam)
            return false;
        const game::Addrs &A = game::addrs();
        const uint32_t m = mode(rdram);
        const uint32_t ped = rd32(rdram, A.world.playerPed);
        if (scopeMode(m) && !driving)
        {
            // The scopes turn by +0xE0 / +0xF0 as the follow camera; zoomed in they turn slower. Attached to a vehicle
            // the heading is the turn from the side he faces, signed (vcs_input.cpp).
            float fov = rdf(rdram, cam + kLcs.fov);
            fov = fov >= 1.0f && fov <= 130.0f ? fov : 70.0f;
            const float k = fov / 70.0f;
            float beta = std::fmod(rdf(rdram, cam + kLcs.beta) + turn * k, kTwoPi);
            if (ped && rd32(rdram, ped + kLcs.pedAttached))
                beta = std::remainder(rdf(rdram, cam + kLcs.beta) + turn * k, kTwoPi);
            else if (beta < 0.0f)
                beta += kTwoPi;
            wrf(rdram, cam + kLcs.beta, beta);
            wrf(rdram, cam + kLcs.alpha, rdf(rdram, cam + kLcs.alpha) + tilt * k);
            return true;
        }
        // looking out of a side with a drive-by weapon: the mouse aims the drive-by (game/lcs_driveby.h)
        if (driving && driveby::mouse(rdram, turn, tilt, paused))
            return true;
        if (driving && carcam::turns(m))
        {
            // the camera behind a vehicle turns at its next update and stays where it is put a while
            if (!paused)
                carcam::mouse(carTurn, carTilt);
            return true;
        }
        if (!footCamera(rdram, driving, paused) || !ped)
            return false;

        // heading: right turns clockwise (the angle falls), kept in 0..2 pi; the source points turn with it about the
        // player (the camera eases its heading back toward where they stand)
        if (turn != 0.0f)
        {
            const float beta = std::fmod(rdf(rdram, cam + kLcs.beta) + turn, kTwoPi);
            wrf(rdram, cam + kLcs.beta, beta < 0.0f ? beta + kTwoPi : beta);
            const float px = rdf(rdram, ped + kLcs.pedPos), py = rdf(rdram, ped + kLcs.pedPos + 4);
            const float c = std::cos(turn), s = std::sin(turn);
            for (const uint32_t off : kLcs.source)
            {
                const uint32_t at = cam + off;
                const float ox = rdf(rdram, at) - px, oy = rdf(rdram, at + 4) - py;
                wrf(rdram, at, px + ox * c - oy * s);
                wrf(rdram, at + 4, py + ox * s + oy * c);
            }
        }
        // pitch (up +), in the game's limits: the source points turn up / down about the target point by the same angle
        const uint32_t indoors = rd32(rdram, kLcs.camIndoors);
        const uint32_t table = kLcs.camTable + (indoors != 0 && indoors != 4 ? 0x3Cu : 0u);
        const float alpha = rdf(rdram, cam + kLcs.alpha);
        const float dTilt = std::clamp(alpha + tilt, -rdf(rdram, table + 0x38), rdf(rdram, table + 0x34)) - alpha;
        if (dTilt != 0.0f)
        {
            wrf(rdram, cam + kLcs.alpha, alpha + dTilt);
            const float tx = rdf(rdram, cam + kLcs.target), ty = rdf(rdram, cam + kLcs.target + 4),
                        tz = rdf(rdram, cam + kLcs.target + 8);
            for (const uint32_t off : kLcs.source)
            {
                const uint32_t at = cam + off;
                const float ox = rdf(rdram, at) - tx, oy = rdf(rdram, at + 4) - ty, oz = rdf(rdram, at + 8) - tz;
                const float h = std::sqrt(ox * ox + oy * oy), d = std::sqrt(h * h + oz * oz);
                if (h < 1e-4f)
                    continue;
                const float e = std::clamp(std::atan2(-oz, h) + dTilt, -1.55f, 1.55f); // looking up: below the target
                const float k = d * std::cos(e) / h;
                wrf(rdram, at, tx + ox * k);
                wrf(rdram, at + 4, ty + oy * k);
                wrf(rdram, at + 8, tz - d * std::sin(e));
            }
        }
        // walls: while the mouse turns it (and a moment after) the update takes the stick's path, which pulls the
        // camera in at a wall (vcs_input.cpp, VCS_FOOT_WALL)
        const uint32_t clock = rd32(rdram, A.timer.timeMs);
        if (turn != 0.0f || dTilt != 0.0f)
        {
            g_footTurn = true;
            g_footTurned = clock;
        }
        else if (clock - g_footTurned > kWallTailMs)
            g_footTurn = false;
        if (g_footTurn && rd32(rdram, kLcs.wallSkip) == 0)
            wr32(rdram, kLcs.wallSkip, 1);
        return true;
    }

    bool wheelZoom(uint8_t *rdram, bool in, bool out)
    {
        const uint32_t m = mode(rdram);
        if (!zoomMode(m))
            return false;
        const float zoomed = rdf(rdram, kLcs.scopeZoom) * (in ? 0.8f : 1.0f) * (out ? 1.25f : 1.0f);
        wrf(rdram, kLcs.scopeZoom, std::clamp(zoomed, m == 46 ? kZoomMin46 : kZoomMin, kZoomMax));
        return true;
    }
}
