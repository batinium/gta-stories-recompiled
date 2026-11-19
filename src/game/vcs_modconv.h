// [android] PC model converter (docs/ANDROID.md "Mods: models"): a GTA III / VC / SA RenderWare model (.dff) and its
// textures (.txd, D3D8 / D3D9) -> VCS's PS2 formats, to stream in place of a stock model (vcs_mods.cpp).
//
// What it writes, from the structures in the player's own stock entry (the "donor", read from the disc at run time):
//   - an `ldm\0` relocatable chunk (MDL): element (atomic) + node (frame) + geometry with materials and one PS2 DMA
//     packet per material: VIF batches of at most 70 vertices (int16 positions, uint8 UVs, int8 normals) laid out
//     exactly as the stock VU1 code expects (vertex count quad at VU address 0, positions at 1, normals at 0x47, UVs
//     at 0x8D, MSCAL 6), triangle strips joined by degenerate triangles;
//   - an `xet\0` texture list: the donor's own textures (HUD icons and the like) plus the model's, resized to powers
//     of two within a budget and palettised to 8 bits (CSM1 CLUT, alpha 0x80 = opaque).
// The model is decimated (quadric edge collapse) to a triangle budget first. Nothing here touches the game: it is
// plain C++ on byte buffers, also built on the PC by tools/modconv_check.cpp.
//
// Supported: single-element models (weapons) and vehicles (convertVehicle: the stock car's structure with the PC
// parts' geometry).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcs::modconv
{
    // Bumped whenever the converter's output changes: part of the converted-model cache key (vcs_mods.cpp
    // modelCache), so a newer converter rebuilds the cached chunks.
    constexpr int kVersion = 12; // 2: the LCS dialect (Options::lcs); 3: LCS texels at +0x40; 4: LCS textures 0x54;
                                // 5: vehicles: SA _ok / _dam parts, dummies moved, "set" lines, SA light / paint colours
                                // 6: the UV set count from its byte, rotated nodes (bikes), the slot's own named
                                //    parts (forks, handlebars, bike wheels), part= (one weapon of a clump)
                                // 7: the welded decimation pass, a car's own wheels (elements on its wheel nodes)
                                // 8: vehicle glass laid over a tint, vehicle textures 128 px within a total budget
                                // 9: parts the slot has no element for drawn with the body
                                // 10: smoother glass tint; 11: own wheels / glass / texture budget LCS only
                                // 12: LCS textures without the swizzle bit; VCS textures 0x60 bytes
    struct Options
    {
        int maxTriangles = 1500;  // decimation target (a stock weapon has 160-540)
        int maxTexture = 128;     // largest texture side after resizing
        float scale = 1.0f;       // model scale (PC -> VCS units are both metres)
        // [lcs] the target game's chunk dialect. LCS's chunks are VCS's but for three things (read from LCS's own stock
        // entries, docs/LCS.md "Mods"): a texture's raster is 8 bytes {texels, flags} (VCS 16: {0, 0x250000 | width,
        // texels, flags}), a vehicle node's name pointer is at +0xA4 (VCS +0xA8), and a vehicle keeps 25 primary
        // paint materials at +0x2C and 25 secondary at +0x90 (VCS 30 at +0x2C, 25 at +0xA4).
        bool lcs = false;
        // convertSimple: only the atomics at or below the frame of this name, in its own space (one weapon of a clump
        // holding several, as GTA III packs ship weapons.dff); "" = the whole clump
        std::string part;
    };

    struct Result
    {
        std::vector<uint8_t> mdl, xet;
        std::string report;       // what was done, for the log / applied.txt; a vehicle's "set position <k> x y z" /
                                  // "set wheelscale <s>" lines are model info values (vcs_mods.cpp applies them)
    };

    // donorMdl / donorXet: the stock entries the converted model replaces (whole streaming entries, sector-padded).
    // modelId: the slot's model id (written into the element). False with `error` when the input can't be used.
    bool convertSimple(const std::vector<uint8_t> &donorMdl, const std::vector<uint8_t> &donorXet,
                       const std::vector<uint8_t> &dff, const std::vector<uint8_t> &txd, int modelId,
                       const Options &options, Result &out, std::string &error);

    // A car / bike / boat: the stock chunk kept whole (nodes, dummies, wheels), each part's geometry replaced by the
    // PC part of the same name (chassis_hi, door_lf_hi_ok / _hi_dam, ...), parts the mod lacks left empty.
    bool convertVehicle(const std::vector<uint8_t> &donorMdl, const std::vector<uint8_t> &donorXet,
                        const std::vector<uint8_t> &dff, const std::vector<uint8_t> &txd, int modelId,
                        const Options &options, Result &out, std::string &error);

    // Reads the converted (or a stock) simple MDL back into triangles: a self-check of the writer, used by the PC
    // harness. Returns the number of non-degenerate triangles, -1 on a malformed chunk.
    int countTriangles(const std::vector<uint8_t> &mdl, std::string *dump = nullptr);
}
