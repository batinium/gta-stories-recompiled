// [gta-recomp] [android] The GS blend (A - B) * C / 128 + D as a fixed-function blend: gsBlend moved here from gs_hw.cpp
// unchanged (tools/gs_blend_check.cpp tests the same code), plus its single-source form for GPUs without dual-source
// blending (Arm Mali: no VkPhysicalDeviceFeatures::dualSrcBlend).
//
// Reference (dual-source) path: C rides in the pixel shader's second output (gs_draw.hlsl DUAL_SOURCE=1, o.blend =
// min(C, 1)) and the colour factors are Src1Alpha / InvSrc1Alpha. The framebuffer alpha is the source alpha (alpha
// factors One / Zero): the GS does not blend alpha, it stores As.
//
// Single-source path (toSingleSource): the same colour equation with ordinary factors, by where C comes from (TEST's
// ALPHA.C as the shader sees it: blendCPremul.x):
//   C = As (0): the shader writes min(As / 128, 1) into o0.a (DUAL_SOURCE=2); colour factors SrcAlpha / InvSrcAlpha.
//               o0.a no longer is As / 255, so the alpha channel blends with srcAlpha = Constant (blend constant alpha
//               128 / 255) and dstAlpha = Zero: stored alpha = min(As / 128, 1) * 128 / 255 = As / 255 for As <= 0x80.
//               As > 0x80 is stored as 0x80 (approximation; bit 7 - DATE's - is still right).
//   C = Ad (1), FIX (2): C is the same for the whole draw (the shader takes Ad as 1.0, FIX / 128 clamped to 1): colour
//               factors Constant / InvConstant with blend constant rgb = C; the plain shader, alpha One / Zero as the
//               reference. Exact.
// Colour factors and ops are otherwise the reference's, so colour is the reference's for every equation; only the
// stored alpha of C = As draws with As > 0x80 differs.
#pragma once

#include "rhi/rhi.h"

#include <algorithm>
#include <cstdint>

namespace gsblend
{
    // GS blend (A - B) * C / 128 + D as a fixed-function blend (C from the shader's second output); false when not
    // expressible (drawn unblended).
    struct GsBlend
    {
        bool enable = false;
        rhi::BlendFactor src = rhi::BlendFactor::One;
        rhi::BlendFactor dst = rhi::BlendFactor::Zero;
        rhi::BlendOp op = rhi::BlendOp::Add;
        int premul = 0; // 0: none, 1: rgb *= C, 2: rgb *= 1 + C
        // [android] single-source form (toSingleSource); the defaults are the reference's
        rhi::BlendFactor srcAlpha = rhi::BlendFactor::One;
        rhi::BlendFactor dstAlpha = rhi::BlendFactor::Zero;
        bool cInAlpha = false;  // the shader writes C into o0.a (DUAL_SOURCE=2 pixel shader)
        bool constant = false;  // the blend uses the blend constant (singleSourceConstant)
    };

    // abe, A, B, D: the draw's PRIM.ABE and ALPHA.A / B / D
    inline bool gsBlend(bool abe, int A, int B, int D, GsBlend &b)
    {
        using F = rhi::BlendFactor;
        using O = rhi::BlendOp;
        b = GsBlend{};
        if (!abe)
            return true;
        O op = O::Add;
        F src = F::One, dst = F::Zero;
        if (A == B)
        {
            // (X - X) * C + D = D
            if (D == 0) { src = F::One; dst = F::Zero; }
            else if (D == 1) { src = F::Zero; dst = F::One; }
            else { src = F::Zero; dst = F::Zero; }
        }
        else if (A == 0 && B == 1)
        {
            if (D == 1) { src = F::Src1Alpha; dst = F::InvSrc1Alpha; }                          // Cs*C + Cd*(1-C)
            else if (D == 0) { b.premul = 2; op = O::Subtract; src = F::One; dst = F::Src1Alpha; } // Cs*(1+C) - Cd*C
            else { src = F::Src1Alpha; dst = F::Src1Alpha; op = O::Subtract; }                 // (Cs - Cd)*C
        }
        else if (A == 0 && B == 2)
        {
            b.premul = 1; // Cs*C + D
            src = F::One;
            dst = (D == 1) ? F::One : F::Zero;
            if (D == 0) { b.premul = 2; src = F::One; dst = F::Zero; }
        }
        else if (A == 1 && B == 0)
        {
            if (D == 0) { src = F::InvSrc1Alpha; dst = F::Src1Alpha; } // Cs*(1-C) + Cd*C
            else return false;
        }
        else if (A == 2 && B == 0)
        {
            b.premul = 1; // D - Cs*C
            if (D == 1) { op = O::ReverseSubtract; src = F::One; dst = F::One; }
            else { src = F::Zero; dst = F::Zero; }
        }
        else if (A == 2 && B == 1)
        {
            if (D == 1) { src = F::Zero; dst = F::InvSrc1Alpha; }                               // Cd*(1-C)
            else if (D == 0) { op = O::ReverseSubtract; src = F::One; dst = F::Src1Alpha; }       // Cs - Cd*C
            else return false;
        }
        else if (A == 1 && B == 2)
        {
            if (D == 0) { src = F::One; dst = F::Src1Alpha; }  // Cs + Cd*C
            else if (D == 2) { src = F::Zero; dst = F::Src1Alpha; } // Cd*C
            else return false;
        }
        else
        {
            return false;
        }
        b.enable = true;
        b.src = src;
        b.dst = dst;
        b.op = op;
        return true;
    }

    inline bool isSrc1(rhi::BlendFactor f)
    {
        return f == rhi::BlendFactor::Src1Alpha || f == rhi::BlendFactor::InvSrc1Alpha;
    }

    // The blend needs dual-source blending (C in the second output)
    inline bool usesSrc1(const GsBlend &b) { return b.enable && (isSrc1(b.src) || isSrc1(b.dst)); }

    // [android] The blend without dual-source blending (see the top of the file). bc: ALPHA.C (0 As, 1 Ad, 2 FIX).
    inline void toSingleSource(GsBlend &b, int bc)
    {
        if (!usesSrc1(b))
            return;
        using F = rhi::BlendFactor;
        const bool as = bc == 0;
        auto map = [as](F f) {
            if (f == F::Src1Alpha) return as ? F::SrcAlpha : F::Constant;
            if (f == F::InvSrc1Alpha) return as ? F::InvSrcAlpha : F::InvConstant;
            return f;
        };
        b.src = map(b.src);
        b.dst = map(b.dst);
        b.constant = true; // the stored-alpha scale (As) or C itself (Ad, FIX)
        if (as)
        {
            b.cInAlpha = true;
            b.srcAlpha = F::Constant;
            b.dstAlpha = F::Zero;
        }
    }

    // [android] The blend constant of a single-source draw: rgb = C as the shader computes it (Ad: 1, FIX: FIX / 128,
    // both at most 1), alpha = 128 / 255 (o0.a = As / 128 back to As / 255).
    inline void singleSourceConstant(int bc, uint32_t fix, float rgba[4])
    {
        const float c = bc == 0 ? 1.0f : bc == 2 ? std::min(static_cast<float>(fix) / 128.0f, 1.0f) : 1.0f;
        rgba[0] = rgba[1] = rgba[2] = c;
        rgba[3] = 128.0f / 255.0f;
    }
}
