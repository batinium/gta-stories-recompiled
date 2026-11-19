// [lcs] See lcs_driveby.h. Addresses: LCS SLUS-21423 v1.01, read from its code (Ghidra) and the generated C++.
//
// - The drive-bys: 0x2AF200 (cars; returns early for model 0x8A), 0x2AF628, 0x2AF938 (bikes: anims 0xC5..0xC7) - the
//   three callers of CWeapon::FireFromCar 0x2A30F0 (a0 the weapon slot, a1 the vehicle). Each: the driver's weapon
//   info (0x34F2F0) +0x68 must be 5, left / right from the active CCam's +0x30 / +0x34 (or the pad's look buttons
//   0x171310 / 0x171388 in camera mode 1), fire = 0x1715F0 (Circle), paced by the slot's +0x10 time. The arm's pose
//   is a fixed clip per side (cars 0x69 / 0x6A, low cars 0x6B / 0x6C, bikes 0xC5 left / 0xC6 right / 0xC7 ahead;
//   0x1F74B8 blends it in): no aim angle to turn it by. The gun is in the right hand (along the hand bone's X axis).
// - The arm IK (VC's CPedIK at CPed +0x310: +0 the ped, +0x14 / +0x18 the upper arm's yaw / pitch, +0x1C / +0x20 the
//   forearm's, +0x2C flags): PointGunInDirection 0x270730 (from CPed::PointGunAt 0x1D4F58, CPed::ProcessControl
//   0x1D6F30 while bit 39 of +0x190 is set; the player's pitch +0xC78) -> PointGunInDirectionUsingArm 0x270A70 (f12
//   yaw from m_fRotationCur +0x4E0, f13 pitch, a0 the IK): the clavicle's matrix as last drawn gives the arm's base,
//   MoveLimb 0x26FFD8 steps the arm toward it within the limb info 0x3935B0 (radians: yaw +5 / -120 deg, step 20,
//   pitch +-70, step 20; forearm 0x3935C8), and RtQuatRotate 0x3330D8 (degrees, the quaternion x y z w at the bone's
//   keyframe +8, axis 0x3BB810 / 20 / 30 = X / Y / Z; 0 replace, 1 q * r, 2 r * q) sets the upper arm (+0x2AC) about
//   X, Y and Z. The bones as last drawn: the clump (+0x50)'s first atomic's skin hierarchy (+0x2C; 0x17C760 /
//   0x17C730 / 0x333848), its world matrices +0x28 by node (+0x30 ids, 0x333518), a bone's X axis along the bone.
// - FireInstantHitFromCar 0x2AC710 (FireFromCar's only call, 0x2A3128): source = the driver's hand bone (or the car's
//   seat offsets), target = source -/+ the vehicle's right * range (left / right), on a bike also + forward * range;
//   + a random +-1.28 m per axis; then DoDriveByAutoAiming 0x2AE1C8 (a0 the player ped, a1 the vehicle, a2 &source,
//   a3 &target; called at 0x2AD144, return 0x2AD14C) and the line test 0x2B0BD0 (the vehicle ignored, 0x3D9CA4).
// - CCam::Process 0x27A380, after the mode's update (which places Source afresh each frame), for modes 3 / 16 / 18 /
//   22 behind a vehicle: LookBehind 0x2786B8, LookLeft 0x278F90 (+0x30 = 1), LookRight 0x279778 (+0x34 = 1).
//   LookLeft / LookRight (modes 3 / 18 / 22): Source +0x1D0 x / y = the vehicle + 9 (mode 18: +0x104) m to the other
//   side, z the mode's; then CCamera::AvoidTheGeometry 0x287B78 (f12 FOV, a0 TheCamera, a1 &Source, a2 &the vehicle's
//   position, a3 &CCam Source: Source put at the same distance from the vehicle, pulled in to what the line from the
//   vehicle hits; TheCamera +0xB50 a small push off the wall and +0xA4 m_bMoveCamToAvoidGeom = 1; returns 0x27921C /
//   0x279A08); Source z kept above the roof; Front +0x1C0 = vehicle - Source + 1.1 z (+1.2 more in mode 22), then
//   0x278148 (Front normalised, Up made from it: 21 callers, the look functions' at 0x279384 / 0x279B70).
// - CCamera::Process 0x28FAC8: with +0xA4 set the matrix's forward is not the CCam's Front but its +0x160
//   (m_cvecTargetCoorsForFudgeInter) - (Source + TheCamera +0xB50): it looks at the follow camera's target point, so a
//   Front tilted by the aim tilted the view only about half as much.
// - CWeaponEffects::Render 0x2B6250: while a target is marked (0x60B810 != 0; 0x60B840 its entity, 0x60B820 the point,
//   0x60B83C RGBA) and the point is on the screen, the player's free aim (FindPlayerPed() +0xCBC, the call returning
//   to 0x2B6580) draws the aiming circle in the middle of the screen, else three markers around the point.
// - Stats: shots fired by the player 0x3D9E50 (FireFromCar), hits 0x3D9E5C (FireInstantHitFromCar's hit cases).
#include "game/lcs_driveby.h"

