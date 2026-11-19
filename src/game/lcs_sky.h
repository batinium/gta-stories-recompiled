// Liberty City Stories: the sky backdrop's fill below the horizon in the fog colour (no dark specks in fog). See
// lcs_sky.cpp.
#pragma once

class PS2Runtime;

namespace lcs::sky
{
    // From applyLcs (lcs_overrides.cpp). LCS_SKY_FILL=0: the PS2's dark grey fill.
    void install(PS2Runtime &runtime);
}
