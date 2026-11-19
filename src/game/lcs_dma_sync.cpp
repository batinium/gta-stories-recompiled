// [lcs] The model heap's frees and moves wait for the VIF1 chain walk in flight - see game/lcs_dma_sync.h.
//
// With PS2X_CHAIN_ASYNC the frame's VIF1 chain is walked (followed and copied from guest RAM) on its own thread while
// the game goes on with the next frame. The chain CALLs the models' packets and REFs their textures where they are in
// the streaming heap, so what the game must not do before the walk is done is free or move that memory: streaming
// loads new models into freed space and compacts the heap. Seen with PS2X_CHAIN_ASYNC=verify on rapid teleports
// (2026-11-18, before these hooks): 3 of 8,200 walks read texture pixels and packets the game had already replaced
// (a texture REF's DIRECT data, then the rest of the chain differed). Addresses SLUS_214.23 (LCS USA v1.01):
//  - CMemoryHeap::Free 0x150740 (heap a0, block a1): RemoveModel 0x18C428 -> DeleteRwObject 0x1B7CB0 / 0x1D0378 free a
//    model's blob through it (game-internals.md, the special-character use-after-free);
//  - FUN_001509B8 (heap, block): the move the defragmenters call; FUN_001916A8 / FUN_00191780: CStreaming's heap
//    defragmentation (MakeSpaceFor 0x18EA08 calls them; game-internals.md "Streaming").
#include "game/lcs_dma_sync.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_chain_observer.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace lcs::dmasync
{
    namespace
    {
        constexpr uint32_t kHooked[] = {0x00150740u, 0x001509B8u, 0x001916A8u, 0x00191780u};
        constexpr int kCount = int(sizeof(kHooked) / sizeof(kHooked[0]));
        PS2Runtime::RecompiledFunction g_original[kCount] = {};

        template <int I>
        PS2_GUEST_CC void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ps2chain::waitAsyncWalk(); // before the original frees / moves anything
            g_original[I](rdram, ctx, runtime);
        }

        template <int... I>
        int installAll(PS2Runtime &runtime, std::integer_sequence<int, I...>)
        {
            int n = 0;
            ((g_original[I] = runtime.lookupFunction(kHooked[I]), g_original[I] ? (runtime.replaceFunction(kHooked[I], &hook<I>), ++n) : 0), ...);
            return n;
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("LCS_DMA_SYNC");
        if (e && *e == '0')
            return;
        const int n = installAll(runtime, std::make_integer_sequence<int, kCount>{});
        if (n != kCount)
            std::fprintf(stderr, "[lcs-dma-sync] %d of %d heap functions hooked\n", n, kCount);
    }
}
