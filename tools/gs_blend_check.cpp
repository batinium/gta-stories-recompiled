// [android] Host check of the GS blend's single-source form (ps2xRuntime src/lib/gs/gs_blend.h), no game, no GPU:
//
//   c++ -std=c++17 -O2 -Isrc -Ithird_party/PS2Recomp/ps2xRuntime/src/lib/gs tools/gs_blend_check.cpp -o build/gs_blend_check
//   build/gs_blend_check
//
// For every GS blend (A, B, C, D) gsBlend draws with dual-source blending, the shader output (gs_draw.hlsl ps_main's
// blend part) and the fixed-function blend of an RGBA8 target are modelled in float, once with the reference
// (dual-source) factors and once with the single-source ones (toSingleSource / singleSourceConstant), over every source
// colour, destination colour and C value (As, Ad or FIX: 0..255). Prints per combination the largest difference, in
// 8-bit steps, of the single-source result from the reference (colour and stored alpha) and, for information, of both
// from the PS2's own (A - B) * C >> 7 + D (clamped; the alpha the GS stores is As) where the renderer means to match it
// (C <= 0x80; Ad only at 0x80, as the shader takes Ad as 1.0). "q8" repeats the colour comparison
// with the shader's o0.a rounded to 8 bits before blending (a GPU that quantises the source to the target's format).
// Exit status 1 when a colour difference from the reference is not 0 (q8 at most 1 step).
#include "gs_blend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
    using F = rhi::BlendFactor;
    using O = rhi::BlendOp;

    struct Src
    {
        float rgb, a, a1; // o0.rgb (one channel), o0.a, the second output's alpha (dual)
    };

    float factor(F f, const Src &s, float dstA, const float k[4], bool alphaChannel)
    {
        switch (f)
        {
        case F::Zero: return 0.0f;
        case F::One: return 1.0f;
        case F::SrcAlpha: return s.a;
        case F::InvSrcAlpha: return 1.0f - s.a;
        case F::DstAlpha: return dstA;
        case F::InvDstAlpha: return 1.0f - dstA;
        case F::Src1Alpha: return s.a1;
        case F::InvSrc1Alpha: return 1.0f - s.a1;
        case F::Constant: return alphaChannel ? k[3] : k[0];
        case F::InvConstant: return 1.0f - (alphaChannel ? k[3] : k[0]);
        }
        return 1.0f;
    }

    float op(O o, float s, float d)
    {
        switch (o)
        {
        case O::Add: return s + d;
        case O::Subtract: return s - d;
        case O::ReverseSubtract: return d - s;
        case O::Min: return std::min(s, d);
        case O::Max: return std::max(s, d);
        }
        return s;
    }

    int unorm8(float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
    float sat(float v) { return std::clamp(v, 0.0f, 1.0f); } // UNORM targets clamp the source before blending

    // One channel and the alpha of an RGBA8 target after the blend: returns colour, alpha in 0..255
    void blend(const gsblend::GsBlend &b, Src s, int cd, int ad, const float k[4], int &outC, int &outA)
    {
        s.rgb = sat(s.rgb);
        s.a = sat(s.a);
        s.a1 = sat(s.a1);
        const float dc = cd / 255.0f, da = ad / 255.0f;
        if (!b.enable)
        {
            outC = unorm8(s.rgb);
            outA = unorm8(s.a);
            return;
        }
        outC = unorm8(op(b.op, s.rgb * factor(b.src, s, da, k, false), dc * factor(b.dst, s, da, k, false)));
        outA = unorm8(op(O::Add, s.a * factor(b.srcAlpha, s, da, k, true), da * factor(b.dstAlpha, s, da, k, true)));
    }

    // gs_draw.hlsl ps_main from `c` on: cf, premultiply, o0 and o.blend (DUAL_SOURCE 1), or C in o0.a (DUAL_SOURCE 2)
    Src shade(int cs, int as, int bc, int fix, int premul, bool cInAlpha)
    {
        const float cf = (bc == 0) ? as / 128.0f : ((bc == 2) ? fix / 128.0f : 1.0f);
        float rgb = static_cast<float>(cs);
        if (premul == 1) rgb *= cf;
        else if (premul == 2) rgb *= (1.0f + cf);
        Src s{rgb / 255.0f, as / 255.0f, std::min(cf, 1.0f)};
        if (cInAlpha)
            s.a = std::min(cf, 1.0f);
        return s;
    }

    int ps2(int A, int B, int D, int cs, int cd, int c)
    {
        auto pick = [&](int sel) { return sel == 0 ? cs : sel == 1 ? cd : 0; };
        const int v = (((pick(A) - pick(B)) * c) >> 7) + pick(D);
        return std::clamp(v, 0, 255); // COLCLAMP on
    }

    const char *kSel[] = {"Cs", "Cd", "0"};
    const char *kC[] = {"As", "Ad", "FIX"};
}

