// [lcs] Liberty City Stories' 3D draws taken at its RSL draw calls (docs/LCS.md "RSL functions", docs/RSL_SUBMISSION.md).
//
// VCS's phase 2 (game/vcs_rsl_world.h) with LCS's addresses: FUN_0029FD40 - every model and building draw - ends by
// writing `CALL packet` into the packet buffer (0x43EA70); as the frame's chain is copied the CALLed packet is marked,
// recorded the first time it is processed (UNPACK results, MSCALs, GS data, the world handler's meshes) and replayed
// on later frames while its bytes hash the same: no VIF parsing, unpacking, mesh hashing or vertex conversion for it on
// the render worker. With the write watch (Windows; Linux with PS2X_WRITE_WATCH=1; not Android), a packet whose pages are unchanged since a
// confirmed copy is not copied at all (step 4).
//
// On only with the write watch (Windows; LCS_RSL=1 elsewhere - a loss on the PC without it). LCS_RSL=0: off (the
// packet path, as before). LCS_RSL_MODELS=0: no packets marked. LCS_RSL_SKIP=0: every packet
// copied. LCS_RSL_RASTERS=1: textures named by raster (VCS's step 3d). VCS_RSL_WORLD_VERIFY=1: every hit processed by
// the packet path and recorded again, compared op by op (log "[rsl-world] verify"); VCS_RSL_WORLD_LOG=1: counts.
#pragma once

class PS2Runtime;

namespace lcs::rsl
{
    void install(PS2Runtime &runtime); // from applyLcs, before runtime.run()
}
