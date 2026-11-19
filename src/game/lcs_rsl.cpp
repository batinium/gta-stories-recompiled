// [lcs] Liberty City Stories' draws from its RSL draw calls - see game/lcs_rsl.h. Addresses: SLUS_214.23 (LCS USA
// v1.01), each with the code it was read from (Ghidra decompiles of the analysed ELF, the recompiled code's comments).
#include "game/lcs_rsl.h"

#include "game/vcs_rsl_native.h"
#include "game/vcs_rsl_world.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/ps2_write_watch.h"

#include <cstdio>
#include <cstdlib>

namespace lcs::rsl
{
    namespace
    {
        // The packet buffer object (VCS 0x520F90, the same layout): +0x14 the half in use, +0x18 the DMA cursor, +0x1C
        // the data pointer growing down; two halves of 0xE6670 bytes from +0x20. FUN_002B5400 (the kick) computes the
        // chain start as `half * 0xE6670 (lui 0xE; ori 0x6670 at 0x2B544C) + 0x20 + object` and the draw functions store
        // the cursor through `lui 0x44; -0x1590` (0x43EA88). Seen live: cursor 0x45B310.. / 0x543260.., top 0x51D790 /
        // 0x603D90 for halves 0 / 1.
        constexpr uint32_t kPacketBuffer = 0x0043EA70u;
        constexpr uint32_t kPacketEnd = kPacketBuffer + 0x20u + 2u * 0xE6670u; // 0x60B770
        // FUN_0029FD40(packet) = VCS's FUN_002F7D58: the state flush FUN_002A02F0, the queued GS registers (0x65EFD8,
        // count gp+0x1D0) as CNT + DIRECTHL, the shared state packet CALL when bit 31 of gp+0x188 is set, then
        // `CALL packet` with qwc 0 (`a0 << 32 | 0x50000000` at 0x29FE24 / 0x29FE68). Every model and world-building draw:
        // FUN_00154390 (a sector's / model's packet lists, opaque then alpha) calls it per packet.
        constexpr uint32_t kModelDraw = 0x0029FD40u;
        // FUN_0029FE98(streams, count, mask) is not VCS's world REF (FUN_002F7EB0) but its Im3D submit (FUN_002F8250):
        // per batch a CNT (STCYCL / UNPACK header) and one REF per vertex stream with its own UNPACK, then MSCAL 6;
        // callers FUN_002B1E60 / FUN_002B2298 build the streams from index lists each frame. Not a prebuilt block: none.
        // LCS has no world REF draw: its buildings are CALLed packets like models.
        // The frame kick FUN_00212810(channel, chain) (lcs_frame.cpp): the frame's VIF1 chain, a0 = 1.
        constexpr uint32_t kFrameKick = 0x00212810u;

        PS2Runtime::RecompiledFunction g_kickNext = nullptr;

