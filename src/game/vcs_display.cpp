// VCS display settings applied to the game (docs/SETTINGS_MENU_WIDESCREEN.md).
//
// Widescreen is Hor+: FUN_001ab9e0 (sets the camera's view window: x = tan(FOV / 2), y = x / aspect, then
// RwCameraSetViewWindow) is wrapped so the horizontal extent grows by r = display aspect / (4:3) while the vertical
// one stays (both arguments times r). RenderWare derives the frustum (EE culling) and the VU matrices from the view
// window, so the 3D, the sector culling and both renderers follow; the picture (still 640 wide) is presented at the
// wider aspect and the renderer keeps the HUD at 4:3 (gsHwSetHudSqueeze).
//
// Narrower than 4:3 (r < 1) it is Vert+: the horizontal extent stays and the vertical one grows by 1 / r (the aspect
// argument times r alone), the picture is shown taller and the HUD is squeezed up and down (gsHwSetHudSqueezeVertical).
//
// Entities are culled by the game's own camera planes, which do not come from the view window:
// CCamera::IsSphereVisible (FUN_003816F8) tests camera-space side planes that FUN_0037b2c8 builds from the FOV global
// alone (right (cos h, -sin h) at cam+0xB60, left (-cos h, -sin h) at +0xB70, h = FOV / 2: the 4:3 view). Without a
// fix buildings, peds and cars at the screen edges vanish in widescreen. The test is wrapped: before it runs, side
// planes the game has rebuilt are widened to the Hor+ view (edge at tan h x r). VCS_WIDE_CULL=0: the game's planes.
// Vert+: the bottom / top planes (+0xB80 / +0xB90, (0, -sin v, -+cos v), v = 0.7 h: inside the 4:3 view's top and
// bottom edges) are moved out instead, to the taller view's edge (tan v / r, at least the view window's own edge).
//
// The game's own widescreen preference (pause menu Display, option 14; *(*0x48F3D0 + 0x30)) is the PS2's anamorphic
// mode for a TV that stretches the picture: a Vert- 16:9 aspect, the HUD, text, pause map and menu layouts squeezed to
// 3/4 width, and no cutscene letterbox (game-internals.md 5a). On a PC every part of it but the bars fights the Hor+
// view and the renderer's HUD squeeze, so it is kept off (the user's decision, DECISIONS.md 2026-11-06): its setter
// FUN_002AF660 stores 0 - the pause menu's row is hidden (vcs_frontend.cpp) and a save's 1 (block +0x80, restored
// at 0x3AD0F8) is not taken - and the frame kick puts 0 back if anything wrote the field directly. The bars are a
// setting of their own (Display > Cutscene black bars): FUN_002804C8, which draws only them, is skipped when off.
// VCS_WIDESCREEN=1 forces the PS2's mode on (comparisons), VCS_WIDESCREEN=game leaves it to the game (before).
#include "game/vcs_display.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include "game/game_functions.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kPrefsPointer = 0x0048F3D0u; // -> the game's preferences / menu object
        constexpr uint32_t kWidescreenField = 0x30u;    // int: 1 = the game's (Vert-) widescreen; FUN_002af658 reads it
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        constexpr uint32_t kRightPlane = 0xB60u; // CCamera: camera-space frustum normals, x right, y forward, z up
        constexpr uint32_t kLeftPlane = 0xB70u;
        constexpr uint32_t kBottomPlane = 0xB80u; // (0, -sin v, -cos v); the top one (0, -sin v, cos v) at +0xB90
        constexpr uint32_t kTopPlane = 0xB90u;

        std::atomic<float> g_widening{1.0f};
        float g_viewTanY = 0.0f; // Vert+: the last view window's vertical extent (tan of half the vertical view); game thread
        std::atomic<int> g_gamePreference{0}; // the value the game's preference is kept at; -1: left to the game
        std::atomic<bool> g_cutsceneBars{true};
        bool g_wideCull = true;

        // Per camera object, its side planes as the game built them and as they were widened (game thread only: the
        // camera's FUN_0037b2c8 and every IsSphereVisible run there). In practice one camera (TheCamera, 0x6F44D0).
        struct SidePlanes
        {
            uint32_t camera = 0;
            float r = 1.0f;
            float game[4] = {}; // right x, y, left x, y
            float wide[4] = {};
            // Vert+: the bottom / top planes (y, z of each) as the game built them and as moved out
            float gameV[4] = {};
            float tallV[4] = {};
            bool vertical = false; // tallV holds moved planes (Vert+)
        };
        SidePlanes g_planes[4];
        unsigned g_nextPlanes = 0;

        uint32_t load32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }

        void store32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

    }

    // [lcs] extracted from setViewWindow below, so LCS's twin of the setter (game/lcs_display.cpp) widens the same way
    void widenViewWindow(R5900Context *ctx)
    {
        const float r = g_widening.load(std::memory_order_relaxed);
        if (r > 1.0001f)
        {
            ctx->f[12] *= r; // horizontal extent x r
            ctx->f[13] *= r; // vertical extent (x / aspect) unchanged
        }
        else if (r < 0.9999f && r > 0.0f)
        {
            ctx->f[13] *= r; // Vert+: horizontal extent unchanged, vertical extent (x / aspect) / r
            if (ctx->f[13] > 0.0f)
                g_viewTanY = ctx->f[12] / ctx->f[13];
        }
    }

    namespace
    {
        // FUN_001ab9e0(camera a0, raster rect a1 (null: 640x448), tan(FOV / 2) f12, aspect f13)
        PS2_GUEST_CC void setViewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            widenViewWindow(ctx);
            VCS_GAME_FN(FUN_001ab9e0_0x1ab9e0, 0x001AB9E0u)(rdram, ctx, runtime);
        }

        float loadFloat(const uint8_t *rdram, uint32_t addr)
        {
            float v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }

        void storeFloat(uint8_t *rdram, uint32_t addr, float v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

        // A side plane (a, b) = (+-cos h, -sin h) has its edge at x / y = tan h; the Hor+ edge is at tan h x r:
        // (a, r b), normalised (the test compares the distance with the sphere's radius).
        void widenPlane(const float *game, float r, float *wide)
        {
            const float a = game[0], b = game[1] * r;
            const float len = std::sqrt(a * a + b * b);
            wide[0] = len > 0.0f ? a / len : game[0];
            wide[1] = len > 0.0f ? b / len : game[1];
        }

        // Vert+: a bottom / top plane (b, c) = (-sin v, -+cos v) has its edge at |z| / y = tan v; moved out to tanV:
        // (b x tan v / tanV, c), normalised
        void tallPlane(const float *game, float tanV, float *tall)
        {
            const float b = game[0], c = game[1];
            const float edge = std::fabs(c) > 1e-6f ? std::fabs(b / c) : 0.0f;
            if (!(edge > 0.0f) || !(tanV > edge))
            {
                tall[0] = b;
                tall[1] = c;
                return;
            }
            const float nb = b * (edge / tanV);
            const float len = std::sqrt(nb * nb + c * c);
            tall[0] = nb / len;
            tall[1] = c / len;
        }

        // Vert+ on this camera's bottom / top planes (prepareSphereTest's p): moved out to the taller view's edge, or
        // put back when the picture is 4:3 or wider again
        void prepareVertical(uint8_t *rdram, uint32_t camera, SidePlanes &p, float r, bool known)
        {
            const float now[4] = {loadFloat(rdram, camera + kBottomPlane + 4), loadFloat(rdram, camera + kBottomPlane + 8),
                                  loadFloat(rdram, camera + kTopPlane + 4), loadFloat(rdram, camera + kTopPlane + 8)};
            const bool ours = known && p.vertical && std::memcmp(now, p.tallV, sizeof(now)) == 0;
            if (!ours)
                std::memcpy(p.gameV, now, sizeof(now)); // rebuilt by the game (or never touched): its planes
            if (r >= 0.9999f)
            {
                if (ours) // back to the game's
                {
                    storeFloat(rdram, camera + kBottomPlane + 4, p.gameV[0]);
                    storeFloat(rdram, camera + kBottomPlane + 8, p.gameV[1]);
                    storeFloat(rdram, camera + kTopPlane + 4, p.gameV[2]);
                    storeFloat(rdram, camera + kTopPlane + 8, p.gameV[3]);
                }
                p.vertical = false;
                return;
            }
            // the game's edge (tan v) taller by 1 / r, and never inside the view window's own edge
            const float c = std::fabs(p.gameV[1]) > 1e-6f ? std::fabs(p.gameV[0] / p.gameV[1]) : 0.0f;
            const float tanV = std::max(c / r, g_viewTanY);
            float tall[4];
            tallPlane(p.gameV, tanV, tall);
            tallPlane(p.gameV + 2, tanV, tall + 2);
            if (ours && std::memcmp(tall, p.tallV, sizeof(tall)) == 0)
                return;
            std::memcpy(p.tallV, tall, sizeof(tall));
            p.vertical = true;
            storeFloat(rdram, camera + kBottomPlane + 4, tall[0]);
            storeFloat(rdram, camera + kBottomPlane + 8, tall[1]);
            storeFloat(rdram, camera + kTopPlane + 4, tall[2]);
            storeFloat(rdram, camera + kTopPlane + 8, tall[3]);
        }
    }

    void prepareSphereTest(uint8_t *rdram, uint32_t camera)
    {
        if (!g_wideCull)
            return;
        const float r = g_widening.load(std::memory_order_relaxed);
        SidePlanes *found = nullptr;
        for (SidePlanes &e : g_planes)
            if (e.camera == camera)
                found = &e;
        if (r < 1.0f || (found && found->vertical)) // Vert+, or just back from it: the bottom / top planes
        {
            if (!found)
            {
                found = &g_planes[g_nextPlanes++ % 4u];
                *found = SidePlanes{};
                found->camera = camera;
                prepareVertical(rdram, camera, *found, r, false);
            }
            else
                prepareVertical(rdram, camera, *found, r, true);
        }
        const float rw = std::max(1.0f, r); // the side planes: Hor+ only (Vert+ keeps the 4:3 view's width)
        if (rw == 1.0f && (!found || found->r == 1.0f))
            return; // 4:3: the game's planes
        float now[4] = {loadFloat(rdram, camera + kRightPlane), loadFloat(rdram, camera + kRightPlane + 4),
                        loadFloat(rdram, camera + kLeftPlane), loadFloat(rdram, camera + kLeftPlane + 4)};
        const bool ours = found && std::memcmp(now, found->wide, sizeof(now)) == 0;
        if (ours && found->r == rw)
            return; // widened already (the game rebuilds them in FUN_0037b2c8, once per camera update)
        SidePlanes &p = found ? *found : g_planes[g_nextPlanes++ % 4u];
        if (!ours)
            std::memcpy(p.game, now, sizeof(now)); // rebuilt by the game: the 4:3 planes
        // else: the ratio changed since they were widened (r = 1: the game's planes go back)
        p.camera = camera;
        p.r = rw;
        if (rw == 1.0f)
            std::memcpy(p.wide, p.game, sizeof(p.wide));
        else
        {
            widenPlane(p.game, rw, p.wide);
            widenPlane(p.game + 2, rw, p.wide + 2);
        }
        storeFloat(rdram, camera + kRightPlane, p.wide[0]);
        storeFloat(rdram, camera + kRightPlane + 4, p.wide[1]);
        storeFloat(rdram, camera + kLeftPlane, p.wide[2]);
        storeFloat(rdram, camera + kLeftPlane + 4, p.wide[3]);
    }

    namespace
    {
        // CCamera::IsSphereVisible(camera a0, centre a1, camera matrix a2, radius f12): a leaf
        PS2_GUEST_CC void sphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepareSphereTest(rdram, GPR_U32(ctx, 4));
            VCS_GAME_FN(FUN_003816f8_0x3816f8, 0x003816F8u)(rdram, ctx, runtime);
        }

        // FUN_002AF660(prefs a0, value a1): the widescreen preference's setter (a leaf: sw a1, 0x30(a0)). Callers: the
        // pause menu's Display page (0x2AF06C) and the save load (0x3AD0F8).
        PS2_GUEST_CC void setWidescreenPreference(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int wanted = g_gamePreference.load(std::memory_order_relaxed);
            if (wanted >= 0 && static_cast<int>(GPR_U32(ctx, 5)) != wanted)
            {
                static bool logged = false;
                if (!logged)
                    std::fprintf(stderr, "[display] the game sets its widescreen preference %u (ra=0x%x): kept at %d\n",
                                 GPR_U32(ctx, 5), GPR_U32(ctx, 31), wanted);
                logged = true;
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(wanted));
            }
            VCS_GAME_FN(FUN_002af660_0x2af660, 0x002AF660u)(rdram, ctx, runtime);
        }

        // FUN_002804C8(cutscene manager a0): the cutscene letterbox - two black bars, 0..34 and 238..272 of the 2D pass's
        // 272, while the manager's +0x34 is set and the widescreen preference is off. Draws nothing else.
        PS2_GUEST_CC void cutsceneBars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_cutsceneBars.load(std::memory_order_relaxed))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            VCS_GAME_FN(FUN_002804c8_0x2804c8, 0x002804C8u)(rdram, ctx, runtime);
        }
    }

    void setCutsceneBars(bool on) { g_cutsceneBars.store(on, std::memory_order_relaxed); }
    bool cutsceneBars() { return g_cutsceneBars.load(std::memory_order_relaxed); } // [lcs]

    void setAspectWidening(float r) { g_widening.store(r, std::memory_order_relaxed); }

    float aspectWidening() { return g_widening.load(std::memory_order_relaxed); }

    // The reflection map is the 128x128 copy of the frame into FBP 420 that car paint samples (game-internals.md 6g): a
    // flat panel shows one of its texels, so the GS's four taps out of ~5 x 3.5 frame pixels make the panel jump as the
    // camera moves. Smooth reads the copy through the renderer's filter (gsHwSetTargetFilter); the bloom at FBP 420 is
    // 256 wide (FBW 4), so only the reflection map's draws are filtered.
    void setReflectionFilter(int mode) { gsHwSetTargetFilter(420u, 2u, mode == 1 ? 1 : mode == 2 ? 2 : 0); }

    void installDisplayHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x001AB9E0u, &setViewWindow);
        if (const char *e = std::getenv("VCS_WIDE_CULL"); e && *e == '0')
            g_wideCull = false;
        runtime.replaceFunction(0x003816F8u, &sphereVisible); // VCS_TRACE_VISIBLE replaces it again, calling prepareSphereTest
        if (const char *e = std::getenv("VCS_WIDESCREEN"); e && *e) // testing: the PS2's own (Vert-, anamorphic) mode
            g_gamePreference.store(std::strcmp(e, "game") == 0 ? -1 : *e == '0' ? 0 : 1, std::memory_order_relaxed);
        if (g_gamePreference.load(std::memory_order_relaxed) >= 0)
            runtime.replaceFunction(0x002AF660u, &setWidescreenPreference);
        runtime.replaceFunction(0x002804C8u, &cutsceneBars);
        // VCS's render targets by what they are (game-internals.md 6g; docs/FULLY_NATIVE.md step 3d). The reflection map
        // is only ever sampled (vehicles' paint), so it follows "Internal resolution: Match window" like the picture.
        gsHwSetTargetRole(140u, 10u, "frame", false);
        gsHwSetTargetRole(0u, 10u, "shown picture (the previous frame)", false);
        gsHwSetTargetRole(280u, 10u, "Z buffer", false);
        gsHwSetTargetRole(420u, 2u, "reflection map (128x128 sample of the frame + sun glow)", true);
        gsHwSetTargetRole(420u, 4u, "bloom", false);
        gsHwSetTargetRole(452u, 4u, "bloom (second buffer)", false);
        gsHwSetTargetRole(384u, 8u, "trails", false);
        // The flip (FUN_003706c8) reads GS CSR at 0x370714 for the FIELD bit alone ($s4 = (CSR >> 13) & 1, the rest of
        // the value is overwritten in the next delay slot). FIELD is set at VBlank on the game thread: no need to wait
        // for the render worker to finish the frame's GS work (that fence was ~25% of the game thread uncapped,
        // game-internals.md 6). VCS_CSR_FENCE=1: fence as before.
        if (!std::getenv("VCS_CSR_FENCE"))
            runtime.addCsrReadWithoutFence(0x00370714u);
    }

    void applyDisplay(uint8_t *rdram)
    {
        const int wanted = g_gamePreference.load(std::memory_order_relaxed);
        if (wanted < 0)
            return;
        const uint32_t prefs = load32(rdram, kPrefsPointer);
        if (prefs == 0u || (prefs & kRamMask) + kWidescreenField + 4u > 0x02000000u)
            return; // the game has not created its preferences yet
        if (static_cast<int>(load32(rdram, prefs + kWidescreenField)) != wanted)
            store32(rdram, prefs + kWidescreenField, static_cast<uint32_t>(wanted)); // FUN_002653f0 picks the aspect up
    }
}
