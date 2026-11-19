// Liberty City Stories (USA, SLUS_214.23 v1.01) bring-up: the few game-specific bindings it needs to get past boot,
// applied only to that ELF (CMake VCS_GAME=lcs builds; a VCS build never loads it). None of VCS's hooks apply here.
#include "game/game_functions.h"
#include "game/game_table.h"
#include "game/lcs_aim.h"
#include "game/lcs_audio.h"
#include "game/lcs_carcam.h"
#include "game/lcs_crossover.h"
#include "game/lcs_framerate.h"
#include "game/lcs_effects_timing.h"
#include "game/lcs_music_timing.h"
#include "game/lcs_particle_timing.h"
#include "game/lcs_physics_timing.h"
#include "game/lcs_world_timing.h"
#include "game/lcs_script_timing.h"
#include "game/lcs_display.h"
#include "game/lcs_dma_sync.h"
#include "game/lcs_inflate.h"
#include "game/lcs_drawdist.h"
#include "game/lcs_driveby.h"
#include "game/lcs_frame.h"
#include "game/dualworld.h" // [dualworld]
#include "game/lcs_freecam.h"
#include "game/lcs_input.h"
#include "game/lcs_frontend.h"
#include "game/lcs_population.h"
#include "game/lcs_prompts.h"
#include "game/lcs_save.h"
#include "game/lcs_swim.h"
#include "game/stories_hero.h"
#include "game/lcs_trainer.h"
#include "game/lcs_uaf_debug.h"
#include "game/lcs_world.h"
#include "game/lcs_render.h"
#include "game/lcs_rsl.h"
#include "game/lcs_sky.h"
#include "game/vcs_mods.h"
#include "game/vcs_stream.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"

#include <cstdio>
#include <string>

namespace
{
    // CDSTREAM's EE client (SIF RPC 0x65686577), as VCS's (game/vcs_stream.cpp): read (fn 1), poll (fn 2), fn 5 - each
    // checked against VCS's copy (0x221818 / 0x2218F0 / 0x221968). Through the emulated IOP the stream stalled after ~20
    // reads on the legal screen; read from the disc image on the host instead.
    // A started read returns 0 here, not VCS's 1: LCS's model streaming (0x18F788) re-issues its read until it gets 0
    // (with 1 it read LSN 0x129B58 in GTA3PS2.IMG forever on the loading screen); the file reader ignores the value.
    constexpr uint32_t kStreamRead = 0x001F6CA0u;
    constexpr uint32_t kStreamPoll = 0x001F6D78u;
    constexpr uint32_t kStreamFn5 = 0x001F6E50u;
    // fn 3 (sync: polled until 0 by CStreaming::LoadAllRequestedModels 0x18F648, called by FlushChannels 0x18F918) and
    // fn 4 (CdStreamGetLastPosn, read by RequestModelStream 0x18F108 / 0x18F648); not installed for VCS
    constexpr uint32_t kStreamSync = 0x001F6EA0u;
    constexpr uint32_t kStreamLastPosn = 0x001F6DE8u;

    // The runtime's own guest allocations (guestMalloc: sceGsResetGraph's packet, IOP host buffers...) come from the
    // heap crt0 gives SetupHeap - the end of .bss (0x673168). LCS's CMemoryHeap starts at that same address (main's
    // 0x14F2F8 builds it from _end), so the runtime's packets landed in the game's first allocations (a vector at
    // 0x673190 got the GS reset's words, a refcounted pointer became 1, free(1) wrecked the heap's free tree and every
    // later allocation failed: fread got buffer 1, the boot hung on the legal screen). On a PS2 nothing else lives
    // there. Here the runtime's heap moves below the ELF (0x80000-0xC0000: the kernel-reserved area the game never
    // touches; the runtime's callback stacks are carved down from 0x100000), right after crt0's
    // SetupHeap: crt0 calls 0x3836B0 next.
    constexpr uint32_t kAfterSetupHeap = 0x003836B0u;
    constexpr uint32_t kRuntimeHeapBase = 0x00080000u;
    constexpr uint32_t kRuntimeHeapLimit = 0x000C0000u;

    // The runtime's other guest-RAM pools - SIF RPC packets and server descriptors, TLS, boot-mode entries - sit at
    // 0x1F00000-0x1F31000 (upstream assumes the game's heap ends below). LCS's heap runs up to its stack, so models
    // were loaded there, and every SIF RPC call (the sound driver's, several a second) rewrote the dummy server
    // descriptors 0x1F10000 / 0x1F10080 / 0x1F10100 - client, size, recvbuf... - and copied its send data (16 / 1444
    // bytes) to the descriptor's "buf", now whatever the game kept there: random heap and .data writes. Skipping the
    // opening cutscene put RenderWare clumps / frames there: their lists broke and the game froze (atomic / frame
    // walks 0x20796C / 0x31E820 on NULL links). The pools move to the kernel area too (0x40000-0x71000, below the
    // runtime heap).
    constexpr uint32_t kRuntimePoolBase = 0x00040000u;

