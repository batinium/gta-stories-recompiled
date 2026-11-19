// [lcs] Liberty City Stories: the game's zlib inflate_fast natively (lcs_inflate.cpp). LCS_INFLATE=0: the guest's;
// LCS_INFLATE=verify: both, compared (stderr every 5 s).
#pragma once

class PS2Runtime;

namespace lcs::inflate
{
    void install(PS2Runtime &runtime);
}
