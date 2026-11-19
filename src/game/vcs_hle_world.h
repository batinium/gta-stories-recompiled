// The native world draw (vcs_hle_world.cpp) as the RSL world draws (vcs_rsl_world.cpp) use it: a world packet's mesh -
// its content key and converted vertices - kept with a recorded geometry block so a replay needs neither the hashing nor
// the conversion. Both are per thread and cover one MSCAL.
#pragma once

#include <cstdint>
#include <vector>

namespace vcs::hleworld
{
    struct WorldMesh
    {
        bool valid = false;
        uint64_t key = 0;
        uint32_t count = 0;
        std::vector<uint8_t> vertices; // count x 32 bytes (vs_world's layout)
    };

    // the next world packet the native handler draws stores its mesh here (it is marked valid when it does)
    void setMeshCapture(WorldMesh *capture);
    // the next world packet takes its mesh from here when it has the same vertex count (the replay of the same block)
    void setMeshHint(const WorldMesh *hint);

    // [lcs] Another game's VU1 image of the routines a pipeline's reproduction covers (game/lcs_render.cpp): asked when
    // VU1 code memory is not one of VCS's images; true = the reproduction applies. skinMinVertices: that image's
    // skinning stage processes at least this many vertices (writing the extra ones over whatever follows the packet's
    // buffers): skinned packets with fewer stay on the VU1. Set once, before boot.
    enum class Pipeline
    {
        Lit,   // stage table 0x16: skinned lit models
        Rigid, // stage table 0x19 with lighting 0x4ff: rigid lit models (and table 0x13's first pass)
    };
    using CodeCheck = bool (*)(Pipeline pipeline, const uint8_t *vuCode);
    void setOtherGameCode(CodeCheck check, uint32_t skinMinVertices);
}