    PS2_GUEST_CC void afterSetupHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->configureGuestHeap(kRuntimeHeapBase, kRuntimeHeapLimit);
        std::fprintf(stderr, "[lcs] runtime guest heap at 0x%x-0x%x (the game's heap starts at .bss end)\n",
                     kRuntimeHeapBase, kRuntimeHeapLimit);
        ps2GameFunction<kAfterSetupHeap>(rdram, ctx, runtime);
    }

    void applyLcs(PS2Runtime &runtime)
    {
        game::setCurrent(game::Id::Lcs); // the per-game address tables (game/game_table.h) pick LCS's
        std::fprintf(stderr, "[lcs] Liberty City Stories bring-up bindings\n");
        ps2_syscalls::setRuntimePoolBase(kRuntimePoolBase); // before the first SifBindRpc
        runtime.replaceFunction(kAfterSetupHeap, &afterSetupHeap);
        const bool sound = lcs::installAudio(runtime); // sound and the boot movies (game/lcs_audio.h)
        const bool stream = vcs::installStreamHooks(runtime, kStreamRead, kStreamPoll, kStreamFn5, 0u) &&
                            vcs::installStreamSyncHooks(runtime, kStreamSync, kStreamLastPosn);
        // With the sound servers and every stream call but the server's init (fn 0 from 0x1F6AB0: 16 bytes sent, no
        // reply - VCS's too, answered in retireIop) on the host, nothing needs CDSTREAM.IRX on the emulated IOP: it is
        // switched off as for VCS (no IRX module loaded or run). PS2X_IOP=1 keeps it on (the reference).
        vcs::retireIop(sound && stream);
        lcs::frontend::install(runtime); // the pause menu with Enter / Escape (game/lcs_frontend.h)
        lcs::carcam::install(runtime);   // the mouse on the camera behind a vehicle (game/lcs_carcam.h)
        lcs::display::install(runtime); // widescreen: Hor+ / Vert+, culling, HUD (game/lcs_display.h)
        lcs::world::install(runtime);   // draw distance, background pause (game/lcs_world.h); after display
        lcs::drawdist::install(runtime); // far plane / fog, scan range, guards, fast streaming (game/lcs_drawdist.h)
        lcs::population::install(runtime); // traffic / pedestrian density, cars in view kept (game/lcs_population.h)
        lcs::aim::install(runtime);      // free aim with the mouse (game/lcs_aim.h)
        lcs::driveby::install(runtime);  // drive-by aiming with the mouse (game/lcs_driveby.h)
        lcs::render::install(runtime); // the renderer's LCS settings and native paths (game/lcs_render.h)
        lcs::sky::install(runtime);    // the backdrop below the horizon in the fog colour: no dark specks in fog (game/lcs_sky.h)
        lcs::frame::install(runtime);   // the perf overlay's frame mark, the rig's frames and jobs (game/lcs_frame.h)
        lcs::dmasync::install(runtime); // heap frees / moves wait for the VIF1 chain walk in flight (game/lcs_dma_sync.h)
        lcs::inflate::install(runtime); // the game's zlib inflate_fast natively: teleports / loads (game/lcs_inflate.h)
        lcs::rsl::install(runtime);     // model / building packets recorded and replayed (game/lcs_rsl.h); after frame
        lcs::save::install(runtime);    // save anywhere, quick save / load (game/lcs_save.h)
        lcs::trainer::install(runtime);  // the trainer menu and `trainer` command (game/lcs_trainer.h)
        lcs::prompts::install(runtime);  // Button prompts: the player's controls in help text (game/lcs_prompts.h)
        lcs::framerate::install(runtime); // frame-rate cap and independent timing (game/lcs_framerate.h)
        lcs::physics_timing::install(runtime); // rolling resistance on the time step (game/lcs_physics_timing.h)
        lcs::input::install(runtime);    // the driving vibration (game/lcs_input.h)
        lcs::freecam::install(runtime);  // the trainer's free camera (game/lcs_freecam.h)
        lcs::swim::install(runtime);     // swimming instead of drowning, by its setting (game/lcs_swim.h)
        lcs::crossover::install(runtime); // [crossover] VCS vehicles on new model ids (game/lcs_crossover.h)
        dualworld::install(runtime);     // [dualworld] STORIES_DUALWORLD only: two games shown as one world (game/dualworld.h)
        lcs::music_timing::install(runtime);   // the radio off the air on the time step (game/lcs_music_timing.h)
        lcs::particle_timing::install(runtime); // particles' fades and emitters at the PS2's rate (game/lcs_particle_timing.h)
        lcs::effects_timing::install(runtime);  // lightning, clouds, shake, explosions ... on PS2 frames (game/lcs_effects_timing.h)
        lcs::world_timing::install(runtime);    // scroll signs, water drift at the PS2's rate (game/lcs_world_timing.h)
        lcs::script_timing::install(runtime);   // mission meters on the time step (game/lcs_script_timing.h)
        lcs::uafdebug::install(runtime); // debug: heap-free ring + element checks, LCS_UAF_LOG=1 only (lcs_uaf_debug.cpp)
        stories::hero::install(runtime); // [hero] STORIES_HERO=vic: Vic Vance as the player (game/stories_hero.h)
        vcs::mods::install(runtime); // the mod loader with LCS's addresses: disc files, streaming entries (game/vcs_mods.h)
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Liberty City Stories (SLUS-21423)", "SLUS_214.23", 0x001C6A60u, 0u, &applyLcs)