int main()
{
    int bad = 0;
    std::printf("%-22s %-9s %8s %8s %8s %10s %10s\n", "(A-B)*C+D", "C", "col-ref", "col-q8", "a-ref", "ref-PS2", "fall-PS2");
    for (int A = 0; A < 3; ++A)
        for (int B = 0; B < 3; ++B)
            for (int D = 0; D < 3; ++D)
                for (int bc = 0; bc < 3; ++bc)
                {
                    gsblend::GsBlend ref;
                    if (!gsblend::gsBlend(true, A, B, D, ref) || !gsblend::usesSrc1(ref))
                        continue; // not dual-source: drawn the same either way
                    gsblend::GsBlend single = ref;
                    gsblend::toSingleSource(single, bc);
                    if (gsblend::usesSrc1(single))
                    {
                        std::printf("toSingleSource left a Src1 factor\n");
                        return 1;
                    }
                    int maxCol = 0, maxColQ = 0, maxA = 0, maxRefPs2 = 0, maxFallPs2 = 0;
                    const float zero[4] = {0, 0, 0, 0};
                    // the swept value: As (C = As), Ad (C = Ad: As fixed at 0x80), FIX
                    for (int v = 0; v < 256; ++v)
                    {
                        const int as = bc == 0 ? v : 0x80, ad = bc == 1 ? v : 0x80, fix = bc == 2 ? v : 0x80;
                        const int c = bc == 0 ? as : bc == 1 ? ad : fix;
                        // the PS2 comparison where the renderer means to match it: C <= 0x80 (the factor is clamped to 1),
                        // Ad = 0x80 only (the shader takes Ad as 1.0: it cannot read the destination)
                        const bool inRange = c <= 0x80 && (bc != 1 || c == 0x80);
                        float k[4];
                        gsblend::singleSourceConstant(bc, static_cast<uint32_t>(fix), k);
                        for (int cs = 0; cs < 256; ++cs)
                        {
                            const Src sr = shade(cs, as, bc, fix, ref.premul, false);
                            const Src ss = shade(cs, as, bc, fix, single.premul, single.cInAlpha);
                            Src sq = ss;
                            sq.a = unorm8(sq.a) / 255.0f;
                            for (int cd = 0; cd < 256; ++cd)
                            {
                                int rc, ra, fc, fa, qc, qa;
                                blend(ref, sr, cd, ad, zero, rc, ra);
                                blend(single, ss, cd, ad, k, fc, fa);
                                blend(single, sq, cd, ad, k, qc, qa);
                                const int p = ps2(A, B, D, cs, cd, c);
                                maxCol = std::max(maxCol, std::abs(fc - rc));
                                maxColQ = std::max(maxColQ, std::abs(qc - rc));
                                maxA = std::max(maxA, std::abs(fa - ra));
                                if (inRange)
                                {
                                    maxRefPs2 = std::max(maxRefPs2, std::abs(rc - p));
                                    maxFallPs2 = std::max(maxFallPs2, std::abs(fc - p));
                                }
                            }
                        }
                    }
                    // stored alpha above 0x80 (C = As): what it becomes
                    char eq[32];
                    std::snprintf(eq, sizeof(eq), "(%s-%s)*C+%s", kSel[A], kSel[B], kSel[D]);
                    std::printf("%-22s %-9s %8d %8d %8d %10d %10d\n", eq, kC[bc], maxCol, maxColQ, maxA, maxRefPs2, maxFallPs2);
                    if (maxCol != 0 || maxColQ > 1)
                        bad = 1;
                }
    // the stored alpha of a C = As single-source draw, As <= 0x80 exact
    {
        gsblend::GsBlend b;
        gsblend::gsBlend(true, 0, 1, 1, b);
        gsblend::toSingleSource(b, 0);
        float k[4];
        gsblend::singleSourceConstant(0, 0, k);
        int firstWrong = -1;
        for (int as = 0; as < 256 && firstWrong < 0; ++as)
        {
            int c, a;
            blend(b, shade(0, as, 0, 0, b.premul, true), 0, 0, k, c, a);
            if (a != as)
                firstWrong = as;
        }
        std::printf("C = As single-source stored alpha: exact for As 0..%d, then 0x80\n", firstWrong < 0 ? 255 : firstWrong - 1);
    }
    std::printf(bad ? "FAIL\n" : "OK: colour identical to the reference for every combination\n");
    return bad;
}