#include "game/game_addrs.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace lcs::driveby
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            uint32_t camUp;              // 0x278148: Front normalised, Up from it (a0 the CCam)
            uint32_t lookLeftReturn, lookRightReturn; // its calls in LookLeft / LookRight (return addresses)
            uint32_t avoid;              // CCamera::AvoidTheGeometry (a0 TheCamera, a1 &source, a2 &target, a3 &out)
            uint32_t avoidLeftReturn, avoidRightReturn; // its calls in LookLeft / LookRight
            uint32_t autoAim;            // DoDriveByAutoAiming (a0 ped, a1 vehicle, a2 &source, a3 &target)
            uint32_t autoAimReturn;      // its call in FireInstantHitFromCar
            uint32_t activeCam, cams, camSize; // TheCamera: ActiveCam (u8), Cams[0], sizeof(CCam)
            uint32_t clearGeometry;      // TheCamera: the push off the wall the matrix adds to Source (vec3)
            uint32_t camMode, camLookLeft, camLookRight, camFront, camSource, camTargetEntity; // CCam offsets
            uint32_t camFudgeTarget;     // CCam: the point the matrix looks at after AvoidTheGeometry
            uint32_t pedSlot, pedWeapons; // s8 current slot, the slots (0x1C each, type first)
            uint32_t weaponInfo;         // *(*(this) + 4) + type * 0x70
            uint32_t infoDriveBy;        // weapon info: == 5 for the drive-by's weapons
            uint32_t vehicleType;        // u32: 5 a bike
            uint32_t statFired, statHits; // u32 counters
            uint32_t effectsRender;      // CWeaponEffects::Render
            uint32_t findPlayerPed, findPlayerPedCircleReturn; // FindPlayerPed; its call before Render's free-aim test
            uint32_t freeAim;            // CPed: the player's free aim (u32)
            uint32_t marker;             // gCrossHair (CWeaponEffects::MarkTarget 0x2B6108 sets it): +0 on (u32),
                                         // +0x10 the point (vec3), +0x2C RGBA, +0x30 the entity, +0x34 its fade
            uint32_t driveBys[3];        // the vehicles' drive-by updates (a0 the vehicle), from their ProcessControl
            uint32_t armIk;              // CPedIK::PointGunInDirectionUsingArm (f12 yaw, f13 pitch, a0 the CPedIK)
            uint32_t pedIk;              // CPed: its CPedIK (+0 the ped, +0x14.. the arm's yaw / pitch, +0x2C flags)
            uint32_t pedRotation;        // CPed: m_fRotationCur (the IK measures the arm's heading from it)
        };
        constexpr Addrs kLcs{
            0x00278148u, 0x0027938Cu, 0x00279B78u,
            0x00287B78u, 0x0027921Cu, 0x00279A08u,
            0x002AE1C8u, 0x002AD14Cu,
            0x10Cu, 0x220u, 0x2B0u, 0xB50u,
            0x40u, 0x30u, 0x34u, 0x1C0u, 0x1D0u, 0x274u, 0x160u,
            0x6B8u, 0x594u, 0x003D9B48u, 0x68u, 0x344u,
            0x003D9E50u, 0x003D9E5Cu,
            0x002B6250u, 0x00107368u, 0x002B6580u, 0xCBCu,
            0x0060B810u,
            {0x002AF200u, 0x002AF628u, 0x002AF938u},
            0x00270A70u, 0x310u, 0x4E0u,
        };
        const Addrs &A = kLcs;
        constexpr uint32_t kBike = 5u;

        // the look camera turned about the point it looks at; the aim's limits
        constexpr float kYawLimit = 1.658f;   // 95 deg either way of the side: a little past straight ahead / behind
        constexpr float kPitchDown = -0.26f, kPitchUp = 0.44f; // 15 deg down, 25 up
        constexpr float kOrbitLow = -0.12f, kOrbitHigh = 1.0f; // the camera's elevation about that point (not underground)
        constexpr float kBikeForwardLimit = 1.047f; // the bike's straight-ahead shot: 60 deg either way
        // the right stick at full tilt (on a curve, x |x|: fine aim near the middle), radians a second
        constexpr float kStickYawRate = 2.4f, kStickPitchRate = 1.4f;
        // the view looks this far over the point the game's look looks at (the vehicle + 1.1 m): the middle of the
        // screen - the aim - clear of the roof / the rider's head (LCS_DRIVEBY_LIFT, metres)
        constexpr float kDefaultLift = 0.7f;
        constexpr uint32_t kScratch = 0x00060010u; // guest scratch (lcs_aim.cpp uses 0x60000..0x6000F): a u32 1, the
                                                   // free-aim flag of the "player" Render's circle test is handed

        uint8_t *g_rdram = nullptr;
        PS2Runtime::RecompiledFunction g_origCamUp = nullptr, g_origAutoAim = nullptr, g_origAvoid = nullptr;
        PS2Runtime::RecompiledFunction g_origRender = nullptr, g_origFindPlayerPed = nullptr;
        bool g_bikeForward = true, g_crosshair = true;
        float g_lift = kDefaultLift;

        // the right stick (vcs_input.cpp's pad read -> the camera's update), -1..1, x right +, y down +
        std::atomic<float> g_stickX{0.0f}, g_stickY{0.0f};

        // game thread
        float g_yaw = 0.0f, g_pitch = 0.0f;
        int g_side = 0;                       // 1 left, 2 right: the look the aim is for
        uint32_t g_lookFrame = 0xFFFFFFFFu;   // the frame the look camera was last turned
        uint32_t g_stickFrame = 0xFFFFFFFFu;  // the frame the stick was last added
        bool g_haveRay = false;
        float g_raySource[3] = {}, g_rayDir[3] = {};
        // AvoidTheGeometry's call this frame: the view it is for (heading, elevation), the place asked for
        uint32_t g_placedFrame = 0xFFFFFFFFu, g_placedCam = 0;
        float g_viewHeading = 0.0f, g_viewElevation = 0.0f;
        float g_asked[3] = {};
        bool g_markerOwned = false;           // the aiming circle is ours (gCrossHair set here)
        // Toni's arm (driveBy<>): the drive-by update's call, sent through the game's arm IK first
        PS2Runtime::RecompiledFunction g_origDriveBy[3] = {};
        bool g_arm = true;
        bool g_armCalling = false;            // the IK runs; it returns into the drive-by update's entry
        uint32_t g_armRa = 0, g_armA0 = 0, g_armPed = 0;
        float g_armRotation = 0.0f;           // the ped's m_fRotationCur, put back after the IK
        std::atomic<int> g_armMode{3};        // lcsdrivebyarm: 0 off, 1 the IK, 2 + the forearm, 3 + the hand
        std::atomic<float> g_armYawBias{0.0f}, g_armPitchBias{0.0f};
        // the upper arm's limits (the IK's limb info, radians: yaw max, min, step, pitch max, min, step) widened for
        // the drive-by's call (put back after it): on foot the right arm points from 5 deg left of ahead round to 120
        // deg right; out of a vehicle's left side it has to reach across
        constexpr uint32_t kArmLimits = 0x003935B0u;
        float g_armLimitsSaved[6] = {};
        std::atomic<float> g_armYawMax{2.6f}, g_armYawMin{-3.1f};
        constexpr float kArmReach = 2.45f; // radians from his heading the arm is asked to point at most
        // the IK measures the arm against the clavicle's axes, which the drive-by clips tilt: what it was asked is
        // corrected by how far the drawn arm missed (game thread)
        float g_armCorrYaw = 0.0f, g_armCorrPitch = 0.0f;
        float g_armWant[3] = {};             // the direction asked last frame
        uint32_t g_armFrame = 0xFFFFFFFFu;    // the frame it was asked
    }
    namespace
    {
        bool inRam(uint32_t a);
        uint32_t rd32(uint32_t a);
        // a bone's matrix as last drawn (world): the skin hierarchy of the clump's first atomic (+0x2C, as 0x17C760 /
        // 0x17C730 find it: the clump's atomic list at +8, an atomic's link at +0x1C), its matrices +0x28 (0x40 each)
        // by node (+0x30: 8 bytes a node, its id first; +4 the count); the bone's id at +0x14 of the CPed's frame
        // pointer (+0x2A0 + node * 4, VC's order: +0x2AC upper arm R, +0x2B4 hand R, +0x2D4 forearm R, +0x2DC
        // clavicle R)
        uint32_t boneMatrix(uint32_t ped, uint32_t frameOff)
        {
            const uint32_t clump = rd32(ped + 0x50);
            const uint32_t link = clump ? rd32(clump + 8) : 0;
            const uint32_t hier = link && link != clump + 8 ? rd32(link + 0x10) : 0;
            const uint32_t frame = rd32(ped + frameOff);
            if (!hier || !frame)
                return 0;
            const uint32_t mats = rd32(hier + 0x28), nodes = rd32(hier + 0x30), count = rd32(hier + 4);
            const uint8_t id = g_rdram[(frame + 0x14) & kRamMask];
            for (uint32_t i = 0; i < count && i < 64; ++i)
                if (g_rdram[(nodes + i * 8) & kRamMask] == id)
                    return inRam(mats) ? mats + i * 0x40 : 0;
            return 0;
        }
        // [android] host time (steady_clock ms) the look camera was last aimed, for aiming() on any thread
        std::atomic<int64_t> g_lookHostMs{INT64_MIN / 2};
        int64_t hostMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
        // reported ("lcsdriveby")
        std::mutex g_reportMutex;
        struct Report
        {
            int side = 0;
            float yaw = 0, pitch = 0;
            uint32_t shots = 0, bikeShots = 0;
            float start[3] = {}, dir[3] = {};
            bool crosshair = false;
            float wallPull = 0.0f; // m the look camera was pulled in from where it was asked to be, last frame
            float viewDir[3] = {}; // the view's forward (TheCamera's matrix) when the aim was set
            uint32_t armCalls = 0;  // the arm IK's calls so far
            float armYaw = 0, armPitch = 0; // what it was asked last (the heading from m_fRotationCur, the pitch)
            float ik[4] = {};       // the CPedIK's upper arm / forearm yaw, pitch after it (+0x14..)
            float corr[2] = {};     // the correction from the arm drawn (yaw, pitch)
            float want[3] = {};     // the arm's direction asked (unit)
        } g_report;

        bool inRam(uint32_t a) { return a >= 0x00100000u && (a & kRamMask) + 4 <= 0x02000000u; }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            if (inRam(a))
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v)
        {
            if ((a & kRamMask) + 4 <= 0x02000000u)
                std::memcpy(g_rdram + (a & kRamMask), &v, 4);
        }
        float rdf(uint32_t a)
        {
            float v = 0.0f;
            if (inRam(a))
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v)
        {
            if ((a & kRamMask) + 4 <= 0x02000000u)
                std::memcpy(g_rdram + (a & kRamMask), &v, 4);
        }
        void rdVec(uint32_t a, float v[3])
        {
            for (int i = 0; i < 3; ++i)
                v[i] = rdf(a + i * 4);
        }
        void wrVec(uint32_t a, const float v[3])
        {
            for (int i = 0; i < 3; ++i)
                wrf(a + i * 4, v[i]);
        }

        uint32_t frame() { return rd32(game::addrs().timer.frameCounter); }
        uint32_t playerPed() { return rd32(game::addrs().world.playerPed); }
        uint32_t playerVehicle(uint32_t ped)
        {
            const game::Addrs &G = game::addrs();
            return ped && rd32(ped + G.off.pedInVehicle) ? rd32(ped + G.off.pedVehicle) : 0;
        }
        uint32_t activeCam()
        {
            const uint32_t camera = game::addrs().world.theCamera;
            if (!camera)
                return 0;
            const uint8_t i = g_rdram[(camera + A.activeCam) & kRamMask];
            return camera + A.cams + (i < 3 ? i : 0) * A.camSize;
        }
        uint32_t camMode(uint32_t cam)
        {
            uint16_t m;
            std::memcpy(&m, g_rdram + ((cam + A.camMode) & kRamMask), 2);
            return m;
        }
        bool lookMode(uint32_t mode) { return mode == 3 || mode == 18 || mode == 22; }
        int lookSide(uint32_t cam) { return rd32(cam + A.camLookLeft) ? 1 : rd32(cam + A.camLookRight) ? 2 : 0; }

        // the weapon in the player's hand is a drive-by one (the drive-bys' own test)
        bool driveByWeapon(uint32_t ped)
        {
            const int8_t slot = static_cast<int8_t>(g_rdram[(ped + A.pedSlot) & kRamMask]);
            if (slot < 0 || slot >= 10)
                return false;
            const uint32_t type = rd32(ped + A.pedWeapons + slot * 0x1Cu);
            const uint32_t table = rd32(rd32(A.weaponInfo) + 4);
            return type && type < 64 && table && rd32(table + type * 0x70u + A.infoDriveBy) == 5u;
        }

        // The player looks out of a side of his vehicle with a drive-by weapon, through the active camera: its side.
        int aimSide(uint32_t &cam, uint32_t &ped, uint32_t &vehicle)
        {
            ped = playerPed();
            vehicle = playerVehicle(ped);
            cam = activeCam();
            if (!vehicle || !cam || !lookMode(camMode(cam)) || rd32(cam + A.camTargetEntity) != vehicle || !driveByWeapon(ped))
                return 0;
            return lookSide(cam);
        }

        // a look that is not the one aimed (another side, or after a gap) starts square to the side
        void startLook(int side, uint32_t now)
        {
            if (side != g_side || now - g_lookFrame > 3u)
            {
                g_yaw = g_pitch = 0.0f;
                g_side = side;
            }
        }

        bool aimingNow()
        {
            uint32_t cam = 0, ped = 0, vehicle = 0;
            const int side = aimSide(cam, ped, vehicle);
            return side && side == g_side && g_haveRay && frame() - g_lookFrame <= 2u;
        }

        void publish()
        {
            std::lock_guard<std::mutex> lock(g_reportMutex);
            g_report.side = g_side;
            g_report.yaw = g_yaw;
            g_report.pitch = g_pitch;
        }

        // CCamera::AvoidTheGeometry from LookLeft / LookRight: the look camera's place (a1) turned about the point the
        // game's look looks at, before the game keeps it out of walls (along the line from the vehicle to it).
        PS2_GUEST_CC void avoid(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == A.avoidLeftReturn || ra == A.avoidRightReturn)
            {
                uint32_t cam = 0, ped = 0, vehicle = 0;
                const int side = aimSide(cam, ped, vehicle);
                const uint32_t srcAt = GPR_U32(ctx, 5), targetAt = GPR_U32(ctx, 6);
                if (side && (srcAt & kRamMask) + 12 <= 0x02000000u && (targetAt & kRamMask) + 12 <= 0x02000000u)
                {
                    const uint32_t now = frame();
                    startLook(side, now);
                    // the right stick, once a frame (the game's time step: 1 = 1/50 s)
                    if (g_stickFrame != now)
                    {
                        g_stickFrame = now;
                        const float sx = g_stickX.load(std::memory_order_relaxed), sy = g_stickY.load(std::memory_order_relaxed);
                        const float step = std::clamp(rdf(game::addrs().timer.timeStep), 0.0f, 3.0f) / 50.0f;
                        g_yaw = std::clamp(g_yaw - sx * std::fabs(sx) * kStickYawRate * step, -kYawLimit, kYawLimit);
                        g_pitch = std::clamp(g_pitch - sy * std::fabs(sy) * kStickPitchRate * step, kPitchDown, kPitchUp);
                    }
                    g_lookFrame = now;
                    g_lookHostMs.store(hostMs(), std::memory_order_relaxed);
                    float s[3], t[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        std::memcpy(&s[i], g_rdram + ((srcAt + i * 4) & kRamMask), 4);
                        std::memcpy(&t[i], g_rdram + ((targetAt + i * 4) & kRamMask), 4);
                    }
                    // the point the game's look looks at (LookLeft's Front: the vehicle - Source + 1.1 z, + 1.2 in 22)
                    const float at[3] = {t[0], t[1], t[2] + 1.1f + (camMode(cam) == 22 ? 1.2f : 0.0f)};
                    const float o[3] = {s[0] - at[0], s[1] - at[1], s[2] - at[2]}; // Source from it
                    const float h = std::sqrt(o[0] * o[0] + o[1] * o[1]);
                    const float d = std::sqrt(h * h + o[2] * o[2]);
                    if (h > 0.01f && d > 0.01f)
                    {
                        // Source orbits that point by the aim (down when it looks up, as far as the orbit's limits);
                        // the view looks back across it, its pitch the game's plus the aim's (one to one, whatever the
                        // orbit's limits or the roof did to the camera's height) plus the lift's
                        const float heading = std::atan2(o[1], o[0]) + g_yaw;
                        const float e = std::atan2(o[2], h);
                        const float e2 = std::clamp(e - g_pitch, std::min(e, kOrbitLow), std::max(e, kOrbitHigh));
                        const float s2[3] = {at[0] + d * std::cos(e2) * std::cos(heading), at[1] + d * std::cos(e2) * std::sin(heading),
                                             at[2] + d * std::sin(e2)};
                        for (int i = 0; i < 3; ++i)
                            std::memcpy(g_rdram + ((srcAt + i * 4) & kRamMask), &s2[i], 4);
                        std::memcpy(g_asked, s2, sizeof(s2));
                        g_viewHeading = heading + 3.14159265f;
                        g_viewElevation = std::clamp(-e + g_pitch + std::atan2(g_lift, d), -1.2f, 1.2f);
                        g_placedFrame = now;
                        g_placedCam = cam;
                    }
                    publish();
                }
            }
            g_origAvoid(rdram, ctx, runtime);
        }

        // 0x278148, from LookLeft / LookRight: Source as the game kept it out of walls (from the place avoid() asked
        // for), Front = the aim. CCamera::Process's matrix looks at +0x160 after AvoidTheGeometry: that point put on
        // the aim's line, so the view is the aim.
        PS2_GUEST_CC void camUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const uint32_t cam = GPR_U32(ctx, 4);
            if ((ra == A.lookLeftReturn || ra == A.lookRightReturn) && g_placedFrame == frame() &&
                (cam & kRamMask) == (g_placedCam & kRamMask))
            {
                const float ce = std::cos(g_viewElevation);
                const float f[3] = {ce * std::cos(g_viewHeading), ce * std::sin(g_viewHeading), std::sin(g_viewElevation)};
                float s[3], push[3] = {};
                rdVec(cam + A.camSource, s);
                if (const uint32_t camera = game::addrs().world.theCamera)
                    rdVec(camera + A.clearGeometry, push);
                const float fudge[3] = {s[0] + push[0] + f[0] * 20.0f, s[1] + push[1] + f[1] * 20.0f, s[2] + push[2] + f[2] * 20.0f};
                wrVec(cam + A.camFront, f);
                wrVec(cam + A.camFudgeTarget, fudge);
                std::memcpy(g_raySource, s, sizeof(s));
                std::memcpy(g_rayDir, f, sizeof(f));
                g_haveRay = true;
                g_placedFrame = 0xFFFFFFFFu;
                const float pull = std::sqrt((s[0] - g_asked[0]) * (s[0] - g_asked[0]) + (s[1] - g_asked[1]) * (s[1] - g_asked[1]) +
                                             (s[2] - g_asked[2]) * (s[2] - g_asked[2]));
                std::lock_guard<std::mutex> lock(g_reportMutex);
                g_report.wallPull = pull;
                std::memcpy(g_report.viewDir, f, sizeof(f));
            }
            g_origCamUp(rdram, ctx, runtime);
        }

        // CWeaponEffects::Render: while aiming a drive-by, the game's own aiming circle in the middle of the screen (as
        // its free aim draws it: gCrossHair on, no entity, its point ahead on the aim; FindPlayerPed's call before the
        // circle's test answered with a "player" whose free-aim flag is set). Never over a target the game marked.
        PS2_GUEST_CC void effectsRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_crosshair && aimingNow() && (g_markerOwned || rd32(A.marker) == 0))
            {
                const uint32_t camera = game::addrs().world.theCamera;
                float pos[3], fwd[3];
                rdVec(camera + 0x30, pos);
                rdVec(camera + 0x10, fwd);
                const float point[3] = {pos[0] + fwd[0] * 30.0f, pos[1] + fwd[1] * 30.0f, pos[2] + fwd[2] * 30.0f};
                wr32(A.marker, 1);
                wrVec(A.marker + 0x10, point);
                wr32(A.marker + 0x2C, 0xFFFFFFFFu); // white, opaque
                wr32(A.marker + 0x30, 0);
                wrf(A.marker + 0x34, 1.0f);
                wr32(kScratch, 1);
                g_markerOwned = true;
            }
            else if (g_markerOwned)
            {
                if (rd32(A.marker + 0x30) == 0)
                    wr32(A.marker, 0); // ours off (a target the game marked since is left alone)
                g_markerOwned = false;
            }
            {
                std::lock_guard<std::mutex> lock(g_reportMutex);
                g_report.crosshair = g_markerOwned;
            }
            g_origRender(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void findPlayerPed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_markerOwned && GPR_U32(ctx, 31) == A.findPlayerPedCircleReturn)
            {
                SET_GPR_U32(ctx, 2, kScratch - A.freeAim); // only its +0xCBC is read there
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origFindPlayerPed(rdram, ctx, runtime);
        }

        // DoDriveByAutoAiming from FireInstantHitFromCar: the player's shot down the look camera's line (instead of the
        // side, the spread and the auto-aim); a bike's straight-ahead shot turned to the camera's heading first.
        PS2_GUEST_CC void autoAim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = playerPed();
            const uint32_t vehicle = playerVehicle(ped);
            if (GPR_U32(ctx, 31) == A.autoAimReturn && vehicle && GPR_U32(ctx, 4) == ped && GPR_U32(ctx, 5) == vehicle)
            {
                const uint32_t srcAt = GPR_U32(ctx, 6), dstAt = GPR_U32(ctx, 7);
                float src[3], dst[3];
                rdVec(srcAt, src);
                rdVec(dstAt, dst);
                const float range = std::sqrt((dst[0] - src[0]) * (dst[0] - src[0]) + (dst[1] - src[1]) * (dst[1] - src[1]) +
                                              (dst[2] - src[2]) * (dst[2] - src[2]));
                uint32_t cam = 0, p = 0, v = 0;
                const int side = aimSide(cam, p, v);
                if (side && side == g_side && g_haveRay && frame() - g_lookFrame <= 2u && range > 0.5f)
                {
                    // from beside the gun on the line through the middle of the screen (where the aiming circle is),
                    // along it (lcs_aim.cpp's shot line): the camera's matrix as shown, else the CCam's line as turned
                    float origin[3], dir[3];
                    std::memcpy(origin, g_raySource, sizeof(origin));
                    std::memcpy(dir, g_rayDir, sizeof(dir));
                    if (const uint32_t camera = game::addrs().world.theCamera)
                    {
                        float pos[3], fwd[3];
                        rdVec(camera + 0x30, pos);
                        rdVec(camera + 0x10, fwd);
                        const float len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
                        const float dx = pos[0] - src[0], dy = pos[1] - src[1], dz = pos[2] - src[2];
                        if (len > 0.5f && len < 2.0f && dx * dx + dy * dy + dz * dz < 40.0f * 40.0f)
                            for (int i = 0; i < 3; ++i)
                            {
                                origin[i] = pos[i];
                                dir[i] = fwd[i] / len;
                            }
                    }
                    const float along = std::max(0.0f, (src[0] - origin[0]) * dir[0] + (src[1] - origin[1]) * dir[1] +
                                                           (src[2] - origin[2]) * dir[2]);
                    const float start[3] = {origin[0] + dir[0] * along, origin[1] + dir[1] * along, origin[2] + dir[2] * along};
                    const float stop[3] = {start[0] + dir[0] * range, start[1] + dir[1] * range, start[2] + dir[2] * range};
                    wrVec(srcAt, start);
                    wrVec(dstAt, stop);
                    {
                        std::lock_guard<std::mutex> lock(g_reportMutex);
                        ++g_report.shots;
                        std::memcpy(g_report.start, start, sizeof(start));
                        std::memcpy(g_report.dir, dir, sizeof(g_report.dir));
                    }
                    ctx->pc = GPR_U32(ctx, 31); // no auto-aim: the shot goes where it is aimed
                    return;
                }
                if (g_bikeForward && !side && cam && rd32(vehicle + A.vehicleType) == kBike && range > 0.5f)
                {
                    // straight ahead on a bike: the shot's heading turned to the camera's, within 60 deg of the bike's
                    const float fx = rdf(cam + A.camFront), fy = rdf(cam + A.camFront + 4);
                    const float bx = rdf(vehicle + 0x10), by = rdf(vehicle + 0x14); // the bike's forward
                    const float dx = dst[0] - src[0], dy = dst[1] - src[1];
                    if ((fx != 0.0f || fy != 0.0f) && (bx != 0.0f || by != 0.0f) && dx * bx + dy * by > 0.0f)
                    {
                        const float bike = std::atan2(by, bx);
                        const float want = bike + std::clamp(std::remainder(std::atan2(fy, fx) - bike, 6.2831855f),
                                                             -kBikeForwardLimit, kBikeForwardLimit);
                        const float turn = want - std::atan2(dy, dx);
                        const float c = std::cos(turn), s = std::sin(turn);
                        const float stop[3] = {src[0] + dx * c - dy * s, src[1] + dx * s + dy * c, dst[2]};
                        wrVec(dstAt, stop);
                        std::lock_guard<std::mutex> lock(g_reportMutex);
                        ++g_report.bikeShots;
                        std::memcpy(g_report.start, src, sizeof(src));
                        const float len = std::max(range, 0.01f);
                        const float d3[3] = {(stop[0] - src[0]) / len, (stop[1] - src[1]) / len, (stop[2] - src[2]) / len};
                        std::memcpy(g_report.dir, d3, sizeof(d3));
                    }
                }
            }
            g_origAutoAim(rdram, ctx, runtime);
        }

        // The vehicles' drive-by updates (from their ProcessControl, after the peds' animations were applied this frame,
        // before the frame is drawn): while the player aims a drive-by, the game's own arm IK (PointGunInDirectionUsingArm,
        // what CPed::PointGunAt uses on foot with a pistol / SMG) is run first with the aim's direction, so his gun arm
        // points where the circle is instead of the clip's square-to-the-side pose. The IK is a guest function: it is
        // entered by setting pc (its return address = this update's entry), and the update runs when it comes back here.
        template <int I> PS2_GUEST_CC void driveBy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t entry = A.driveBys[I];
            if (g_armCalling && GPR_U32(ctx, 31) != entry && frame() - g_armFrame > 2u)
            {
                // the IK never came back (a load in between): the arm's limits put back
                g_armCalling = false;
                for (int i = 0; i < 6; ++i)
                    wrf(kArmLimits + i * 4, g_armLimitsSaved[i]);
            }
            if (g_armCalling && GPR_U32(ctx, 31) == entry)
            {
                // back from the IK: m_fRotationCur put back, the update's own a0 / return address
                g_armCalling = false;
                wrf(g_armPed + A.pedRotation, g_armRotation);
                for (int i = 0; i < 6; ++i)
                    wrf(kArmLimits + i * 4, g_armLimitsSaved[i]);
                // the clip's elbow (and wrist) bend kept the forearm square to the arm the IK turned: straightened
                const int mode = g_armMode.load(std::memory_order_relaxed);
                for (const uint32_t off : {0x2D4u, 0x2B4u})
                {
                    if (off == 0x2B4u && mode < 3)
                        continue;
                    if (mode < 2)
                        continue;
                    const uint32_t frame = rd32(g_armPed + off);
                    const uint32_t key = frame ? rd32(frame + 0x10) : 0;
                    if (key)
                    {
                        const float id[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        for (int i = 0; i < 4; ++i)
                            wrf(key + 8 + i * 4, id[i]);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_reportMutex);
                    for (int i = 0; i < 4; ++i)
                        g_report.ik[i] = rdf(g_armPed + A.pedIk + 0x14 + i * 4);
                }
                SET_GPR_U32(ctx, 4, g_armA0);
                SET_GPR_U32(ctx, 31, g_armRa);
                g_origDriveBy[I](rdram, ctx, runtime);
                return;
            }
            const uint32_t vehicle = GPR_U32(ctx, 4);
            const uint32_t ped = playerPed();
            if (g_arm && g_armMode.load(std::memory_order_relaxed) && !g_armCalling && ped && aimingNow() &&
                playerVehicle(ped) == vehicle)
            {
                // the aim's point: on the line through the middle of the screen, 20 m past the player; from his
                // shoulder (the upper arm as last drawn)
                float p[3];
                const uint32_t upper = boneMatrix(ped, 0x2ACu);
                const uint32_t now = frame();
                if (upper)
                    rdVec(upper + 0x30, p);
                else
                {
                    rdVec(ped + 0x30, p);
                    p[2] += 0.6f;
                }
                // how far the arm drawn missed what was asked last frame (its X axis is the arm's line): the next
                // ask corrected by half of it, so the arm settles on the aim in a few frames
                if (upper && now - g_armFrame <= 2u)
                {
                    float x[3];
                    rdVec(upper, x);
                    const float hx = std::sqrt(x[0] * x[0] + x[1] * x[1]);
                    const float hw = std::sqrt(g_armWant[0] * g_armWant[0] + g_armWant[1] * g_armWant[1]);
                    if (hx > 0.05f && hw > 0.05f)
                    {
                        const float dy = std::remainder(std::atan2(-g_armWant[0], g_armWant[1]) - std::atan2(-x[0], x[1]), 6.2831855f);
                        const float dp = std::atan2(g_armWant[2], hw) - std::atan2(x[2], hx);
                        g_armCorrYaw = std::clamp(g_armCorrYaw + 0.5f * dy, -1.0f, 1.0f);
                        g_armCorrPitch = std::clamp(g_armCorrPitch - 0.5f * dp, -1.5f, 1.5f);
                    }
                }
                else if (now - g_armFrame > 2u)
                    g_armCorrYaw = g_armCorrPitch = 0.0f;
                const float along = (p[0] - g_raySource[0]) * g_rayDir[0] + (p[1] - g_raySource[1]) * g_rayDir[1] +
                                    (p[2] - g_raySource[2]) * g_rayDir[2];
                float d[3];
                for (int i = 0; i < 3; ++i)
                    d[i] = g_raySource[i] + g_rayDir[i] * (along + 20.0f) - p[i];
                const float h = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                if (h > 0.1f)
                {
                    // GTA's heading (0 = +y, counter-clockwise), as the IK's caller passes it less m_fRotationCur
                    // m_fRotationCur is not kept in a vehicle: his heading from his matrix for the call
                    // His right arm reaches round to 140 deg from ahead either way (across his chest to the left);
                    // an aim further behind him holds the arm there (the asked direction too, or the correction
                    // would wind up against the IK's limits).
                    const float facing = std::atan2(-rdf(ped + 0x10), rdf(ped + 0x14));
                    const float rel = std::clamp(std::remainder(std::atan2(-d[0], d[1]) - facing, 6.2831855f), -kArmReach, kArmReach);
                    const float heading = facing + rel;
                    d[0] = -h * std::sin(heading);
                    d[1] = h * std::cos(heading);
                    const float yaw = std::remainder(heading - facing + g_armCorrYaw, 6.2831855f) +
                                      g_armYawBias.load(std::memory_order_relaxed);
                    // the pitch the other way round: the IK lowers the arm for a larger pitch here (its pitch is
                    // measured from the clavicle's Y axis, which points the other way in the drive-by clips)
                    const float pitch = -std::atan2(d[2], h) + g_armCorrPitch + g_armPitchBias.load(std::memory_order_relaxed);
                    std::memcpy(g_armWant, d, sizeof(d));
                    g_armFrame = now;
                    g_armCalling = true;
                    g_armPed = ped;
                    g_armRotation = rdf(ped + A.pedRotation);
                    wrf(ped + A.pedRotation, facing);
                    for (int i = 0; i < 6; ++i)
                        g_armLimitsSaved[i] = rdf(kArmLimits + i * 4);
                    wrf(kArmLimits, g_armYawMax.load(std::memory_order_relaxed));
                    wrf(kArmLimits + 4, g_armYawMin.load(std::memory_order_relaxed));
                    g_armA0 = vehicle;
                    g_armRa = GPR_U32(ctx, 31);
                    {
                        std::lock_guard<std::mutex> lock(g_reportMutex);
                        ++g_report.armCalls;
                        g_report.armYaw = yaw;
                        g_report.armPitch = pitch;
                        g_report.corr[0] = g_armCorrYaw;
                        g_report.corr[1] = g_armCorrPitch;
                        const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        for (int i = 0; i < 3; ++i)
                            g_report.want[i] = d[i] / n;
                    }
                    ctx->f[12] = yaw;
                    ctx->f[13] = pitch;
                    SET_GPR_U32(ctx, 4, ped + A.pedIk);
                    SET_GPR_U32(ctx, 31, entry);
                    ctx->pc = A.armIk;
                    return;
                }
            }
            g_origDriveBy[I](rdram, ctx, runtime);
        }

        bool off(const char *name)
        {
            const char *e = std::getenv(name);
            return e && e[0] == '0';
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (off("LCS_DRIVEBY_AIM"))
        {
            std::fprintf(stderr, "[lcs-driveby] drive-by aim off (LCS_DRIVEBY_AIM=0)\n");
            return;
        }
        g_bikeForward = !off("LCS_DRIVEBY_BIKE_FORWARD");
        g_crosshair = !off("LCS_DRIVEBY_CROSSHAIR");
        if (const char *e = std::getenv("LCS_DRIVEBY_LIFT"); e && *e)
            g_lift = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 3.0f);
        g_origCamUp = runtime.lookupFunction(A.camUp);
        g_origAutoAim = runtime.lookupFunction(A.autoAim);
        g_origAvoid = runtime.lookupFunction(A.avoid);
        if (!g_origCamUp || !g_origAutoAim || !g_origAvoid)
        {
            std::fprintf(stderr, "[lcs-driveby] a hooked function is missing: drive-by aim off\n");
            g_origCamUp = g_origAutoAim = g_origAvoid = nullptr;
            return;
        }
        runtime.replaceFunction(A.camUp, &camUp);
        runtime.replaceFunction(A.autoAim, &autoAim);
        runtime.replaceFunction(A.avoid, &avoid);
        g_arm = !off("LCS_DRIVEBY_ARM");
        if (g_arm)
        {
            for (int i = 0; i < 3; ++i)
                g_origDriveBy[i] = runtime.lookupFunction(A.driveBys[i]);
            if (g_origDriveBy[0] && g_origDriveBy[1] && g_origDriveBy[2] && runtime.lookupFunction(A.armIk))
            {
                runtime.replaceFunction(A.driveBys[0], &driveBy<0>);
                runtime.replaceFunction(A.driveBys[1], &driveBy<1>);
                runtime.replaceFunction(A.driveBys[2], &driveBy<2>);
            }
            else
                g_arm = false;
        }
        // lcsdrivebyarm [mode] [yaw bias] [pitch bias] -> ok mode=.. (tests: 0 the clip's arm, 1 the IK)
        ps2control::addCommand("lcsdrivebyarm", [](const std::string &args, ps2control::Reply reply) {
            int mode = -1;
            float yb = 0.0f, pb = 0.0f, ymax = 0.0f, ymin = 0.0f;
            const int n = std::sscanf(args.c_str(), "%d %f %f %f %f", &mode, &yb, &pb, &ymax, &ymin);
            if (n >= 1)
                g_armMode = mode;
            if (n >= 2)
                g_armYawBias = yb;
            if (n >= 3)
                g_armPitchBias = pb;
            if (n >= 5)
            {
                g_armYawMax = ymax;
                g_armYawMin = ymin;
            }
            Report r;
            {
                std::lock_guard<std::mutex> lock(g_reportMutex);
                r = g_report;
            }
            char text[1024];
            int len = std::snprintf(text, sizeof(text),
                                    "ok on=%d mode=%d calls=%u yaw=%.3f pitch=%.3f want=%.3f,%.3f,%.3f corr=%.3f,%.3f "
                                    "ik=%.3f,%.3f,%.3f,%.3f",
                                    g_arm ? 1 : 0, g_armMode.load(), r.armCalls, r.armYaw, r.armPitch, r.want[0], r.want[1],
                                    r.want[2], r.corr[0], r.corr[1], r.ik[0], r.ik[1], r.ik[2], r.ik[3]);
            // the bones as last drawn (rows X, Y, Z, position): clavicle, upper arm, forearm, hand (all right)
            const uint32_t ped = g_rdram ? playerPed() : 0;
            for (const uint32_t off : {0x2DCu, 0x2ACu, 0x2D4u, 0x2B4u})
                if (const uint32_t m = ped ? boneMatrix(ped, off) : 0)
                    len += std::snprintf(text + len, sizeof(text) - len,
                                         " b%x=%u:%.2f,%.2f,%.2f|%.2f,%.2f,%.2f|%.2f,%.2f,%.2f|%.2f,%.2f,%.2f", off, 0u, rdf(m),
                                         rdf(m + 4), rdf(m + 8), rdf(m + 16), rdf(m + 20), rdf(m + 24), rdf(m + 32), rdf(m + 36),
                                         rdf(m + 40), rdf(m + 48), rdf(m + 52), rdf(m + 56));
            reply(text);
        });
        if (g_crosshair)
        {
            g_origRender = runtime.lookupFunction(A.effectsRender);
            g_origFindPlayerPed = runtime.lookupFunction(A.findPlayerPed);
            if (g_origRender && g_origFindPlayerPed)
            {
                runtime.replaceFunction(A.effectsRender, &effectsRender);
                runtime.replaceFunction(A.findPlayerPed, &findPlayerPed);
            }
            else
                g_crosshair = false;
        }
        // lcsdriveby -> ok side=<0|1|2> look mode weapon vehicle yaw pitch shots bikeshots start=x,y,z dir=x,y,z
        // view=x,y,z (the view's forward when last aimed) cross=<0|1> pull=<m> fired hits
        ps2control::addCommand("lcsdriveby", [](const std::string &, ps2control::Reply reply) {
            Report r;
            {
                std::lock_guard<std::mutex> lock(g_reportMutex);
                r = g_report;
            }
            const uint32_t ped = g_rdram ? playerPed() : 0;
            const uint32_t cam = g_rdram ? activeCam() : 0;
            char text[480];
            std::snprintf(text, sizeof(text),
                          "ok side=%d look=%d mode=%u weapon=%d vehicle=0x%x yaw=%.3f pitch=%.3f shots=%u bikeshots=%u "
                          "start=%.2f,%.2f,%.2f dir=%.3f,%.3f,%.3f view=%.3f,%.3f,%.3f cross=%d pull=%.2f fired=%u hits=%u",
                          r.side, cam ? lookSide(cam) : 0, cam ? camMode(cam) : 0u, ped && driveByWeapon(ped) ? 1 : 0,
                          playerVehicle(ped), r.yaw, r.pitch, r.shots, r.bikeShots, r.start[0], r.start[1], r.start[2],
                          r.dir[0], r.dir[1], r.dir[2], r.viewDir[0], r.viewDir[1], r.viewDir[2], r.crosshair ? 1 : 0,
                          r.wallPull, rd32(A.statFired), rd32(A.statHits));
            reply(text);
        });
        std::fprintf(stderr, "[lcs-driveby] drive-by aim with the mouse / right stick installed%s%s\n",
                     g_crosshair ? ", aiming circle" : "", g_bikeForward ? "" : " (bike straight-ahead shots left alone)");
    }

    bool mouse(uint8_t *rdram, float turn, float tilt, bool paused)
    {
        if (!g_origCamUp || !rdram)
            return false;
        uint32_t cam = 0, ped = 0, vehicle = 0;
        const int side = aimSide(cam, ped, vehicle);
        if (!side)
            return false;
        if (paused)
            return true;
        startLook(side, frame());
        // left (+) turns the look counter-clockwise; the camera on the far side of the vehicle is turned the same way
        g_yaw = std::clamp(g_yaw + turn, -kYawLimit, kYawLimit);
        g_pitch = std::clamp(g_pitch + tilt, kPitchDown, kPitchUp);
        publish();
        return true;
    }

    void stick(float x, float y)
    {
        g_stickX.store(x, std::memory_order_relaxed);
        g_stickY.store(y, std::memory_order_relaxed);
    }

    // [android] the look camera was aimed within the last 150 ms (a few game frames even at 20 fps): a drive-by look
    bool aiming() { return hostMs() - g_lookHostMs.load(std::memory_order_relaxed) < 150; }
}
