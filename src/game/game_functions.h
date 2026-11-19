#pragma once
// [gta-recomp] Calling a recompiled game function by its address from our hooks: VCS_GAME_FN(FUN_00220c68_0x220c68,
// 0x00220C68u)(rdram, ctx, runtime), also usable as a template argument.
//
// A build with the generated code linked in (desktop, the Android personal build) calls the generated function by its
// name, as before. The Android split build (docs/ANDROID.md "Runtime / game split", PS2X_SPLIT_RUNTIME) links our code
// without the generated code: the call goes through ps2GameFunction<address>, which finds the generated function
// at that address once (ps2OriginalFunction: the one the game library put there, also after replaceFunction swapped a
// hook in, so a hook calling its original never calls itself). The name is then only documentation.

#include "ps2_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

template <uint32_t Address>
PS2_GUEST_CC void ps2GameFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const PS2Runtime::RecompiledFunction fn = [] {
        const PS2Runtime::RecompiledFunction found = ps2OriginalFunction(Address);
        if (!found)
        {
            std::fprintf(stderr, "[game-library] no recompiled function at 0x%08x\n", static_cast<unsigned>(Address));
            std::abort();
        }
        return found;
    }();
    fn(rdram, ctx, runtime);
}

// (VCS_GAME_OTHER: a build for another game's bring-up, CMake VCS_GAME - VCS's functions aren't in its generated code)
#if (defined(PS2X_SPLIT_RUNTIME) && PS2X_SPLIT_RUNTIME) || defined(VCS_GAME_OTHER)
#define VCS_GAME_FN(name, address) ps2GameFunction<address>
#else
#include <ps2_recompiled_functions.h>
#define VCS_GAME_FN(name, address) name
#endif
