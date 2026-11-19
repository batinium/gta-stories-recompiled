// [lcs] See lcs_freecam.h. The flying is vcs_freecam.cpp's (same controls, speeds and limits); the addresses are LCS's.
#include "game/lcs_freecam.h"

#include "game/dualworld.h" // [dualworld]

#include "game/lcs_trainer.h"
#include "game/vcs_input.h"
#include "game/vcs_trainer.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_host_std.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace lcs::freecam
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            // TheCamera (lcs_camera.cpp): CCamera::Process 0x28FAC8 indexes Cams by the byte +0x10C (0x28FCAC) times
            // 0x2B0 from +0x220 (0x28FCB8)
            uint32_t theCamera = 0x0043BBF0u;
            uint32_t activeCam = 0x10Cu;
            uint32_t cams = 0x220u;
            uint32_t camSize = 0x2B0u;
            // CCam::ProcessSpecialHeightRoutines: the only caller is CCamera::Process, right after CCam::Process
            // (0x28FCBC, then 0x28FCD0 with the same CCam); it starts from TheCamera's target entity (+0xBA4) and lifts
            // the follow camera over peds (VCS's FUN_002A2ED0 in the same place)
            uint32_t heightRoutines = 0x00287020u;
        };
        constexpr Addrs A{};
        // CCam fields: CCamera::Process copies Source (TheCamera + 0x3F0 = cam + 0x1D0) and Up (+0x410 = cam + 0x1F0)
        // into the frame's camera matrix (0x28FE34..0x28FE88) and Front (+0x3E0 = cam + 0x1C0) beside Up (0x290098..);
        // FOV at cam + 0xE8 (+0x308, 0x2900F4; the scope zoom writes it: lcs_camera.cpp); the target point +0x160
        // (lcs_camera notes: the ped's position + 0.6 z on foot)
        constexpr uint32_t kCamFront = 0x1C0u, kCamSource = 0x1D0u, kCamUp = 0x1F0u, kCamFov = 0xE8u, kCamTarget = 0x160u;

        constexpr float kTurnRate = 2.5f;  // radians a second, the stick at full tilt (as VCS's)
        constexpr float kZoomRate = 40.0f; // degrees a second
        constexpr float kPitchLimit = 1.55f;

        PS2Runtime::RecompiledFunction g_origHeight = nullptr;
        std::atomic<bool> g_installed{false};

        // game thread
        bool g_flying = false;
        float g_x = 0, g_y = 0, g_z = 0, g_yaw = 0, g_pitch = 0, g_fov = 70;
        std::chrono::steady_clock::time_point g_last{};

        std::mutex g_poseMutex;
        vcs::freecam::Pose g_pose;
        vcs::freecam::Pose g_jump; // setPose: taken by the next update (active = pending)

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        void wrVec(uint8_t *ram, uint32_t a, float x, float y, float z)
        {
            wrf(ram, a, x);
            wrf(ram, a + 4, y);
            wrf(ram, a + 8, z);
        }

        bool activeCam(const uint8_t *ram, uint32_t cam)
        {
            return cam == A.theCamera + A.cams + ram[(A.theCamera + A.activeCam) & kRamMask] * A.camSize;
        }

        void begin(const uint8_t *ram, uint32_t cam)
        {
            g_x = rdf(ram, cam + kCamSource);
            g_y = rdf(ram, cam + kCamSource + 4);
            g_z = rdf(ram, cam + kCamSource + 8);
            const float fx = rdf(ram, cam + kCamFront), fy = rdf(ram, cam + kCamFront + 4), fz = rdf(ram, cam + kCamFront + 8);
            g_yaw = std::atan2(fy, fx);
            g_pitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -kPitchLimit, kPitchLimit);
            const float fov = rdf(ram, cam + kCamFov);
            g_fov = fov >= 10.0f && fov <= 130.0f ? fov : 70.0f;
            g_last = ps2x::hostNow();
            g_flying = true;
            std::fprintf(stderr, "[lcs-freecam] on at (%.1f, %.1f, %.1f)\n", g_x, g_y, g_z);
        }

        void end()
        {
            if (!g_flying)
                return;
            g_flying = false;
            std::lock_guard<std::mutex> lock(g_poseMutex);
            g_pose.active = false;
            std::fprintf(stderr, "[lcs-freecam] off\n");
        }

        void update(uint8_t *ram, uint32_t cam)
        {
            const auto now = ps2x::hostNow();
            const float dt = std::clamp(std::chrono::duration<float>(now - g_last).count(), 0.0f, 0.1f);
            g_last = now;
            const vcs::input::FreeCamInput in = vcs::input::takeFreeCameraInput();
            {
                std::lock_guard<std::mutex> lock(g_poseMutex);
                if (g_jump.active)
                {
                    g_x = g_jump.x, g_y = g_jump.y, g_z = g_jump.z;
                    g_yaw = g_jump.yaw, g_pitch = std::clamp(g_jump.pitch, -kPitchLimit, kPitchLimit);
                    g_fov = std::clamp(g_jump.fov, 20.0f, 110.0f);
                    g_jump.active = false;
                }
            }

            g_yaw = std::remainder(g_yaw + in.turn + in.turnRate * kTurnRate * dt, 6.2831855f);
            g_pitch = std::clamp(g_pitch + in.tilt + in.tiltRate * kTurnRate * dt, -kPitchLimit, kPitchLimit);
            g_fov = std::clamp(g_fov - in.zoom * kZoomRate * dt, 20.0f, 110.0f);

            const float cy = std::cos(g_yaw), sy = std::sin(g_yaw), cp = std::cos(g_pitch), sp = std::sin(g_pitch);
            const float front[3] = {cp * cy, cp * sy, sp};
            const float right[3] = {sy, -cy, 0.0f};
            const float up[3] = {-cy * sp, -sy * sp, cp};
            const float speed = static_cast<float>(vcs::trainer::freeCameraSpeed()) * (in.fast ? 4.0f : 1.0f) *
                                (in.slow ? 0.25f : 1.0f);
            const float step = speed * dt;
            g_x += (front[0] * in.forward + right[0] * in.strafe) * step;
            g_y += (front[1] * in.forward + right[1] * in.strafe) * step;
            g_z += (front[2] * in.forward + in.rise) * step;

            wrVec(ram, cam + kCamSource, g_x, g_y, g_z);
            wrVec(ram, cam + kCamFront, front[0], front[1], front[2]);
            wrVec(ram, cam + kCamUp, up[0], up[1], up[2]);
            wrVec(ram, cam + kCamTarget, g_x + front[0], g_y + front[1], g_z + front[2]);
            wrf(ram, cam + kCamFov, g_fov);

            std::lock_guard<std::mutex> lock(g_poseMutex);
            g_pose = {true, g_x, g_y, g_z, g_yaw, g_pitch, g_fov};
        }

        PS2_GUEST_CC void camHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t cam = GPR_U32(ctx, 4);
            if (ctx->pc != A.heightRoutines || !activeCam(rdram, cam))
                return g_origHeight(rdram, ctx, runtime);
            if (dualworld::backCamera(rdram, cam)) // [dualworld] the background game's camera follows the front game's
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (!flying())
            {
                end();
                return g_origHeight(rdram, ctx, runtime);
            }
            if (!g_flying)
                begin(rdram, cam);
            update(rdram, cam);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_FREECAM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs] free camera off (LCS_FREECAM=0)\n");
            return;
        }
        g_origHeight = runtime.lookupFunction(A.heightRoutines);
        if (!g_origHeight)
            return;
        runtime.replaceFunction(A.heightRoutines, &camHeight);
        g_installed = true;
    }

    bool installed() { return g_installed.load(std::memory_order_relaxed); }

    bool flying()
    {
        return installed() && lcs::trainer::toggle(vcs::trainer::FreeCamera) && lcs::trainer::ready();
    }

    vcs::freecam::Pose pose()
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        return g_pose;
    }

    void setPose(float x, float y, float z, float yaw, float pitch, float fov)
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        g_jump = {true, x, y, z, yaw, pitch, fov};
    }
}
