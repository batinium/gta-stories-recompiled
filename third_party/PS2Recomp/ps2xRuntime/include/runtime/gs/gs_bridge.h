// [gta-recomp] Stories bridge (src/game/stories_bridge.h): extra 3D geometry drawn into the game's own frame.
//
// The HW executor (gs_hw.cpp) calls the source once per frame on the render (main) thread, at the same point as the
// dual-world composite (the game's full-screen Z clear before its HUD: the world is complete, its depth still there),
// before that composite. The triangles are drawn untextured, Gouraud-coloured, fogged with the frame's fog colour,
// depth-tested GEQUAL against the game's depth and written into it (the far city's picture then stays out of them).
// Not set (the default): nothing changes.
#pragma once

#include <cstdint>
#include <vector>

namespace gsbridge
{
    struct Vert
    {
        float x, y;     // 0..1 of the game's frame, top-left origin
        float depth;    // the frame's depth units 0..1 (larger = nearer, as the GS's GEQUAL games): used when the
                        // renderer has no projection of the frame's own (below)
        float w;        // view depth: with the frame's measured projection (the dual world's, from the game's native
                        // draws: Z = a + b / w) the depth is taken from it - the game's own mapping exactly
        uint32_t rgba;  // 0xAABBGGRR
        float fog = 1.0f; // [gta-recomp] the GS's fog factor (1: none, 0: the frame's fog colour, FOGCOL)
    };
    // Fills `tris` (3 vertices a triangle); false: nothing this frame. frameTag: the game frame being drawn (its world's
    // native draws carry it: gsdw::setFrameTag, [dwlag]; 0 unknown), so the source gives that frame's projection.
    using Source = bool (*)(std::vector<Vert> &tris, uint64_t frameTag);
    void setSource(Source source);

    // statistics: frames drawn, triangles in the last one
    struct Stats
    {
        uint64_t frames = 0;
        uint32_t triangles = 0;
    };
    Stats stats();
}
