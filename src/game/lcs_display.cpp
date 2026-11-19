// [lcs] See lcs_display.h. The same route as VCS's widescreen (docs/SETTINGS_MENU_WIDESCREEN.md, game/vcs_display.cpp):
// - Hor+ / Vert+: LCS's view-window setter (the twin of VCS's FUN_001ab9e0: x = tan(FOV / 2), y = x / aspect, raster
//   640x448, RwCameraSetViewWindow) gets VCS's widened arguments (vcs::widenViewWindow), so RenderWare's frustum, the
//   sector culling and the renderer follow the display aspect.
// - Entity culling: LCS's CCamera::IsSphereVisible tests the same four camera-space planes as VCS's, built by the twin
//   of FUN_0037b2c8 from the FOV alone; they are widened / made taller by VCS's code (vcs::prepareSphereTest).
// - The game's own widescreen preference (FrontEndMenuManager.m_PrefsUseWideScreen, a byte) is the PS2's anamorphic
//   mode (aspect 16/9 = Vert-, the HUD laid out for a stretching TV, no cutscene letterbox), as in VCS: kept off.
// - The letterbox (CCamera::DrawBordersForWideScreen) follows Display > Cutscene black bars.
// Addresses: SLUS-21423 v1.01, found by matching against VCS's code (internal notes: LCS address report; the evidence
// is next to each value). VCS_WIDESCREEN=1 / =game and VCS_WIDE_CULL=0 work as for VCS (comparisons).
#include "game/lcs_display.h"

#include "game/game_table.h"
#include "game/lcs_hud.h"
#include "game/vcs_display.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::display
{
    namespace
    {
        struct Addrs
        {
            uint32_t viewWindow;   // fn (camera a0, rect a1, tan(FOV / 2) f12, aspect f13)
            uint32_t sphereTest;   // fn CCamera::IsSphereVisible(camera a0, centre a1, matrix a2, radius f12): a leaf
            uint32_t planes;       // CCamera: the right plane; left +0x10, bottom +0x20, top +0x30 (VCS +0xB60)
            uint32_t aspectChoice; // fn CDraw::CalculateAspectRatio: writes the aspect global from the preference
            uint32_t widePref;     // u8 the game's widescreen preference
            uint32_t borders;      // fn CCamera::DrawBordersForWideScreen(camera a0): the cutscene letterbox
        };
        constexpr Addrs kLcs{
            0x00113478u, // the only exact twin of VCS's 0x1AB9E0 (best.py ratio 1.0, 30 instructions, same 640 / 0x1C0
                         // raster default, div.s f12 / f13 into RwCameraSetViewWindow 0x100578); five callers like
                         // VCS's, the first (0x1F3F98) loads the FOV global x pi / 360 and the aspect global into f12 / f13
            0x00298D28u, // twin of VCS's 0x3816F8 (ratio 0.972, 72 instructions each): VU0 transform of the centre,
                         // near / far tests, then the four plane dot products with the radius
            0x1C20u,     // that test's lwc1 0x1C20 / 0x1C24, 0x1C30 / 0x1C34 (x, y), 0x1C44 / 0x1C48, 0x1C54 / 0x1C58
                         // (y, z); the builder 0x291E90 (twin of VCS's 0x37B2C8, ratio 0.76) aligns its swc1 0xB60 /
                         // 0xB70 / 0xB80 / 0xB90 to 0x1C20 / 0x1C30 / 0x1C40 / 0x1C50
            0x002083A8u, // called by the view-window caller 0x1F3F98 right before it reads the aspect (as VCS's 0x2653F0
                         // from 0x21CD20); stores 0 then 16/9 (0x3FE38E39) or 4/3 (0x3FAAAAAB) to the aspect global
                         // 0x3D540C by the byte below - VCS's also tests the prefs object and m_WideScreenOn
            0x003D8B8Au, // that byte (lbu); read by ~170 places (the letterbox below, the frontend 0x23F438 ...),
                         // written by the frontend (0x241F6C) and the save / settings load (0x339AB8)
            0x0028CF08u, // twin of VCS's 0x2804C8 (ratio 0.72): returns at once when the byte above is set, else two
                         // black rects 480 wide through the rect fill 0x3219C0 (VCS 0x3E0B10, exact twin), heights from
                         // the camera's +0x1D4 (the bars' percentage) x 1.36 - the top one from 0, the bottom one to 272
        };

        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kVcsPlanes = 0xB60u; // vcs::prepareSphereTest's planes: camera + 0xB60 .. 0xB9C

        game::Fn g_viewWindow, g_sphereTest, g_aspectChoice, g_borders;
        uint32_t g_widePref = 0;
        uint32_t g_planeShift = 0;
        std::atomic<int> g_gamePreference{0}; // the value the game's preference is kept at; -1: left to the game

        // the preference kept at g_gamePreference (before every aspect choice: the frontend, a loaded save or the
        // settings file may have changed it)
        void keepPreference(uint8_t *rdram)
        {
            const int wanted = g_gamePreference.load(std::memory_order_relaxed);
            uint8_t &pref = rdram[g_widePref & kRamMask];
            if (wanted < 0 || pref == static_cast<uint8_t>(wanted))
                return;
            static bool logged = false;
            if (!logged)
                std::fprintf(stderr, "[lcs-display] the game set its widescreen preference %u: kept at %d\n", pref, wanted);
            logged = true;
            pref = static_cast<uint8_t>(wanted);
        }

        PS2_GUEST_CC void viewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            vcs::widenViewWindow(ctx);
            g_viewWindow(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void sphereTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // VCS's plane code reads camera + 0xB60.., so it gets a camera address shifted by the layouts' difference
            // (it keeps its per-camera state by that address; nothing else is read through it)
            vcs::prepareSphereTest(rdram, GPR_U32(ctx, 4) + g_planeShift);
            g_sphereTest(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void aspectChoice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            keepPreference(rdram);
            g_aspectChoice(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void borders(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!vcs::cutsceneBars())
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_borders(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const Addrs &A = kLcs;
        g_viewWindow.bind(A.viewWindow);
        g_sphereTest.bind(A.sphereTest);
        g_aspectChoice.bind(A.aspectChoice);
        g_borders.bind(A.borders);
        g_widePref = A.widePref;
        g_planeShift = A.planes - kVcsPlanes;
        runtime.replaceFunction(A.viewWindow, &viewWindow);
        if (const char *e = std::getenv("VCS_WIDE_CULL"); !(e && *e == '0'))
            runtime.replaceFunction(A.sphereTest, &sphereTest);
        if (const char *e = std::getenv("VCS_WIDESCREEN"); e && *e) // testing: the PS2's own (Vert-, anamorphic) mode
            g_gamePreference.store(std::strcmp(e, "game") == 0 ? -1 : *e == '0' ? 0 : 1, std::memory_order_relaxed);
        runtime.replaceFunction(A.aspectChoice, &aspectChoice);
        runtime.replaceFunction(A.borders, &borders);
        lcs::hud::install(runtime); // the HUD's side (game/lcs_hud.h)
        std::fprintf(stderr, "[lcs-display] Hor+ / Vert+ view window, culling planes, widescreen preference %d\n",
                     g_gamePreference.load(std::memory_order_relaxed));
    }
}
