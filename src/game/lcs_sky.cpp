// Liberty City Stories: the sky backdrop's fill below the horizon (docs/game-internals.md 6p, docs/LCS.md "Fog specks").
//
// CClouds::RenderBackground (FUN_0016C008) first fills the whole screen with (32,32,32) - `CRGBA(0x20, 0x20, 0x20,
// 0xFF)` built by the CRGBA constructor FUN_002B9CF8 at 0x16C27C (a1..a3 = 0x20 at 0x16C26C..0x16C274, ra 0x16C284),
// the rect 0,0-640,448 by FUN_0025B300, drawn by FUN_00321A90 - then the sky gradient down to the horizon and a 4-pixel
// band under it (0x16C3E0: 4.0). Nothing else of the backdrop is below the horizon: the sea and the world cover it.
// In fog (far plane 116 m) the world's far edge leaves that fill visible through the transparent texels of distant
// objects (lattice masts, railings, trees: no alpha test, ATST ALWAYS, so they still write depth and the land behind
// them fails its depth test) and in gaps between the far meshes: dark specks in the haze, fogged colour everywhere
// around them. Seen the same with the VU1 path (PS2X_HLE=0), LCS_RENDER=0 and PS2X_HW_POSTFX=0 (agent "visual" F12
// dumps, 2026-11-18): the game's own picture, not a renderer bug. Fix: the fill takes the timecycle's current fog
// colour (what every fogged pixel converges to), so the holes read as haze; and in fog the horizon band below is left
// out (the dashes on the horizon line). Other weathers: the same holes (rare with the far plane past 1 km) show the
// horizon's colour instead of dark grey. LCS_SKY_FILL=0: the PS2's (32,32,32) and band.
#include "game/lcs_sky.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kCrgbaCtor = 0x002B9CF8u; // CRGBA::CRGBA(r, g, b, a): sb a1..t0 into a0[0..3], returns a0
    constexpr uint32_t kFillRet = 0x0016C284u;   // its call for RenderBackground's full-screen fill (jal at 0x16C27C)
    constexpr uint32_t kTimecycle = 0x003D4AE8u; // CTimeCycle object pointer (gp-0x2408; 0x40D520 in a run)
    constexpr uint32_t kFogRgb = 0x2B54u;        // its current fog / horizon colour, ints R, G, B (CTimeCycle::Update
                                                 // 0x1B7F60; the GS FOGCOL the F12 log shows as fogCol)
    constexpr uint32_t kRamMask = 0x01FFFFFFu;

    // CClouds::RenderHorizon (FUN_0016C5A0) then draws a band from the horizon 36 pixels down with CSprite2d::DrawRect
    // (FUN_00321B08, four colour pointers a0..a3): (top + 2 bottom) / 3 at alpha 230 -> 80 over 4 pixels (jal 0x16C644),
    // then that colour -> (100,100,100) alpha 0 (jal 0x16C6D0; a third call at 0x16C740 only when 0x3E9703's alpha is
    // set, never: 0x16C6A0 clears it). Its RGB reaches the GS halved (F12: 0x59 for a 179 fog), so in fog it is a dark
    // grey strip at the horizon - covered by the world except, again, through the far edge's gaps: the dark dashes on
    // the horizon line. In clear weather it is the thin horizon line over the far sea, part of the look: so it is left
    // out only while the timecycle's far clip (obj +0x2B8C, passed to the far-plane setter at 0x1F5DDC) is short, i.e.
    // the horizon is inside the fog anyway (foggy 116 m; sunny noon 1,168, midnight 859).
    constexpr uint32_t kDrawRect = 0x00321B08u;
    constexpr uint32_t kHorizonRet[] = {0x0016C64Cu, 0x0016C6D8u, 0x0016C748u};
    constexpr uint32_t kFarClip = 0x2B8Cu;
    constexpr float kFogFar = 400.0f;

    PS2Runtime::RecompiledFunction g_origCtor = nullptr, g_origDrawRect = nullptr;

    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + (a & kRamMask), 4);
        return v;
    }

    PS2_GUEST_CC void drawRect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        for (uint32_t r : kHorizonRet)
            if (ra == r)
            {
                const uint32_t tc = rd32(rdram, kTimecycle);
                float farClip = 0.0f;
                if (tc != 0u && tc < 0x02000000u)
                    std::memcpy(&farClip, rdram + ((tc + kFarClip) & kRamMask), 4);
                if (farClip > 0.0f && farClip < kFogFar)
                {
                    ctx->pc = ra; // in fog: the band is not drawn
                    return;
                }
                break;
            }
        g_origDrawRect(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void crgbaCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (GPR_U32(ctx, 31) == kFillRet)
        {
            const uint32_t tc = rd32(rdram, kTimecycle);
            if (tc != 0u && tc < 0x02000000u)
            {
                const uint32_t r = rd32(rdram, tc + kFogRgb), g = rd32(rdram, tc + kFogRgb + 4u), b = rd32(rdram, tc + kFogRgb + 8u);
                if (r <= 255u && g <= 255u && b <= 255u)
                {
                    SET_GPR_U32(ctx, 5, r);
                    SET_GPR_U32(ctx, 6, g);
                    SET_GPR_U32(ctx, 7, b);
                }
            }
        }
        g_origCtor(rdram, ctx, runtime);
    }
}

namespace lcs::sky
{
    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_SKY_FILL"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs-sky] below-horizon fill: the PS2's dark grey (LCS_SKY_FILL=0)\n");
            return;
        }
        if (!(g_origCtor = runtime.lookupFunction(kCrgbaCtor)) || !(g_origDrawRect = runtime.lookupFunction(kDrawRect)))
        {
            std::fprintf(stderr, "[lcs-sky] 0x%x / 0x%x not found: below-horizon fill unchanged\n", kCrgbaCtor, kDrawRect);
            return;
        }
        runtime.replaceFunction(kCrgbaCtor, &crgbaCtor);
        runtime.replaceFunction(kDrawRect, &drawRect);
        std::fprintf(stderr, "[lcs-sky] below-horizon fill in the fog colour, no horizon band in fog\n");
    }
}
