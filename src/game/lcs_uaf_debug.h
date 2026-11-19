// [lcs] Debug aid for use-after-free hunts (LCS_UAF_LOG=1): heap-free ring and element render checks (lcs_uaf_debug.cpp).
#pragma once

class PS2Runtime;

namespace lcs::uafdebug
{
    void install(PS2Runtime &runtime); // applyLcs; hooks nothing unless LCS_UAF_LOG is set
}
