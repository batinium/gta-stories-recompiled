// See gs_shaders.h. The headers come from src/render/CMakeLists.txt: DXBC from fxc (shaders/dxbc), SPIR-V from
// tools/hlsl_spirv.py (shaders/spirv), MSL text from tools/hlsl_msl.py (shaders/msl).
#include "render/gs_shaders.h"

#if defined(VCS_HAVE_D3D11)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/gs_draw_vs.h"
#include "dxbc/gs_world_vs.h"
#include "dxbc/gs_lit_vs.h"
#include "dxbc/gs_draw_ps0.h"
#include "dxbc/gs_draw_ps0_dual.h"
#include "dxbc/gs_draw_ps0_ca.h" // [android]
#include "dxbc/gs_draw_ps1.h"
#include "dxbc/gs_draw_ps1_dual.h"
#include "dxbc/gs_draw_ps1_ca.h" // [android]
#include "dxbc/gs_draw_ps2.h"
#include "dxbc/gs_draw_ps2_dual.h"
#include "dxbc/gs_draw_ps2_ca.h" // [android]
#include "dxbc/gs_draw_ps3.h"
#include "dxbc/gs_draw_ps3_dual.h"
#include "dxbc/gs_draw_ps3_ca.h" // [android]
#include "dxbc/gs_draw_ps4.h"
#include "dxbc/gs_draw_ps4_dual.h"
#include "dxbc/gs_draw_ps4_ca.h" // [android]
#include "dxbc/gs_draw_ps5.h"
#include "dxbc/gs_draw_ps5_dual.h"
#include "dxbc/gs_draw_ps5_ca.h" // [android]
#include "dxbc/gs_draw_ps6.h"
#include "dxbc/gs_draw_ps6_dual.h"
#include "dxbc/gs_draw_ps6_ca.h" // [android]
#include "dxbc/gs_quad_vs.h"
#include "dxbc/gs_blit_ps.h"
#include "dxbc/gs_fill_color_ps.h"
#include "dxbc/gs_fill_depth_ps.h"
#include "dxbc/gs_dw_backdrop_ps.h"
#include "dxbc/gs_dw_fog_slice_ps.h"
#include "dxbc/gs_dw_unfog_ps.h"
#include "dxbc/present_vs.h"
#include "dxbc/present_ps.h"
#include "dxbc/ui_vs.h"
#include "dxbc/ui_ps.h"
#include "dxbc/gs_draw_ps0_noz.h"
#include "dxbc/gs_draw_ps0_dual_noz.h"
#include "dxbc/gs_draw_ps0_ca_noz.h" // [android]
#include "dxbc/gs_draw_ps1_noz.h"
#include "dxbc/gs_draw_ps1_dual_noz.h"
#include "dxbc/gs_draw_ps1_ca_noz.h" // [android]
#include "dxbc/gs_draw_ps2_noz.h"
#include "dxbc/gs_draw_ps2_dual_noz.h"
#include "dxbc/gs_draw_ps2_ca_noz.h" // [android]
#include "dxbc/gs_draw_ps3_noz.h"
#include "dxbc/gs_draw_ps3_dual_noz.h"
#include "dxbc/gs_draw_ps3_ca_noz.h" // [android]
#include "dxbc/gs_draw_ps4_noz.h"
#include "dxbc/gs_draw_ps4_dual_noz.h"
#include "dxbc/gs_draw_ps4_ca_noz.h" // [android]
#include "dxbc/gs_draw_ps5_noz.h"
#include "dxbc/gs_draw_ps5_dual_noz.h"
#include "dxbc/gs_draw_ps5_ca_noz.h" // [android]
#include "dxbc/gs_draw_ps6_noz.h"
#include "dxbc/gs_draw_ps6_dual_noz.h"
#include "dxbc/gs_draw_ps6_ca_noz.h" // [android]
#endif
#if defined(VCS_HAVE_VULKAN)
#include "spirv/gs_draw_vs.h"
#include "spirv/gs_draw_point_vs.h"
#include "spirv/gs_world_vs.h"
#include "spirv/gs_lit_vs.h"
#include "spirv/gs_draw_ps0.h"
#include "spirv/gs_draw_ps0_dual.h"
#include "spirv/gs_draw_ps0_ca.h" // [android]
#include "spirv/gs_draw_ps1.h"
#include "spirv/gs_draw_ps1_dual.h"
#include "spirv/gs_draw_ps1_ca.h" // [android]
#include "spirv/gs_draw_ps2.h"
#include "spirv/gs_draw_ps2_dual.h"
#include "spirv/gs_draw_ps2_ca.h" // [android]
#include "spirv/gs_draw_ps3.h"
#include "spirv/gs_draw_ps3_dual.h"
#include "spirv/gs_draw_ps3_ca.h" // [android]
#include "spirv/gs_draw_ps4.h"
#include "spirv/gs_draw_ps4_dual.h"
#include "spirv/gs_draw_ps4_ca.h" // [android]
#include "spirv/gs_draw_ps5.h"
#include "spirv/gs_draw_ps5_dual.h"
#include "spirv/gs_draw_ps5_ca.h" // [android]
#include "spirv/gs_draw_ps6.h"
#include "spirv/gs_draw_ps6_dual.h"
#include "spirv/gs_draw_ps6_ca.h" // [android]
#include "spirv/gs_quad_vs.h"
#include "spirv/gs_blit_ps.h"
#include "spirv/gs_fill_color_ps.h"
#include "spirv/gs_fill_depth_ps.h"
#include "spirv/gs_dw_backdrop_ps.h"
#include "spirv/gs_dw_fog_slice_ps.h"
#include "spirv/gs_dw_unfog_ps.h"
#include "spirv/present_vs.h"
#include "spirv/present_ps.h"
#include "spirv/ui_vs.h"
#include "spirv/ui_ps.h"
#include "spirv/gs_draw_ps0_noz.h"
#include "spirv/gs_draw_ps0_dual_noz.h"
#include "spirv/gs_draw_ps0_ca_noz.h" // [android]
#include "spirv/gs_draw_ps1_noz.h"
#include "spirv/gs_draw_ps1_dual_noz.h"
#include "spirv/gs_draw_ps1_ca_noz.h" // [android]
#include "spirv/gs_draw_ps2_noz.h"
#include "spirv/gs_draw_ps2_dual_noz.h"
#include "spirv/gs_draw_ps2_ca_noz.h" // [android]
#include "spirv/gs_draw_ps3_noz.h"
#include "spirv/gs_draw_ps3_dual_noz.h"
#include "spirv/gs_draw_ps3_ca_noz.h" // [android]
#include "spirv/gs_draw_ps4_noz.h"
#include "spirv/gs_draw_ps4_dual_noz.h"
#include "spirv/gs_draw_ps4_ca_noz.h" // [android]
#include "spirv/gs_draw_ps5_noz.h"
#include "spirv/gs_draw_ps5_dual_noz.h"
#include "spirv/gs_draw_ps5_ca_noz.h" // [android]
#include "spirv/gs_draw_ps6_noz.h"
#include "spirv/gs_draw_ps6_dual_noz.h"
#include "spirv/gs_draw_ps6_ca_noz.h" // [android]
#endif
#if defined(VCS_HAVE_METAL)
#include "msl/gs_draw_vs.h"
#include "msl/gs_world_vs.h"
#include "msl/gs_lit_vs.h"
#include "msl/gs_draw_ps0.h"
#include "msl/gs_draw_ps0_dual.h"
#include "msl/gs_draw_ps0_ca.h" // [android]
#include "msl/gs_draw_ps1.h"
#include "msl/gs_draw_ps1_dual.h"
#include "msl/gs_draw_ps1_ca.h" // [android]
#include "msl/gs_draw_ps2.h"
#include "msl/gs_draw_ps2_dual.h"
#include "msl/gs_draw_ps2_ca.h" // [android]
#include "msl/gs_draw_ps3.h"
#include "msl/gs_draw_ps3_dual.h"
#include "msl/gs_draw_ps3_ca.h" // [android]
#include "msl/gs_draw_ps4.h"
#include "msl/gs_draw_ps4_dual.h"
#include "msl/gs_draw_ps4_ca.h" // [android]
#include "msl/gs_draw_ps5.h"
#include "msl/gs_draw_ps5_dual.h"
#include "msl/gs_draw_ps5_ca.h" // [android]
#include "msl/gs_draw_ps6.h"
#include "msl/gs_draw_ps6_dual.h"
#include "msl/gs_draw_ps6_ca.h" // [android]
#include "msl/gs_quad_vs.h"
#include "msl/gs_blit_ps.h"
#include "msl/gs_fill_color_ps.h"
#include "msl/gs_fill_depth_ps.h"
#include "msl/gs_dw_backdrop_ps.h"
#include "msl/gs_dw_fog_slice_ps.h"
#include "msl/gs_dw_unfog_ps.h"
#include "msl/present_vs.h"
#include "msl/present_ps.h"
#include "msl/ui_vs.h"
#include "msl/ui_ps.h"
#include "msl/gs_draw_ps0_noz.h"
#include "msl/gs_draw_ps0_dual_noz.h"
#include "msl/gs_draw_ps0_ca_noz.h" // [android]
#include "msl/gs_draw_ps1_noz.h"
#include "msl/gs_draw_ps1_dual_noz.h"
#include "msl/gs_draw_ps1_ca_noz.h" // [android]
#include "msl/gs_draw_ps2_noz.h"
#include "msl/gs_draw_ps2_dual_noz.h"
#include "msl/gs_draw_ps2_ca_noz.h" // [android]
#include "msl/gs_draw_ps3_noz.h"
#include "msl/gs_draw_ps3_dual_noz.h"
#include "msl/gs_draw_ps3_ca_noz.h" // [android]
#include "msl/gs_draw_ps4_noz.h"
#include "msl/gs_draw_ps4_dual_noz.h"
#include "msl/gs_draw_ps4_ca_noz.h" // [android]
#include "msl/gs_draw_ps5_noz.h"
#include "msl/gs_draw_ps5_dual_noz.h"
#include "msl/gs_draw_ps5_ca_noz.h" // [android]
#include "msl/gs_draw_ps6_noz.h"
#include "msl/gs_draw_ps6_dual_noz.h"
#include "msl/gs_draw_ps6_ca_noz.h" // [android]
#endif

