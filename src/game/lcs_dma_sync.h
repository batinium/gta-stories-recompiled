// [lcs] Liberty City Stories: the model heap's frees and moves wait for the VIF1 chain walk in flight
// (PS2X_CHAIN_ASYNC, runtime/ps2_chain_observer.h ps2chain::waitAsyncWalk). LCS_DMA_SYNC=0: not hooked.
#pragma once

class PS2Runtime;

namespace lcs::dmasync
{
    void install(PS2Runtime &runtime);
}
