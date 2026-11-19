// Liberty City Stories: the renderer's per-game settings and the native (HLE) render paths that apply to LCS's VU1
// microcode. See lcs_render.cpp.
#pragma once

class PS2Runtime;

namespace lcs::render
{
    // From applyLcs (lcs_overrides.cpp). LCS_RENDER=0: none of it (the generic GS / VU1 path, for comparison).
    void install(PS2Runtime &runtime);
}