namespace render
{
    const GsShaders &gsShaders(rhi::Backend backend)
    {
#if defined(VCS_HAVE_D3D11)
        static const GsShaders dxbc{
            {kGsDrawVS, sizeof(kGsDrawVS)},
            {kGsWorldVS, sizeof(kGsWorldVS)},
            {kGsLitVS, sizeof(kGsLitVS)},
            {
                {{kGsDrawPS0, sizeof(kGsDrawPS0)}, {kGsDrawPS0Dual, sizeof(kGsDrawPS0Dual)}, {kGsDrawPS0CA, sizeof(kGsDrawPS0CA)}},
                {{kGsDrawPS1, sizeof(kGsDrawPS1)}, {kGsDrawPS1Dual, sizeof(kGsDrawPS1Dual)}, {kGsDrawPS1CA, sizeof(kGsDrawPS1CA)}},
                {{kGsDrawPS2, sizeof(kGsDrawPS2)}, {kGsDrawPS2Dual, sizeof(kGsDrawPS2Dual)}, {kGsDrawPS2CA, sizeof(kGsDrawPS2CA)}},
                {{kGsDrawPS3, sizeof(kGsDrawPS3)}, {kGsDrawPS3Dual, sizeof(kGsDrawPS3Dual)}, {kGsDrawPS3CA, sizeof(kGsDrawPS3CA)}},
                {{kGsDrawPS4, sizeof(kGsDrawPS4)}, {kGsDrawPS4Dual, sizeof(kGsDrawPS4Dual)}, {kGsDrawPS4CA, sizeof(kGsDrawPS4CA)}},
                {{kGsDrawPS5, sizeof(kGsDrawPS5)}, {kGsDrawPS5Dual, sizeof(kGsDrawPS5Dual)}, {kGsDrawPS5CA, sizeof(kGsDrawPS5CA)}},
                {{kGsDrawPS6, sizeof(kGsDrawPS6)}, {kGsDrawPS6Dual, sizeof(kGsDrawPS6Dual)}, {kGsDrawPS6CA, sizeof(kGsDrawPS6CA)}},
            },
            {kGsQuadVS, sizeof(kGsQuadVS)},
            {kGsBlitPS, sizeof(kGsBlitPS)},
            {kGsFillColorPS, sizeof(kGsFillColorPS)},
            {kGsFillDepthPS, sizeof(kGsFillDepthPS)},
            {kPresentVS, sizeof(kPresentVS)},
            {kPresentPS, sizeof(kPresentPS)},
            {kUiVS, sizeof(kUiVS)},
            {kUiPS, sizeof(kUiPS)},
            {
                {{kGsDrawPS0NoZ, sizeof(kGsDrawPS0NoZ)}, {kGsDrawPS0DualNoZ, sizeof(kGsDrawPS0DualNoZ)}, {kGsDrawPS0CANoZ, sizeof(kGsDrawPS0CANoZ)}},
                {{kGsDrawPS1NoZ, sizeof(kGsDrawPS1NoZ)}, {kGsDrawPS1DualNoZ, sizeof(kGsDrawPS1DualNoZ)}, {kGsDrawPS1CANoZ, sizeof(kGsDrawPS1CANoZ)}},
                {{kGsDrawPS2NoZ, sizeof(kGsDrawPS2NoZ)}, {kGsDrawPS2DualNoZ, sizeof(kGsDrawPS2DualNoZ)}, {kGsDrawPS2CANoZ, sizeof(kGsDrawPS2CANoZ)}},
                {{kGsDrawPS3NoZ, sizeof(kGsDrawPS3NoZ)}, {kGsDrawPS3DualNoZ, sizeof(kGsDrawPS3DualNoZ)}, {kGsDrawPS3CANoZ, sizeof(kGsDrawPS3CANoZ)}},
                {{kGsDrawPS4NoZ, sizeof(kGsDrawPS4NoZ)}, {kGsDrawPS4DualNoZ, sizeof(kGsDrawPS4DualNoZ)}, {kGsDrawPS4CANoZ, sizeof(kGsDrawPS4CANoZ)}},
                {{kGsDrawPS5NoZ, sizeof(kGsDrawPS5NoZ)}, {kGsDrawPS5DualNoZ, sizeof(kGsDrawPS5DualNoZ)}, {kGsDrawPS5CANoZ, sizeof(kGsDrawPS5CANoZ)}},
                {{kGsDrawPS6NoZ, sizeof(kGsDrawPS6NoZ)}, {kGsDrawPS6DualNoZ, sizeof(kGsDrawPS6DualNoZ)}, {kGsDrawPS6CANoZ, sizeof(kGsDrawPS6CANoZ)}},
            },
            {kGsDwBackdropPS, sizeof(kGsDwBackdropPS)},
            {kGsDwFogSlicePS, sizeof(kGsDwFogSlicePS)},
            {kGsDwUnfogPS, sizeof(kGsDwUnfogPS)},
        };
        if (backend == rhi::Backend::D3D11)
            return dxbc;
#endif
#if defined(VCS_HAVE_VULKAN)
        static const GsShaders spirv{
            {kGsDrawVS_spv, sizeof(kGsDrawVS_spv)},
            {kGsWorldVS_spv, sizeof(kGsWorldVS_spv)},
            {kGsLitVS_spv, sizeof(kGsLitVS_spv)},
            {
                {{kGsDrawPS0_spv, sizeof(kGsDrawPS0_spv)}, {kGsDrawPS0Dual_spv, sizeof(kGsDrawPS0Dual_spv)}, {kGsDrawPS0CA_spv, sizeof(kGsDrawPS0CA_spv)}},
                {{kGsDrawPS1_spv, sizeof(kGsDrawPS1_spv)}, {kGsDrawPS1Dual_spv, sizeof(kGsDrawPS1Dual_spv)}, {kGsDrawPS1CA_spv, sizeof(kGsDrawPS1CA_spv)}},
                {{kGsDrawPS2_spv, sizeof(kGsDrawPS2_spv)}, {kGsDrawPS2Dual_spv, sizeof(kGsDrawPS2Dual_spv)}, {kGsDrawPS2CA_spv, sizeof(kGsDrawPS2CA_spv)}},
                {{kGsDrawPS3_spv, sizeof(kGsDrawPS3_spv)}, {kGsDrawPS3Dual_spv, sizeof(kGsDrawPS3Dual_spv)}, {kGsDrawPS3CA_spv, sizeof(kGsDrawPS3CA_spv)}},
                {{kGsDrawPS4_spv, sizeof(kGsDrawPS4_spv)}, {kGsDrawPS4Dual_spv, sizeof(kGsDrawPS4Dual_spv)}, {kGsDrawPS4CA_spv, sizeof(kGsDrawPS4CA_spv)}},
                {{kGsDrawPS5_spv, sizeof(kGsDrawPS5_spv)}, {kGsDrawPS5Dual_spv, sizeof(kGsDrawPS5Dual_spv)}, {kGsDrawPS5CA_spv, sizeof(kGsDrawPS5CA_spv)}},
                {{kGsDrawPS6_spv, sizeof(kGsDrawPS6_spv)}, {kGsDrawPS6Dual_spv, sizeof(kGsDrawPS6Dual_spv)}, {kGsDrawPS6CA_spv, sizeof(kGsDrawPS6CA_spv)}},
            },
            {kGsQuadVS_spv, sizeof(kGsQuadVS_spv)},
            {kGsBlitPS_spv, sizeof(kGsBlitPS_spv)},
            {kGsFillColorPS_spv, sizeof(kGsFillColorPS_spv)},
            {kGsFillDepthPS_spv, sizeof(kGsFillDepthPS_spv)},
            {kPresentVS_spv, sizeof(kPresentVS_spv)},
            {kPresentPS_spv, sizeof(kPresentPS_spv)},
            {kUiVS_spv, sizeof(kUiVS_spv)},
            {kUiPS_spv, sizeof(kUiPS_spv)},
            {
                {{kGsDrawPS0NoZ_spv, sizeof(kGsDrawPS0NoZ_spv)}, {kGsDrawPS0DualNoZ_spv, sizeof(kGsDrawPS0DualNoZ_spv)}, {kGsDrawPS0CANoZ_spv, sizeof(kGsDrawPS0CANoZ_spv)}},
                {{kGsDrawPS1NoZ_spv, sizeof(kGsDrawPS1NoZ_spv)}, {kGsDrawPS1DualNoZ_spv, sizeof(kGsDrawPS1DualNoZ_spv)}, {kGsDrawPS1CANoZ_spv, sizeof(kGsDrawPS1CANoZ_spv)}},
                {{kGsDrawPS2NoZ_spv, sizeof(kGsDrawPS2NoZ_spv)}, {kGsDrawPS2DualNoZ_spv, sizeof(kGsDrawPS2DualNoZ_spv)}, {kGsDrawPS2CANoZ_spv, sizeof(kGsDrawPS2CANoZ_spv)}},
                {{kGsDrawPS3NoZ_spv, sizeof(kGsDrawPS3NoZ_spv)}, {kGsDrawPS3DualNoZ_spv, sizeof(kGsDrawPS3DualNoZ_spv)}, {kGsDrawPS3CANoZ_spv, sizeof(kGsDrawPS3CANoZ_spv)}},
                {{kGsDrawPS4NoZ_spv, sizeof(kGsDrawPS4NoZ_spv)}, {kGsDrawPS4DualNoZ_spv, sizeof(kGsDrawPS4DualNoZ_spv)}, {kGsDrawPS4CANoZ_spv, sizeof(kGsDrawPS4CANoZ_spv)}},
                {{kGsDrawPS5NoZ_spv, sizeof(kGsDrawPS5NoZ_spv)}, {kGsDrawPS5DualNoZ_spv, sizeof(kGsDrawPS5DualNoZ_spv)}, {kGsDrawPS5CANoZ_spv, sizeof(kGsDrawPS5CANoZ_spv)}},
                {{kGsDrawPS6NoZ_spv, sizeof(kGsDrawPS6NoZ_spv)}, {kGsDrawPS6DualNoZ_spv, sizeof(kGsDrawPS6DualNoZ_spv)}, {kGsDrawPS6CANoZ_spv, sizeof(kGsDrawPS6CANoZ_spv)}},
            },
            {kGsDwBackdropPS_spv, sizeof(kGsDwBackdropPS_spv)},
            {kGsDwFogSlicePS_spv, sizeof(kGsDwFogSlicePS_spv)},
            {kGsDwUnfogPS_spv, sizeof(kGsDwUnfogPS_spv)},
            {kGsDrawPointVS_spv, sizeof(kGsDrawPointVS_spv)}, // [android] the only vertex shader with a point size output
        };
        if (backend == rhi::Backend::Vulkan)
            return spirv;
#endif
#if defined(VCS_HAVE_METAL)
        static const GsShaders msl{
            {kGsDrawVS_msl, sizeof(kGsDrawVS_msl)},
            {kGsWorldVS_msl, sizeof(kGsWorldVS_msl)},
            {kGsLitVS_msl, sizeof(kGsLitVS_msl)},
            {
                {{kGsDrawPS0_msl, sizeof(kGsDrawPS0_msl)}, {kGsDrawPS0Dual_msl, sizeof(kGsDrawPS0Dual_msl)}, {kGsDrawPS0CA_msl, sizeof(kGsDrawPS0CA_msl)}},
                {{kGsDrawPS1_msl, sizeof(kGsDrawPS1_msl)}, {kGsDrawPS1Dual_msl, sizeof(kGsDrawPS1Dual_msl)}, {kGsDrawPS1CA_msl, sizeof(kGsDrawPS1CA_msl)}},
                {{kGsDrawPS2_msl, sizeof(kGsDrawPS2_msl)}, {kGsDrawPS2Dual_msl, sizeof(kGsDrawPS2Dual_msl)}, {kGsDrawPS2CA_msl, sizeof(kGsDrawPS2CA_msl)}},
                {{kGsDrawPS3_msl, sizeof(kGsDrawPS3_msl)}, {kGsDrawPS3Dual_msl, sizeof(kGsDrawPS3Dual_msl)}, {kGsDrawPS3CA_msl, sizeof(kGsDrawPS3CA_msl)}},
                {{kGsDrawPS4_msl, sizeof(kGsDrawPS4_msl)}, {kGsDrawPS4Dual_msl, sizeof(kGsDrawPS4Dual_msl)}, {kGsDrawPS4CA_msl, sizeof(kGsDrawPS4CA_msl)}},
                {{kGsDrawPS5_msl, sizeof(kGsDrawPS5_msl)}, {kGsDrawPS5Dual_msl, sizeof(kGsDrawPS5Dual_msl)}, {kGsDrawPS5CA_msl, sizeof(kGsDrawPS5CA_msl)}},
                {{kGsDrawPS6_msl, sizeof(kGsDrawPS6_msl)}, {kGsDrawPS6Dual_msl, sizeof(kGsDrawPS6Dual_msl)}, {kGsDrawPS6CA_msl, sizeof(kGsDrawPS6CA_msl)}},
            },
            {kGsQuadVS_msl, sizeof(kGsQuadVS_msl)},
            {kGsBlitPS_msl, sizeof(kGsBlitPS_msl)},
            {kGsFillColorPS_msl, sizeof(kGsFillColorPS_msl)},
            {kGsFillDepthPS_msl, sizeof(kGsFillDepthPS_msl)},
            {kPresentVS_msl, sizeof(kPresentVS_msl)},
            {kPresentPS_msl, sizeof(kPresentPS_msl)},
            {kUiVS_msl, sizeof(kUiVS_msl)},
            {kUiPS_msl, sizeof(kUiPS_msl)},
            {
                {{kGsDrawPS0NoZ_msl, sizeof(kGsDrawPS0NoZ_msl)}, {kGsDrawPS0DualNoZ_msl, sizeof(kGsDrawPS0DualNoZ_msl)}, {kGsDrawPS0CANoZ_msl, sizeof(kGsDrawPS0CANoZ_msl)}},
                {{kGsDrawPS1NoZ_msl, sizeof(kGsDrawPS1NoZ_msl)}, {kGsDrawPS1DualNoZ_msl, sizeof(kGsDrawPS1DualNoZ_msl)}, {kGsDrawPS1CANoZ_msl, sizeof(kGsDrawPS1CANoZ_msl)}},
                {{kGsDrawPS2NoZ_msl, sizeof(kGsDrawPS2NoZ_msl)}, {kGsDrawPS2DualNoZ_msl, sizeof(kGsDrawPS2DualNoZ_msl)}, {kGsDrawPS2CANoZ_msl, sizeof(kGsDrawPS2CANoZ_msl)}},
                {{kGsDrawPS3NoZ_msl, sizeof(kGsDrawPS3NoZ_msl)}, {kGsDrawPS3DualNoZ_msl, sizeof(kGsDrawPS3DualNoZ_msl)}, {kGsDrawPS3CANoZ_msl, sizeof(kGsDrawPS3CANoZ_msl)}},
                {{kGsDrawPS4NoZ_msl, sizeof(kGsDrawPS4NoZ_msl)}, {kGsDrawPS4DualNoZ_msl, sizeof(kGsDrawPS4DualNoZ_msl)}, {kGsDrawPS4CANoZ_msl, sizeof(kGsDrawPS4CANoZ_msl)}},
                {{kGsDrawPS5NoZ_msl, sizeof(kGsDrawPS5NoZ_msl)}, {kGsDrawPS5DualNoZ_msl, sizeof(kGsDrawPS5DualNoZ_msl)}, {kGsDrawPS5CANoZ_msl, sizeof(kGsDrawPS5CANoZ_msl)}},
                {{kGsDrawPS6NoZ_msl, sizeof(kGsDrawPS6NoZ_msl)}, {kGsDrawPS6DualNoZ_msl, sizeof(kGsDrawPS6DualNoZ_msl)}, {kGsDrawPS6CANoZ_msl, sizeof(kGsDrawPS6CANoZ_msl)}},
            },
            {kGsDwBackdropPS_msl, sizeof(kGsDwBackdropPS_msl)},
            {kGsDwFogSlicePS_msl, sizeof(kGsDwFogSlicePS_msl)},
            {kGsDwUnfogPS_msl, sizeof(kGsDwUnfogPS_msl)},
        };
        if (backend == rhi::Backend::Metal)
            return msl;
#endif
        (void)backend;
        static const GsShaders none{};
        return none;
    }
}
