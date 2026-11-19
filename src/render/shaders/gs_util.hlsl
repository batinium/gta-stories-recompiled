// Helper passes for the HW GS renderer (gs_hw.cpp): textured blits between targets and solid fills (partial clears).
// Both draw one quad from the vertex id (triangle strip of 4) inside `rect` (NDC: left, top, right, bottom).

cbuffer Quad : register(b0)
{
    float4 rect;
    float4 uvRect;   // blit: source texture coordinates (0..1) at the left/top and right/bottom edges
    float4 color;    // fill: colour 0..1
    float4 params;   // x: fill depth 0..1, y: blit writes alpha 1 (the displayed picture)
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID)
{
    const float2 c = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0.0, 1.0);
    o.uv = lerp(uvRect.xy, uvRect.zw, c);
    return o;
}

Texture2D<float4> source : register(t0);
SamplerState sourceSampler : register(s0);

float4 ps_blit(VSOut i) : SV_Target0
{
    const float4 t = source.Sample(sourceSampler, i.uv);
    // params.z > 0: the alpha scaled by it ([stories] the dual world's backdrop fading out in haze; 0: as is)
    return float4(t.rgb, (params.y > 0.5 ? 1.0 : t.a) * (params.z > 0.0 ? params.z : 1.0));
}

float4 ps_fill_color(VSOut i) : SV_Target0
{
    return color;
}

float ps_fill_depth(VSOut i) : SV_Depth
{
    return params.x;
}

// [gta-recomp][tone] Dual world, the background game's far fog as transparency (gs_hw.cpp dwFogToAlpha). A slice of the
// fog: `color` (its alpha: the fog factor at that distance) at the depth params.x, drawn with the GreaterEqual test so it
// lands on every pixel at least that far
struct SliceOut
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

SliceOut ps_dw_fog_slice(VSOut i)
{
    SliceOut o;
    o.color = color;
    o.depth = params.x;
    return o;
}

// The picture copied with its fog taken out: a pixel the GS fogged is f * c + (1 - f) * FOGCOL (color.rgb, 0..1), its
// alpha f (the slices). Kept as c, alpha f: blended over the other game's sky the far city fades into that sky as it
// faded into its own fog colour. f 0 (all fog): transparent
float4 ps_dw_unfog(VSOut i) : SV_Target0
{
    const float4 t = source.Sample(sourceSampler, i.uv);
    const float f = t.a;
    const float3 c = f > 0.004 ? saturate((t.rgb - color.rgb * (1.0 - f)) / f) : t.rgb;
    return float4(c, f);
}

// [gta-recomp] Dual world (runtime/gs/gs_dualworld.h): the other game's picture as the backdrop, with a depth. Where the
// pixel's view ray crosses the border plane (x = dwCam.w) towards the other city, the depth of that crossing in the
// frame's own Z (Z = dwZ.x + dwZ.y / view depth, in units of dwZ.z): with the GreaterEqual test the picture then also
// covers what this game drew beyond the border (its far sea), not only where its depth is clear.
cbuffer DwView : register(b1)
{
    float4 dwCam;   // xyz the camera, w the border's x
    float4 dwRight; // xyz the view's right * tan(half the horizontal view), w the other city's side (+1: towards +x)
    float4 dwUp;    // xyz the view's up * tan(half the vertical view), w the blend's width past the border (m; 0 none)
    float4 dwFront; // xyz the view direction, w the sea level
    float4 dwZ;     // x, y: the frame's Z mapping; z: Z's scale to 0..1; w: 1 = the border's depth written (else 0: depth clear)
    // [dwlag] the camera the picture was rendered with (it is a frame or two older than this one): each pixel's ray is
    // looked up in the picture where that camera saw it, so the other city stays locked to the world while the camera
    // turns. The camera's move since then: the other city taken on the plane x = dwPicFront.w (its parallax is tiny)
    float4 dwPicCam;   // xyz its position (this game's coordinates), w: 1 = reproject
    float4 dwPicRight; // xyz its right * tan(half the horizontal view), w 1 / |xyz|^2
    float4 dwPicUp;    // xyz its up * tan(half the vertical view), w 1 / |xyz|^2
    float4 dwPicFront; // xyz its view direction, w the stand-in plane's x
    float4 dwPad;      // [stories] x > 0: the picture's alpha scaled by it (the GPU hand-over's haze; 0: as is)
};

struct DwOut
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

DwOut ps_dw_backdrop(VSOut i)
{
    DwOut o;
    o.depth = 0.0;
    const float2 n = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
    const float3 r = dwFront.xyz + n.x * dwRight.xyz + n.y * dwUp.xyz; // view depth along it: its parameter
    float2 uv = i.uv;
    float inside = 1.0;
    if (dwPicCam.w > 0.5)
    {
        // the ray's point on the stand-in plane, seen from the picture's camera: r + (cam - picCam) / t (t: the ray's
        // parameter at the plane; rays away from it: far, rotation only)
        const float t = (dwPicFront.w - dwCam.x) / (abs(r.x) > 1e-6 ? r.x : 1e-6);
        const float3 d = r + (t > 1.0 ? (dwCam.xyz - dwPicCam.xyz) / t : float3(0.0, 0.0, 0.0));
        const float z = dot(d, dwPicFront.xyz);
        const float2 m = float2(dot(d, dwPicRight.xyz) * dwPicRight.w, dot(d, dwPicUp.xyz) * dwPicUp.w) / max(z, 1e-6);
        uv = float2(0.5 + 0.5 * m.x, 0.5 - 0.5 * m.y);
        // outside the picture (a fast turn past its overscan): its edge, faded out over the next 3% of the screen
        const float2 out2 = max(abs(m) - 1.0, 0.0);
        inside = z > 1e-6 ? saturate(1.0 - max(out2.x, out2.y) / 0.06) : 0.0;
        uv = saturate(uv);
    }
    o.color = source.Sample(sourceSampler, uv);
    o.color.a *= inside * (dwPad.x > 0.0 ? dwPad.x : 1.0);
    // only rays that reach the sea past the border: below the horizon what this game drew past that point is its far sea
    // (nothing else of it stands there). Above the horizon the depth-clear rule as before - both games' skies and clouds
    // write depth, so a depth test there would put the other game's sky band over this one's sky
    if (r.z < -1e-6 && dwCam.z > dwFront.w)
    {
        // where the ray meets the sea: this side of the border, this game's sea there and never the other city (which
        // stands past the border, above the sea): the picture is left out - also where this game's sea wrote no depth
        // (Vice City's). Just past the border it is blended in over dwUp.w m of sea: the two seas meet softly
        const float past = (dwCam.x + (dwFront.w - dwCam.z) / r.z * r.x - dwCam.w) * dwRight.w;
        o.color.a *= dwUp.w > 0.0 ? saturate(past / dwUp.w) : step(0.0, past);
        const float gap = (dwCam.w - dwCam.x) * dwRight.w;
        const float rx = r.x * dwRight.w;
        if (dwZ.w > 0.5 && past > 0.0 && gap > 0.0 && rx > 1e-6)
            o.depth = saturate(floor(dwZ.x + dwZ.y * rx / gap) * dwZ.z);
    }
    return o;
}