        PS2_GUEST_CC void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 4) == 1u) // VIF1: the frame's draws are complete, their chain is copied by the original
            {
                // the last frame's chain may still be walked (PS2X_CHAIN_ASYNC): its observers read the kicked draw list,
                // which onFrameKick replaces with this frame's
                ps2chain::waitAsyncWalk();
                vcs::rslworld::onFrameKick();
            }
            g_kickNext(rdram, ctx, runtime);
        }

        bool off(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e == '0';
        }
        bool on(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e == '1';
        }
    }

    void install(PS2Runtime &runtime)
    {
        // The state flush FUN_002A02F0 natively (VCS's FUN_002F8710 port, game/vcs_rsl_native.h): read from the
        // recompiled code's listing, instruction for instruction the same function at LCS's addresses - dirty word
        // gp+0x188 = 0x3D7078 (gp 0x3D6EF0), context gp+0x18C, its packet gp+0x190, instance matrix switch / REF gp+0x198 /
        // gp+0x1A0; the GS register values 0x3939D8.. (`lui 0x39; ld 0x39D8..0x39F0`), queue 0x65EFD8 (`lui 0x66; -0x1028`)
        // counted at gp+0x1D0; bit 0x2000's table 0x66AC60 (`lui 0x67; -0x53A0`) indexed by gp+0x31DC = 0x3DA0CC; bit 0x8's
        // source 0x66AC90; the viewport block 0x43DA70 (`lui 0x44; -0x2590`), and the others where VCS's are relative to
        // it (0x43D9B0 / 0x43D9F0 / 0x43DA30 / 0x43DAE0 / 0x43DAF0 / 0x43DC10); the same VU addresses and MSCALs (0x75 /
        // 0x4A / 0x8F / 0x83 / 0x3C). The two +0x50 plug-in callbacks seen (VCS_RSL_PLUGIN_LOG=1: 0x356190 ~8k, 0x23E630 ~300\n        // flushes a second) are VCS's two, written natively as there: 0x23E630 -> FUN_0023E5B8 (CNT 3, FLUSHE, UNPACK V4-32 x3\n        // -> vu: VCS FUN_001CEF18), 0x356190 -> FUN_00356110 when src+0x90 is set (CNT 9, UNPACK x9, clears +0x90: VCS\n        // FUN_0040F6C8); any other callback runs the original.
        // VCS_RSL_NATIVE_VERIFY=1 compares it with the original; LCS_RSL_NATIVE=0 / VCS_RSL_NATIVE=0: the original.
        if (!off("LCS_RSL_NATIVE"))
        {
            const vcs::rslnative::FlushAddrs f = {0x002A02F0u, kPacketBuffer, 0x000E6670u, 0x003D7078u, 0x003939D8u, 0x0065EFD8u,
                                                  0x003D70C0u, 0x0066AC60u, 0x003DA0CCu, 0x0066AC90u, 0x0043DA70u, 0x0023E630u, 0x00356190u};
            if (vcs::rslnative::installOther(runtime, f))
                std::fprintf(stderr, "[lcs-rsl] state flush 0x2A02F0 native\n");
        }
        if (off("LCS_RSL"))
        {
            std::fprintf(stderr, "[lcs-rsl] off (LCS_RSL=0): every draw through VIF1 as the packet path\n");
            return;
        }
        // Without the write watch (Android, Linux) every packet is still copied by the game thread and checked against its
        // recording on the worker, and the observed chain walk costs the game thread more: measured on the PC with
        // PS2X_WRITE_WATCH=0 a loss (Portland 400%: 56 -> 43 fps, the chain submission +1-3 ms). Off there unless
        // LCS_RSL=1 (to measure on a phone).
        if (!ps2writewatch::enabled() && !on("LCS_RSL"))
        {
            std::fprintf(stderr, "[lcs-rsl] off: no write watch on this platform (LCS_RSL=1 turns it on)\n");
            return;
        }
        vcs::rslworld::GameAddrs a{};
        a.packetLo = kPacketBuffer;
        a.packetHi = kPacketEnd;
        a.worldDraw = 0u;
        a.modelDraw = kModelDraw;
        a.modelDrawB = 0u;
        a.models = !off("LCS_RSL_MODELS");
        a.rasters = on("LCS_RSL_RASTERS");
        a.skip = !off("LCS_RSL_SKIP");
        g_kickNext = runtime.lookupFunction(kFrameKick);
        if (!g_kickNext)
        {
            std::fprintf(stderr, "[lcs-rsl] frame kick 0x%x not in the function map: off\n", kFrameKick);
            return;
        }
        // The texture pixel REFs stay chain images (PERFORMANCE.md 34: interned, the GS gets them by reference) with the
        // observer installed - only outside the marked packets (runtime/ps2_chain_observer.h). LCS_RSL_CHAIN_IMAGES=0:
        // copied into the chain as VCS's are (the render worker then copies ~5 MB of pixels a frame in Fort Staunton).
        const bool images = !off("LCS_RSL_CHAIN_IMAGES");
        ps2chain::setChainImagesWithObserver(images);
        if (!vcs::rslworld::installOther(runtime, a))
        {
            ps2chain::setChainImagesWithObserver(false);
            std::fprintf(stderr, "[lcs-rsl] not installed (PS2X_HLE=0 / PS2X_HLE_VERIFY / the HLE survey, or every part off)\n");
            return;
        }
        runtime.replaceFunction(kFrameKick, &frameKick);
        // the walk with the replay's observers may run on the asynchronous walk's thread (no draw items here: the observers
        // touch only the walk's state; the frame kick above waits for the walk before the draw list changes hands)
        ps2chain::setAsyncWithObserver(!off("LCS_RSL_ASYNC"));
        std::fprintf(stderr, "[lcs-rsl] model / world packets recorded once and replayed (VCS_RSL_WORLD_VERIFY=1 checks); "
                             "unchanged ones %s; raster names %s; chain images %s\n",
                     vcs::rslworld::skipping() ? "not copied (write watch)" : "copied", a.rasters ? "on" : "off", images ? "on" : "off");
    }
}
