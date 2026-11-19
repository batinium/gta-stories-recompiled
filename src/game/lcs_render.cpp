// Liberty City Stories: the renderer's per-game settings and the native (HLE) render paths that apply to LCS (docs/ANDROID.md
// "LCS renderer").
//
// What LCS shares with VCS (measured, SLUS_214.23 vs SLUS_215.90): every VU1 microcode upload VCS makes is in LCS's ELF
// byte for byte except one (VCS 0x3ABAA0 -> LCS 0x153110: the 4-bone skinning routine at VU 0x2D28). So the world HLE
// (game/vcs_hle_world.cpp) already drew LCS's prelit world natively; what differs is listed per item below.
#include "game/lcs_render.h"

#include "game/vcs_hle_world.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_control.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
    uint64_t fnv(const uint8_t *p, uint32_t from, uint32_t to, uint64_t h)
    {
        for (uint32_t i = from; i < to; ++i)
            h = (h ^ p[i]) * 0x100000001B3ull;
        return h;
    }

    // vcs_hle_world.cpp's code check ranges with the last one cut at the skinning routine (VU 0x2D28): everything the
    // lit / rigid reproductions run but the skinning stage (stages 0x0da, 0x4ff, 0x09c, 0x2b6, entries 0x0e9 / 0x0de)
    uint64_t routinesHash(const uint8_t *code)
    {
        static const uint32_t ranges[][2] = {{0x30, 0x118}, {0x6D0, 0x748}, {0x748, 0x7E0}, {0x7F0, 0xC20}, {0xC38, 0x1030},
                                             {0x1048, 0x11F8}, {0x1360, 0x13D8}, {0x15B0, 0x1638}, {0x2700, 0x2D28}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        return h;
    }

    // LCS's VU1 images for the lit pipelines, checked against VCS's (scratch tool: the images rebuilt from both ELFs'
    // uploads, compared with each other and with VU1 code dumps of LCS runs):
    // - rigid lit (VCS: base 0x2AC130.., lit-pass trio 0x368730 / 0x368B60 / 0x368F60, 0x3ABAA0 + 0x3AC2A0, 0x2D08F0,
    //   0x2F13D0 at 0x2700; LCS: 0x24B5A0.., 0x2D4790 / 0x2D4BC0 / 0x2D4FC0, 0x153110 + 0x153910, 0x2E2AA0, 0x272740):
    //   identical but 0x2F70-0x3720 (the skinning routine and what follows it, which table 0x19 never calls);
    // - skinned lit (VCS: trio 0x289360 / 0x289740 / 0x289AF0, 0x3ABAA0 + 0x3AC2A0, 0x2F18C0 at 0x2700; LCS: 0x22F3B0 /
    //   0x22F790 / 0x22FB40, 0x153110 + 0x153910, 0x272C30): identical but the skinning routine 0x2D28-0x3528. LCS's
    //   routine is VCS's software-pipelined path for more than 6 vertices (VCS 0x2F98) moved up to 0x2DA0 and taken for
    //   every packet, the count raised to at least 6 (`ISUBIU vi08, vi05, 6; IBGTZ; IADDIU vi05, vi00, 6`): the same
    //   arithmetic per vertex (PS2X_HLE_VERIFY: bit-exact), but at least 6 vertices written back - over the next buffer
    //   for a smaller packet, which then draws differently (verify: all its packets off): those stay on the VU1.
    constexpr uint64_t kRigidRoutines = 0x697F8E7734FCE35Eull; // routinesHash of VCS's rigid image = LCS's
    constexpr uint64_t kLitRoutines = 0xC8F56AF031C3CC86ull;   // routinesHash of VCS's skinned image = LCS's
    constexpr uint64_t kLcsSkin = 0x2F70862EF13B21A1ull;       // fnv of LCS's skinning routine, VU 0x2D28-0x3528

    bool lcsLitCode(vcs::hleworld::Pipeline pipeline, const uint8_t *code)
    {
        if (pipeline == vcs::hleworld::Pipeline::Rigid)
            return routinesHash(code) == kRigidRoutines;
        return routinesHash(code) == kLitRoutines && fnv(code, 0x2D28, 0x3528, 0xCBF29CE484222325ull) == kLcsSkin;
    }
}

namespace lcs::render
{
    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("LCS_RENDER");
        if (e && *e == '0')
        {
            std::fprintf(stderr, "[lcs-render] off (LCS_RENDER=0)\n");
            return;
        }
        // Textures: LCS sends each texture before its draw into one shared VRAM area (base 13592) - level 0 as CT32 /
        // CT16 (a T8 / T4 texture in another format), then its mip levels and the CLUT (13584) into the same pages. The
        // page generations changed on every upload, so each draw's texture was decoded again (~1,100 a frame in the
        // opening cutscene's office: the GS stage at ~50 ms a frame). Keyed by the blocks it reads instead
        // (gsHwSetBlockContentKeys). PS2X_HW_BLOCK_KEYS=0: as before.
        gsHwSetBlockContentKeys(true);
        // Lit models (peds, vehicles, props): VCS's C++ reproductions of the skinned and rigid lit pipelines, for LCS's
        // images of the same routines (above). PS2X_HLE_VERIFY=1 checks them against the VU1 as for VCS; LCS_HLE_LIT=0
        // leaves them on the VU1.
        const char *lit = std::getenv("LCS_HLE_LIT");
        const bool litNative = !(lit && *lit == '0');
        if (litNative)
            vcs::hleworld::setOtherGameCode(&lcsLitCode, 6u);
        // tests: `lcsrender lit 0|1` switches the lit models between the VU1 and the native path while the game runs, for
        // an A/B of the same frame (a parked car under the free camera)
        ps2control::addCommand("lcsrender", [](const std::string &args, ps2control::Reply reply) {
            if (args.rfind("lit ", 0) == 0)
            {
                const bool on = args.size() > 4 && args[4] == '1';
                vcs::hleworld::setOtherGameCode(on ? &lcsLitCode : nullptr, 6u);
                reply(on ? "ok lit native" : "ok lit on the VU1");
                return;
            }
            reply("error usage: lcsrender lit 0|1");
        });
        // The flip (0x2D9640, VCS's FUN_003706c8 instruction for instruction) reads GS CSR at 0x2D9678 for the FIELD bit
        // alone (`ld v1, 0(a0)`, a0 = 0x12001000; VCS 0x370714). FIELD is set at VBlank on the game thread: no need to wait
        // for the render worker to finish the frame's GS work - that fence held the game thread ~40% of a frame in
        // gameplay (sample: FUN_002d9640 -> read64 -> gsDrain), the game and the renderer taking turns instead of
        // overlapping. LCS_CSR_FENCE=1: fence as before.
        const bool csrFence = std::getenv("LCS_CSR_FENCE") != nullptr;
        if (!csrFence)
            runtime.addCsrReadWithoutFence(0x002D9678u);
        std::fprintf(stderr, "[lcs-render] textures keyed by their blocks' upload; lit models %s; flip %s\n",
                     litNative ? "native (HLE)" : "on the VU1", csrFence ? "fences" : "without the CSR fence");
    }
}
