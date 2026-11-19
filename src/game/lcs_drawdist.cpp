// [lcs] See lcs_drawdist.h. Addresses: SLUS_214.23 (LCS USA v1.01); each with the instructions it was read from (the
// generated code's disassembly comments and Ghidra decompiles of the same functions); docs/game-internals.md 12e.
//
// What limits the view (measured, docs/LCS.md "Draw distance"):
// - The world itself (buildings, roads, the islands' LODs) is the level's hex-cell draw lists (lcs_world: one cell of
//   the WRLD image at a time, FUN_002007B0), drawn to the camera's far plane. The far plane is the timecycle's far clip
//   (*0x3D4AE8 + 0x2B8C, about 1,170 m at noon), set every frame at 0x1F5DD8, the fog start (+0x2B90) at 0x1F5DF0.
// - Objects (props, trees, lamp posts, fences: entities with their own models) are drawn when CRenderer::ScanWorld
//   (FUN_002CAA48) puts their 40 m sector in the scan - 300 m ahead at most - and SetupEntityVisibility (FUN_002CB3A0)
//   finds them inside their model's LOD distance x LODDistMultiplier (TheCamera+0x170, lcs_world's scale). A model
//   not in memory is requested (return 3 -> RequestModel FUN_0018B400 from ScanSectorList FUN_002CBE70) once the
//   entity is within that distance + 50 m.
// - Model streaming (CStreaming::Update FUN_0018AE68 -> FUN_0018F5C8) finishes one read and starts the next once a
//   frame, one model per read (FUN_0018F108): about 30 models a second at 30 fps. With the host's instant reads that,
//   not the disc, is what makes objects appear late.
#include "game/lcs_drawdist.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_host_std.h"
#include "runtime/ps2_perf.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace lcs::drawdist
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        // ---- far plane and fog
        // RwCameraSetFarClipPlane FUN_001006A0 (`swc1 $f12, 0x7C(camera)`), its per-frame call at 0x1F5DD8 with the
        // timecycle's far clip (`lwc1 $f12, 0x2B8C(*0x3D4AE8)`), returning to 0x1F5DE0
        constexpr uint32_t kSetFar = 0x001006A0u, kSetFarRet = 0x001F5DE0u;
        // RwCameraSetFogDistance FUN_00100748 (`swc1 $f12, 0x80(camera)`), called at 0x1F5DF0 with the timecycle's fog
        // start (+0x2B90), returning to 0x1F5DF8
        constexpr uint32_t kSetFog = 0x00100748u, kSetFogRet = 0x001F5DF8u;
        // The VU projection / clip planes FUN_0029D790(tan f12, aspect f13, near f14 = camera+0x78, far f15 = the
        // constant 2000: `lui $at, 0x44FA` at 0x100348) from RwCameraBeginUpdate (camera in $s1), returning to 0x100358 -
        // VCS's FUN_002F57A0 from 0x18F088, instruction for instruction
        constexpr uint32_t kProjection = 0x0029D790u, kProjectionRet = 0x00100358u;
        // The fog's VU parameters FUN_0029DFD8(fog start f12, fog end f13): a = -255 / (end - start) (`lui 0xC37F`),
        // b = (start / (end - start) + 1) x 255 - the per-vertex F = a x w + b (vcs_hle_world.cpp's fogp .w / .z, qword
        // 53), 255 at the fog start, 0 (all fog colour, GS FOGCOL from 0x3D96C8 via FUN_0029DF88 just before) at the
        // end. RwCameraBeginUpdate calls it at 0x1003B4 with f13 = camera+0x7C (the far clip) - returning to 0x1003BC:
        // the fog's end is the far plane, but nothing else reads this f13, so the haze can end elsewhere
        constexpr uint32_t kFogParams = 0x0029DFD8u, kFogParamsRet = 0x001003BCu;
        constexpr float kVuFar = 2000.0f;
        constexpr float kFarMax = 6000.0f; // the whole map fits (the islands span about 4 km)

        // ---- the entity scan
        // RwV3dTransformPoints FUN_00144CF0(out a0 = sp, in a1 = sp, 9, the camera's LTM) from ScanWorld at 0x2CAE30
        // (returns to 0x2CAE38): the camera-space points sp+0 (eye), +0x10..+0x40 (the far corners, at the far plane),
        // +0x50 / +0x60 (two far corners scaled to 300 m: `lui 0x4396` x 1/far) and +0x70 / +0x80 (60 m); far in $f20.
        // Past 300 m the scan polygons are (eye, +0x70, +0x80) for the priority list and (eye, +0x50, +0x60)
        constexpr uint32_t kTransform = 0x00144CF0u, kTransformRet = 0x002CAE38u;
        constexpr float kScanCap = 300.0f;

        // ---- SetupEntityVisibility FUN_002CB3A0 (entity a0; 0 invisible, 1 visible, 2 off screen, 3 stream it)
        constexpr uint32_t kVisibility = 0x002CB3A0u;
        // ScanSectorList's render lists: entities with 1 go to 0x66C6C0[*0x3DA0E4] or 0x66E600[*0x3DA0E8] (alpha),
        // 2,000 entries each (0x66C6C0 + 2000 x 4 = 0x66E600, + 2000 x 4 = 0x670540, the next list); never bounded
        constexpr uint32_t kVisibleCount = 0x003DA0E4u, kAlphaCount = 0x003DA0E8u;
        constexpr uint32_t kListCapacity = 2000u, kListGuard = 1900u;
        constexpr uint32_t kCamPos = 0x0060CF50u;      // the scan's camera position (ScanWorld copies it there)
        constexpr uint32_t kModelInfos = 0x003D8720u;  // CModelInfo::ms_modelInfoPtrs (`lw (id x 4 + *(gp+0x1830))`)
        constexpr uint32_t kModelCount = 0x003D9BA0u;  // its size, compared before every lookup
        constexpr uint32_t kLodMult = 0x0043BD60u;     // TheCamera+0x170 LODDistMultiplier (GetAtomicFromDistance 0x2E9258)

        // ---- the level's object list. Objects and dummies (entity type +0x40 bits 33-35 = 4 / 5) whose model is one of
        // the level's own resources (FUN_002043D0: a binary search of the WRLD's table, *(level+0x14)+0x30C, 0x30 bytes
        // an entry) are not drawn by the LOD distance: SetupEntityVisibility hands them to FUN_002046A0(level a0,
        // resource entry a1, entity a2), which queues them only within 100 m of the camera (`lui 0x461C; ori 0x4000`
        // = 10000.0 compared with the squared distance at 0x204744) and 300 a frame (`slti 0x12C`, level+0x1D60).
        // That 100 m is where props, lamp posts, signs... appear. Replaced (below) by the same code with 100 m x the
        // scale; at 100% the original runs.
        constexpr uint32_t kLevelAdd = 0x002046A0u;
        constexpr uint32_t kLevelScratch = 0x0043EA8Cu; // a pointer the function moves down 0x400 an object (0x204788)
        constexpr uint32_t kLevelFrameValue = 0x003DA0D0u; // FUN_0029F250 returns it (`lw v0, 0x31E0(gp)`)
        constexpr uint32_t kWorldOffset = 0x006199C0u; // three floats FUN_00204508 adds to the object's position
        constexpr uint32_t kTheCamera = 0x0043BBF0u;   // +0x30 the position FUN_002046A0 measures from
        constexpr int kLevelMax = 300;
        constexpr int kLevelFarMax = 200; // objects past 100 m: 100 entries stay for the near ones (20-40 seen at 100%)

        // ---- past the 300: a second list. The 300 entries (level+0x1D64, 0x10 each) end where the per-type list heads
        // begin (level+0x3024 = 0x1D64 + 300 x 0x10; s16 indices, 7 types, type 6 the alpha one - FUN_002044B8 resets
        // 0x3024..0x3032 and the count), so the list cannot grow in place. FUN_002007B0 (the level's draw) walks each
        // type's chain `for (i = head[t]; i >= 0; i = entry[i].next)`: types 0-5 call FUN_00204460(level a0, entry a1)
        // for every entry (`jal` at 0x201F48 / 0x202D38, returning to 0x201F50 / 0x202D40: the two branches of its cell
        // blend), type 6 appends a 12-byte record {entry+0, entry, 0x60 | (entry+0xC & 0xF), 0xFF, 0, 1} to the level's
        // alpha array (*(level+0x3034) .. *(level+0x3038), 0x3000 bytes = 1,024 records: FUN_00150E68(0x3000) at
        // 0x1FD718), which FUN_00205250(begin a0, end a1) sorts (`jal` at 0x202ECC, returning to 0x202ED4; the end is
        // read again from level+0x3038 after it) and the loop at 0x202EE4 draws, a record with byte 0xB = 1 through
        // FUN_00204460 too (0x203168). FUN_00204460 itself only reads entry+6 (the resource), +8 (the matrix), +0xC.
        // So the objects past the 300 get entries (and their 0x400 matrix block, as the game's) from the top of the
        // frame's DMA buffer, and are drawn: types 0-5 after the last entry of the same type's chain (FUN_00204460 as a
        // guest call returning to itself, once per extra entry), type 6 as records added to the alpha array before
        // the sort (at most 1,024 - 64 records in all). An opaque object goes to the second list only when its type
        // already has an entry in the first (the last 16 entries of the 300 are kept for the types still without one).
        constexpr uint32_t kLevelDraw = 0x00204460u;
        constexpr uint32_t kLevelDrawRetA = 0x00201F50u, kLevelDrawRetB = 0x00202D40u;
        constexpr uint32_t kLevelReset = 0x002044B8u;
        constexpr uint32_t kAlphaSort = 0x00205250u, kAlphaSortRet = 0x00202ED4u;
        constexpr uint32_t kAlphaBytes = 0x3000u, kAlphaRecord = 12u, kAlphaKeep = 64u;
        constexpr int kLevelTypes = 7;
        constexpr int kTypeReserve = 16;
        constexpr int kLevelTotalMax = 1000; // both lists: 300 + 700
        constexpr int kLevelNearKeep = 100;  // as kLevelFarMax: the last 100 for objects within the game's 100 m
        // the second list takes objects within 500 m only: past that a prop is a few pixels (screenshots with and
        // without the list at 1000% differed in 12-134 pixels) while each costs a draw (+2,000-3,000 draws, -8..-11
        // fps on the quick build in the first runs without this limit)
        constexpr float kOverRadius = 500.0f;
        // The DMA packet buffer 0x43EA70: two of 0xE6670 bytes (FUN_002B5698: start = base + 0x20 + index x 0xE6670);
        // +0x18 the packets written upwards, +0x1C the top, allocations downwards (the level's matrices, -0x50 blocks in
        // 0x1542A0...). Nothing checks the two meet. FUN_002B5400 (from RwCameraEndUpdate at 0x100714) sends the frame's
        // buffer: what is free there is what the frame left; the second list is filled only while the buffer keeps the
        // packets the rest of a frame wrote (measured, the largest of the last ~4 s) and 128 KB more
        constexpr uint32_t kDmaBase = 0x0043EA70u, kDmaWrite = kDmaBase + 0x18u, kDmaTop = kDmaBase + 0x1Cu;
        constexpr uint32_t kDmaKick = 0x002B5400u;
        constexpr int kDmaMargin = 128 * 1024;

        // ---- streaming
        // CStreaming's info table: *(0x3D0CC8) + id x 0x14, +0xC the state (1 loaded, 2 requested, 3 reading)
        constexpr uint32_t kStreamInfo = 0x003D0CC8u;
        constexpr uint32_t kNumRequested = 0x003D9D14u; // -- in RequestModelStream, ++ in RequestModel
        constexpr uint32_t kStreamError = 0x003D9D04u;  // -1, or the channel whose read failed
        constexpr uint32_t kChannelState = 0x00661928u; // + channel x 0x20: 0 idle, 1 reading, 2 error
        constexpr uint32_t kChannel = 0x003D0D58u;      // gp-0x6198: the channel FUN_0018F5C8 works on
        // FUN_0018F5C8: if the channel's read is in (state 1) process it (FUN_0018EAC0), then, idle and no error, start
        // the next one (RequestModelStream FUN_0018F108, one model). Its only caller: CStreaming::Update at 0x18AF88.
        constexpr uint32_t kLoadStep = 0x0018F5C8u;
        constexpr int kFastStepsMax = 24;
        constexpr double kFastBudgetMs = 3.0;
        // CMemoryHeap 0x634140 (MakeSpaceFor FUN_0018EA08: models come from it): free = +0x10 - +0xC - +0x14
        // (FUN_0014FE30)
        constexpr uint32_t kHeap = 0x00634140u;
        // Memory pressure: MakeSpaceFor (FUN_0018EA08) first removes unused models (RemoveLeastUsedModel FUN_0018C8A0,
        // refcount +0x1C < 1), then defragments (FUN_001916A8); when that is not enough it calls FUN_0018CAE0, which
        // deletes the RW objects of entities behind / far from the camera (VC's DeleteRwObjectsBehindCamera: the
        // objects drawn come back only when streamed again). The PS2 game runs with its heap nearly full (0.2-0.9 MB
        // free measured at 100%), so free memory is no signal; FUN_0018CAE0 being needed is.
        constexpr uint32_t kDeleteRwObjects = 0x0018CAE0u;

        struct Settings
        {
            std::atomic<int> draw{100};
            std::atomic<int> fog{1};       // 0 the PS2's far plane and fog, 1 with the draw distance, 2 + haze
            std::atomic<int> streaming{1}; // 0 PS2 pace (one model a frame), 1 fast
        } g_set;

        bool g_installed = false;
        bool g_guardOn = true; // LCS_DRAW_GUARD=0: no memory / list governor (testing)
        PS2Runtime::RecompiledFunction g_origFogParams = nullptr;
        float g_hazeK = 0.15f; // LCS_HAZE_K: the haze's end = timecycle far x (1 + (scale - 1) x k)
        std::atomic<float> g_stHaze{0.0f};
        PS2Runtime::RecompiledFunction g_origSetFar = nullptr, g_origSetFog = nullptr, g_origProjection = nullptr,
                                       g_origTransform = nullptr, g_origVisibility = nullptr, g_origLoadStep = nullptr,
                                       g_origDeleteRw = nullptr;
        std::atomic<uint32_t> g_pressure{0}; // FUN_0018CAE0 calls since the last frame
        std::atomic<int> g_stStepMaxUs{0}, g_stLoopMaxUs{0}; // fast streaming: longest extra step / frame's loop
        float g_slowMs = 40.0f;              // LCS_DRAW_SLOW_MS (0: frame time not watched)
        std::atomic<uint64_t> g_stSlow{0};
        std::atomic<float> g_stBusyAvg{0.0f};

        // game thread
        float g_effective = 1.0f; // the scale used this frame (<= the setting's)
        float g_farScale = 1.0f;
        uint32_t g_loadRa = 0;
        int g_loadSteps = 0;
        std::chrono::steady_clock::time_point g_loadStart{};
        uint64_t g_frame = 0;

        // pop-in counters: an object inside its draw distance whose model is not in memory is "waiting"; when it
        // draws, how far inside its draw distance it was and how long it waited
        struct Waiting
        {
            uint64_t since;
            float firstGap; // draw distance - distance when it was first seen waiting (m)
        };
        std::unordered_map<uint32_t, Waiting> g_waiting;

        // published for the command
        std::atomic<float> g_stEffective{1.0f}, g_stFar{0.0f}, g_stFog{0.0f}, g_stTcFar{0.0f};
        std::atomic<int> g_stHeapFree{0}, g_stHeapMin{1 << 30}, g_stVisible{0}, g_stAlpha{0}, g_stVisibleMax{0},
            g_stRequested{0}, g_stWaitingNow{0};
        std::atomic<uint64_t> g_stLoads{0}, g_stSteps{0}, g_stFrames{0}, g_stGuarded{0}, g_stPops{0}, g_stPopFrames{0},
            g_stWaitFrames{0}, g_stBackoffs{0}, g_stPressure{0};
        std::atomic<float> g_stPopGapSum{0.0f};

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        float target() { return std::clamp(g_set.draw.load(std::memory_order_relaxed), 100, kMaxPercent) / 100.0f; }
        bool scaled() { return g_effective > 1.0f; }

        void addFloat(std::atomic<float> &a, float v)
        {
            float cur = a.load(std::memory_order_relaxed);
            while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed))
            {
            }
        }

        // ---- far plane and fog: both times the draw distance (at most kFarMax), the VU's far with them
        PS2_GUEST_CC void setFar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kSetFarRet)
            {
                const float tc = ctx->f[12];
                g_stTcFar.store(tc, std::memory_order_relaxed);
                g_farScale = 1.0f;
                if (g_set.fog.load(std::memory_order_relaxed) && tc > 0.0f)
                    g_farScale = std::clamp(target(), 1.0f, std::max(1.0f, kFarMax / tc));
                if (g_farScale != 1.0f)
                    ctx->f[12] = tc * g_farScale;
                g_stFar.store(ctx->f[12], std::memory_order_relaxed);
            }
            g_origSetFar(rdram, ctx, runtime);
        }

        // the fog start: times the scale (with the haze: the PS2's own start, the haze then ends nearer than the far plane)
        PS2_GUEST_CC void setFog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kSetFogRet && g_farScale != 1.0f && g_set.fog.load(std::memory_order_relaxed) != 2)
                ctx->f[12] *= g_farScale;
            if (GPR_U32(ctx, 31) == kSetFogRet)
                g_stFog.store(ctx->f[12], std::memory_order_relaxed);
            g_origSetFog(rdram, ctx, runtime);
        }

        // "With draw distance + haze": the fog moved out with the far plane leaves far buildings in their own colours,
        // darker than the PS2's noon haze, where the PS2 had them all but fog. The haze (the timecycle's fog colour,
        // linear in depth as the PS2's) starts where the PS2's does and is complete at the timecycle far x
        // (1 + (far scale - 1) x 0.15): 1.45 x at 400%, 1.62 x at 1000% (the far scale stops at 6 km / 1,168 = 5.1;
        // 1,690 / 1,890 m at noon) - between the PS2's 1,170 m and the far plane; what is beyond is drawn in the fog
        // colour, as the PS2 draws its last metres. 0.15 chosen from screenshots (0.15 / 0.25 / 0.35, world2 notes):
        // the other island stays a hazy silhouette as the PS2 shows its nearer streets
        PS2_GUEST_CC void fogParams(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kFogParamsRet && g_farScale > 1.0f && g_set.fog.load(std::memory_order_relaxed) == 2)
            {
                const float tc = g_stTcFar.load(std::memory_order_relaxed);
                const float end = tc * (1.0f + (g_farScale - 1.0f) * g_hazeK);
                if (end > ctx->f[12] + 1.0f && end < ctx->f[13])
                    ctx->f[13] = end;
            }
            if (GPR_U32(ctx, 31) == kFogParamsRet)
                g_stHaze.store(ctx->f[13], std::memory_order_relaxed);
            g_origFogParams(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void projection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kProjectionRet && ctx->f[15] == kVuFar)
            {
                const float far = rdf(rdram, GPR_U32(ctx, 17) + 0x7Cu);
                if (far > kVuFar && far <= kFarMax * 1.01f)
                    ctx->f[15] = far;
            }
            g_origProjection(rdram, ctx, runtime);
        }

        // ---- the scan: its 300 m points (or the far corners, when the far plane is nearer) times the scale
        PS2_GUEST_CC void transform(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kTransformRet && GPR_U32(ctx, 6) == 9u && scaled())
            {
                const uint32_t in = GPR_U32(ctx, 5);
                const float far = ctx->f[20];
                const float s = g_effective;
                auto scale = [&](int point, float by) {
                    for (int i = 0; i < 3; ++i)
                    {
                        const uint32_t a = in + point * 0x10u + i * 4u;
                        wrf(rdram, a, rdf(rdram, a) * by);
                    }
                };
                if (far > kScanCap)
                {
                    // never past the far plane: 300 x s, at most `far`
                    const float by = std::min(s, far / kScanCap);
                    scale(5, by);
                    scale(6, by);
                }
            }
            g_origTransform(rdram, ctx, runtime);
        }

        uint32_t modelInfo(const uint8_t *ram, int id)
        {
            if (id < 0 || id >= static_cast<int>(rd32(ram, kModelCount)))
                return 0;
            return rd32(ram, rd32(ram, kModelInfos) + static_cast<uint32_t>(id) * 4u) & kRamMask;
        }

        // ---- SetupEntityVisibility: the list guard, and the pop-in counters (read only)
        PS2_GUEST_CC void visibility(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t e = GPR_U32(ctx, 4) & kRamMask;
            if (scaled() && (rd32(rdram, kVisibleCount) >= kListGuard || rd32(rdram, kAlphaCount) >= kListGuard))
            {
                // the lists are full: nothing more this frame (the game would write past them)
                g_stGuarded.fetch_add(1, std::memory_order_relaxed);
                SET_GPR_U32(ctx, 2, 0);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (e)
            {
                int16_t id;
                std::memcpy(&id, rdram + e + 0x58u, 2);
                const uint32_t info = modelInfo(rdram, id);
                const uint8_t type = info ? rdram[info + 0x10u] : 0;
                if (type == 1 || type == 3)
                {
                    uint64_t flags;
                    std::memcpy(&flags, rdram + e + 0x40u, 8);
                    const uint64_t kind = flags & 0xE00000000ull; // 0x8.. / 0xA..: drawn from the level's resources
                    const uint8_t n = rdram[info + 0x34u];
                    if (kind != 0x800000000ull && kind != 0xA00000000ull && n > 0 && n <= 3)
                    {
                        const float lod = rdf(rdram, info + 0x28u + (n - 1u) * 4u) * rdf(rdram, kLodMult);
                        const float dx = rdf(rdram, e + 0x30u) - rdf(rdram, kCamPos),
                                    dy = rdf(rdram, e + 0x34u) - rdf(rdram, kCamPos + 4u),
                                    dz = rdf(rdram, e + 0x38u) - rdf(rdram, kCamPos + 8u);
                        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const bool loaded = rd32(rdram, info + 0x24u) != 0;
                        if (dist < lod)
                        {
                            auto it = g_waiting.find(e);
                            if (!loaded)
                            {
                                if (it == g_waiting.end())
                                    g_waiting.emplace(e, Waiting{g_frame, lod - dist});
                                g_stWaitFrames.fetch_add(1, std::memory_order_relaxed);
                            }
                            else if (it != g_waiting.end())
                            {
                                g_stPops.fetch_add(1, std::memory_order_relaxed);
                                g_stPopFrames.fetch_add(g_frame - it->second.since, std::memory_order_relaxed);
                                addFloat(g_stPopGapSum, lod - dist);
                                g_waiting.erase(it);
                            }
                        }
                    }
                }
            }
            g_origVisibility(rdram, ctx, runtime);
        }

        // ---- fast streaming: FUN_0018F5C8 again as a guest tail call that returns to itself, while models are
        // waiting, up to kFastStepsMax times / kFastBudgetMs a frame (the read itself is the host's: done at once)
        PS2_GUEST_CC void loadStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (A step that ran to its end returns with pc = ra = kLoadStep: seen right after the call - the runtime's
            // call dispatch would take pc at the entry for "done" and go on at the caller. One stopped at an EE
            // checkpoint comes back through the scheduler, ra = kLoadStep. Before 2026-11-18 (world2) the loop relied
            // on the second way only and made one step a frame.)
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra != kLoadStep) // from CStreaming::Update: a frame's first step
            {
                g_stSteps.fetch_add(1, std::memory_order_relaxed);
                if (!g_set.streaming.load(std::memory_order_relaxed))
                    return g_origLoadStep(rdram, ctx, runtime);
                g_loadRa = ra;
                g_loadSteps = 1;
                g_loadStart = ps2x::hostNow();
                SET_GPR_U32(ctx, 31, kLoadStep);
                ctx->pc = kLoadStep;
                g_origLoadStep(rdram, ctx, runtime);
                if (ctx->pc != kLoadStep)
                    return; // stopped part-way
            }
            // back from a step
            for (;;)
            {
                const uint32_t channel = rd32(rdram, kChannel);
                const uint32_t state = channel < 2 ? rd32(rdram, kChannelState + channel * 0x20u) : 2u;
                const bool more = static_cast<int32_t>(rd32(rdram, kStreamError)) == -1 && state <= 1 &&
                                  (state == 1 || static_cast<int32_t>(rd32(rdram, kNumRequested)) > 0) &&
                                  g_loadSteps < kFastStepsMax &&
                                  std::chrono::duration<double, std::milli>(ps2x::hostNow() - g_loadStart).count() <
                                      kFastBudgetMs;
                if (!more)
                    break;
                ++g_loadSteps;
                g_stSteps.fetch_add(1, std::memory_order_relaxed);
                SET_GPR_U32(ctx, 31, kLoadStep);
                ctx->pc = kLoadStep;
                const auto t0 = ps2x::hostNow();
                g_origLoadStep(rdram, ctx, runtime);
                if (ctx->pc != kLoadStep)
                    return;
                // (droidperf2) the longest extra step that ran to its end, and the frame's loop: lcsdraw stepmax / loopmax
                const int us = static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(ps2x::hostNow() - t0).count());
                if (us > g_stStepMaxUs.load(std::memory_order_relaxed))
                    g_stStepMaxUs.store(us, std::memory_order_relaxed);
                const int loopUs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::microseconds>(ps2x::hostNow() - g_loadStart).count());
                if (loopUs > g_stLoopMaxUs.load(std::memory_order_relaxed))
                    g_stLoopMaxUs.store(loopUs, std::memory_order_relaxed);
            }
            SET_GPR_U32(ctx, 31, g_loadRa);
            ctx->pc = g_loadRa;
        }

        // FUN_002046A0 as the game has it (read instruction by instruction, 0x2046A0..0x204864, with FUN_00204508 and
        // FUN_002D7FB0 inlined), the 100 m radius times the scale. Same float operations in the same order.
        std::atomic<int> g_stLevelMax{0};
        std::atomic<uint64_t> g_stLevelFull{0}, g_stLevelFar{0}, g_stLevelCut{0};
        PS2Runtime::RecompiledFunction g_origLevelAdd = nullptr;

        // the second list (game thread)
        bool g_overOn = true;                     // LCS_LEVEL_LIST=0: the 300 only
        int g_levelTotalMax = kLevelTotalMax;     // LCS_LEVEL_MAX (tests)
        uint32_t g_overLevel = 0;                 // the level the lists below belong to
        uint32_t g_over[kLevelTypes][kLevelTotalMax]; // guest addresses of the extra entries, per type
        uint64_t g_overKey[kLevelTypes][kLevelTotalMax]; // what each held when written (resource << 32 | matrix)
        int g_overN[kLevelTypes] = {};
        int g_overTotal = 0;
        // The extras live in the frame's DMA buffer: valid from the levelAdd that made them to that buffer's send
        // (FUN_002B5400). The level is also drawn in frames with no scan and no reset (loading / fades after a
        // teleport): the 300 then still point into the level struct, but extras from a buffer since reused would be
        // garbage (a resource id read from packets: a jump through 0xFFFFFFFC in 0x2D9304 - the teleport freeze of
        // 2026-11-18). So they are drawn only while live, and each is checked against what was written
        bool g_overLive = false;
        // after a camera jump (teleport, cut, respawn) the first frames at the new place are the heaviest (the whole
        // view at once: 800 KB written after the scan was measured, the buffer then short by 166 KB with the extras)
        // and nothing measured yet says so: no extras for 2 s after the camera moves more than 50 m in a frame
        int g_overBlock = 0;
        bool g_jumped = false;
        int g_dmaLastSend = 1 << 30; // the least free at a send since the last onFrame
        float g_lastCam[3] = {0.0f, 0.0f, 0.0f};
        std::unordered_map<uint32_t, uint64_t> g_liveAlpha; // the alpha extras' entries -> key (the record draw checks)
        constexpr uint32_t kAlphaDrawRet = 0x00203170u;      // FUN_00204460 from the alpha record loop (jal 0x203168)
        std::atomic<uint64_t> g_stOverStale{0};

        uint64_t overKey(const uint8_t *ram, uint32_t e)
        {
            uint16_t res;
            std::memcpy(&res, ram + ((e + 6u) & kRamMask), 2);
            return (static_cast<uint64_t>(res) << 32) | rd32(ram, e + 8u);
        }

        void pushOver(const uint8_t *ram, int t, uint32_t e)
        {
            g_over[t][g_overN[t]] = e;
            g_overKey[t][g_overN[t]] = overKey(ram, e);
            if (t == 6)
                g_liveAlpha[e] = g_overKey[t][g_overN[t]];
            ++g_overN[t];
            g_overLive = true;
        }

        void dropOver()
        {
            for (int t = 0; t < kLevelTypes; ++t)
                g_overN[t] = 0;
            g_overTotal = 0;
            g_overLive = false;
            g_liveAlpha.clear();
        }
        int8_t g_baseType[kLevelMax];             // the type of each of the 300 (filled by levelAdd)
        uint32_t g_drawRa = 0, g_drawLevel = 0;   // a chain's tail drawing its extras: the caller, the level
        int g_drawType = -1, g_drawNext = 0;
        // the DMA buffer's room
        int g_dmaFreeNow = 0;                     // at the last levelAdd
        bool g_dmaAdded = false;                  // a levelAdd since the last kick
        struct Hist
        {
            uint64_t frame;
            int freeAdd, freeKick, overBytes, overTotal;
            uint32_t write, top;
            bool added;
        };
        Hist g_hist[64];
        uint32_t g_histN = 0;
        int g_overBytes = 0;
        int g_dmaAfterAdd = 256 * 1024;           // what a frame wrote after its last levelAdd (the most of ~4 s)
        int g_dmaAfterWin[4] = {}, g_dmaAfterSlot = 0;
        uint64_t g_dmaWinFrame = 0;
        PS2Runtime::RecompiledFunction g_origLevelDraw = nullptr, g_origLevelReset = nullptr, g_origAlphaSort = nullptr,
                                       g_origDmaKick = nullptr;
        std::atomic<int> g_stOverMax{0}, g_stDmaMin{1 << 30}, g_stDmaAfter{0}, g_stAlphaMax{0};
        std::atomic<uint64_t> g_stOverAdded{0}, g_stOverDrawn{0}, g_stOverAlpha{0}, g_stOverNoTail{0},
            g_stOverNoRoom{0};

        int dmaFree(const uint8_t *ram) { return static_cast<int>(rd32(ram, kDmaTop) - rd32(ram, kDmaWrite)); }

        // the second list's room this frame: the buffer must keep what the rest of the frame writes, and the margin
        bool dmaRoom(const uint8_t *ram, uint32_t bytes)
        {
            return dmaFree(ram) - static_cast<int>(bytes) > g_dmaAfterAdd + kDmaMargin;
        }

        uint32_t dmaAlloc(uint8_t *ram, uint32_t bytes)
        {
            g_overBytes += static_cast<int>(bytes);
            const uint32_t m = rd32(ram, kDmaTop) - bytes;
            std::memcpy(ram + (kDmaTop & kRamMask), &m, 4);
            return m;
        }

        // FUN_002044B8 (level a0): the 300 and the heads reset - the second list with them
        PS2_GUEST_CC void levelReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t level = GPR_U32(ctx, 4) & kRamMask;
            if (level == g_overLevel || !g_overLevel)
            {
                dropOver();
                g_overLevel = level;
            }
            g_origLevelReset(rdram, ctx, runtime);
        }

        // FUN_00204460 (level a0, entry a1): a chain's last entry draws its type's extras after it, each as a guest
        // call of the original returning here (ra = kLevelDraw); then back to FUN_002007B0
        // the extras of a chain, one guest call each, once the previous one returned here (pc = ra = kLevelDraw). The
        // runtime's call dispatch takes a replaced function returning with pc at its own entry for "done, go on at the
        // caller's return address", so a call that ran to its end is seen right after it, here; one that stopped at an
        // EE checkpoint comes back through the scheduler (levelDraw with ra = kLevelDraw)
        void levelDrawMore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            while (g_drawType >= 0 && g_drawNext < g_overN[g_drawType])
            {
                const int i = g_drawNext++;
                const uint32_t e = g_over[g_drawType][i];
                if (!g_overLive || overKey(rdram, e) != g_overKey[g_drawType][i])
                {
                    g_stOverStale.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                SET_GPR_U32(ctx, 4, g_drawLevel);
                SET_GPR_U32(ctx, 5, e);
                SET_GPR_U32(ctx, 31, kLevelDraw);
                g_stOverDrawn.fetch_add(1, std::memory_order_relaxed);
                ctx->pc = kLevelDraw;
                g_origLevelDraw(rdram, ctx, runtime);
                if (ctx->pc != kLevelDraw)
                    return; // stopped part-way
            }
            const uint32_t back = g_drawRa;
            g_drawRa = 0;
            g_drawType = -1;
            SET_GPR_U32(ctx, 31, back);
            ctx->pc = back;
        }

        PS2_GUEST_CC void levelDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == kLevelDraw && g_drawRa) // back from one through the scheduler
                return levelDrawMore(rdram, ctx, runtime);
            if (ra == kAlphaDrawRet)
            {
                // an alpha record's entry (0x203168): one of ours only while live and as written; the game's own are
                // in the level struct
                const uint32_t level = GPR_U32(ctx, 4) & kRamMask, e = GPR_U32(ctx, 5) & kRamMask;
                const uint32_t first = level + 0x1D64u;
                if (e < first || e >= first + kLevelMax * 0x10u)
                {
                    const auto it = g_liveAlpha.find(e);
                    if (!g_overLive || it == g_liveAlpha.end() || it->second != overKey(rdram, e))
                    {
                        g_stOverStale.fetch_add(1, std::memory_order_relaxed);
                        ctx->pc = ra;
                        return;
                    }
                }
            }
            if ((ra == kLevelDrawRetA || ra == kLevelDrawRetB) && g_overTotal > 0 && g_overLive)
            {
                const uint32_t level = GPR_U32(ctx, 4) & kRamMask, e = GPR_U32(ctx, 5) & kRamMask;
                int16_t next;
                std::memcpy(&next, rdram + e + 4u, 2);
                const uint32_t first = level + 0x1D64u;
                if (level == g_overLevel && next < 0 && e >= first && e < first + kLevelMax * 0x10u &&
                    (e - first) % 0x10u == 0)
                {
                    const int t = g_baseType[(e - first) / 0x10u];
                    if (t >= 0 && t < 6 && g_overN[t] > 0)
                    {
                        g_drawRa = ra;
                        g_drawLevel = level;
                        g_drawType = t;
                        g_drawNext = 0;
                        SET_GPR_U32(ctx, 31, kLevelDraw);
                        ctx->pc = kLevelDraw;
                        g_origLevelDraw(rdram, ctx, runtime); // the chain's own last entry
                        if (ctx->pc == kLevelDraw)
                            levelDrawMore(rdram, ctx, runtime);
                        return;
                    }
                }
            }
            g_origLevelDraw(rdram, ctx, runtime);
        }

        // FUN_00205250 (begin a0, end a1) from the level's draw: the alpha extras as records, before the sort
        PS2_GUEST_CC void alphaSort(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kAlphaSortRet)
            {
                const uint32_t level = GPR_U32(ctx, 19) & kRamMask; // $s3 (`lw a0, 0x3034($s3)` at 0x202EC8)
                const uint32_t begin = GPR_U32(ctx, 4) & kRamMask;
                uint32_t end = GPR_U32(ctx, 5) & kRamMask;
                if (level == g_overLevel && g_overLive && g_overN[6] > 0 && begin && end >= begin &&
                    rd32(rdram, level + 0x3034u) == GPR_U32(ctx, 4))
                {
                    const uint32_t cap = begin + kAlphaBytes - kAlphaKeep * kAlphaRecord;
                    for (int i = 0; i < g_overN[6] && end + kAlphaRecord <= cap; ++i)
                    {
                        const uint32_t e = g_over[6][i];
                        if (overKey(rdram, e) != g_overKey[6][i])
                            continue;
                        uint8_t rec[12];
                        std::memcpy(rec, rdram + (e & kRamMask), 4);      // +0: its distance
                        std::memcpy(rec + 4, &e, 4);                       // +4: the entry
                        rec[8] = static_cast<uint8_t>(0x60u | (rdram[(e & kRamMask) + 0xCu] & 0xFu));
                        rec[9] = 0xFF;
                        rec[10] = 0;
                        rec[11] = 1;                                       // drawn through FUN_00204460 (0x203158)
                        std::memcpy(rdram + end, rec, sizeof(rec));
                        end += kAlphaRecord;
                        g_stOverAlpha.fetch_add(1, std::memory_order_relaxed);
                    }
                    const uint32_t endGuest = (GPR_U32(ctx, 4) & ~kRamMask) | end;
                    std::memcpy(rdram + level + 0x3038u, &endGuest, 4);
                    SET_GPR_U32(ctx, 5, endGuest);
                }
                const int n = static_cast<int>((end - begin) / kAlphaRecord);
                if (n > g_stAlphaMax.load(std::memory_order_relaxed))
                    g_stAlphaMax.store(n, std::memory_order_relaxed);
            }
            g_origAlphaSort(rdram, ctx, runtime);
        }

        // FUN_002B5400 (the frame's DMA buffer sent): what it left free, and what was written after the last levelAdd
        PS2_GUEST_CC void dmaKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if ((GPR_U32(ctx, 4) & kRamMask) == kDmaBase)
            {
                const int free = dmaFree(rdram);
                g_dmaLastSend = std::min(g_dmaLastSend, free);
                {
                    Hist &h = g_hist[g_histN++ % 64];
                    h = Hist{g_frame, g_dmaFreeNow, free, g_overBytes, g_overTotal, rd32(rdram, kDmaWrite), rd32(rdram, kDmaTop),
                             g_dmaAdded};
                    g_overBytes = 0;
                    // this buffer is sent: its extras are not to be drawn again (the lists stay until the reset)
                    g_overLive = false;
                    g_liveAlpha.clear();
                    if (free < 64 * 1024)
                        std::fprintf(stderr, "[lcs-draw] DMA buffer nearly full at the send: %d bytes free (frame %llu, extra %d)\n",
                                     free, (unsigned long long)g_frame, g_overTotal);
                }
                if (free < g_stDmaMin.load(std::memory_order_relaxed))
                    g_stDmaMin.store(free, std::memory_order_relaxed);
                if (g_dmaAdded)
                {
                    g_dmaAdded = false;
                    const int after = std::max(0, g_dmaFreeNow - free);
                    int &slot = g_dmaAfterWin[g_dmaAfterSlot];
                    slot = std::max(slot, after);
                    if (++g_dmaWinFrame % 60u == 0) // four windows of 60 kicks: the most of the last 3-4 s
                    {
                        g_dmaAfterSlot = (g_dmaAfterSlot + 1) % 4;
                        g_dmaAfterWin[g_dmaAfterSlot] = 0;
                    }
                    g_dmaAfterAdd = *std::max_element(std::begin(g_dmaAfterWin), std::end(g_dmaAfterWin));
                    g_stDmaAfter.store(g_dmaAfterAdd, std::memory_order_relaxed);
                }
            }
            g_origDmaKick(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void levelAdd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t level = GPR_U32(ctx, 4) & kRamMask, entry = GPR_U32(ctx, 5) & kRamMask,
                           entity = GPR_U32(ctx, 6) & kRamMask;
            const int count = static_cast<int32_t>(rd32(rdram, level + 0x1D60u));
            if (count > g_stLevelMax.load(std::memory_order_relaxed))
                g_stLevelMax.store(count, std::memory_order_relaxed);
            const float dx = rdf(rdram, entity + 0x30u) - rdf(rdram, kTheCamera + 0x30u);
            const float dy = rdf(rdram, entity + 0x34u) - rdf(rdram, kTheCamera + 0x34u);
            const float dz = rdf(rdram, entity + 0x38u) - rdf(rdram, kTheCamera + 0x38u);
            float d2 = dx * dx;
            d2 = d2 + dy * dy;
            d2 = d2 + dz * dz;
            const float radius = 100.0f * g_effective;
            if (radius * radius < d2) // in view, in the scan, but past the radius: not drawn (yet)
                g_stLevelCut.fetch_add(1, std::memory_order_relaxed);
            if (!scaled())
                return g_origLevelAdd(rdram, ctx, runtime);
            ctx->pc = GPR_U32(ctx, 31);
            if (!g_overLevel || (g_overTotal == 0 && level != g_overLevel))
                g_overLevel = level;
            const bool overOk = g_overOn && level == g_overLevel && g_overBlock == 0;
            const int total = count + (overOk ? g_overTotal : 0);
            const int totalMax = overOk ? g_levelTotalMax : kLevelMax;
            if (total >= totalMax)
            {
                g_stLevelFull.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (radius * radius < d2)
                return;
            if (10000.0f < d2)
            {
                // past the game's 100 m only while the list keeps room for the near ones: the scan's order is not by
                // distance (beyond its 60 m priority list), and an object the game draws must never be pushed out
                if (total >= (overOk ? totalMax - kLevelNearKeep : kLevelFarMax))
                {
                    g_stLevelFull.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                g_stLevelFar.fetch_add(1, std::memory_order_relaxed);
            }
            auto wr16 = [&](uint32_t a, uint16_t v) { std::memcpy(rdram + (a & kRamMask), &v, 2); };
            auto rd16 = [&](uint32_t a) {
                uint16_t v;
                std::memcpy(&v, rdram + (a & kRamMask), 2);
                return v;
            };
            auto wr32 = [&](uint32_t a, uint32_t v) { std::memcpy(rdram + (a & kRamMask), &v, 4); };
            const uint8_t type = rdram[entry + 0x1Du];
            const uint16_t second = type == 6 ? 0 : rd16(entry + 0x1Eu);
            const int need = second ? 2 : 1;
            g_dmaFreeNow = dmaFree(rdram);
            g_dmaAdded = true;
            // the first list while it has room (its last kTypeReserve entries only for a type with no entry yet), else
            // the second: an opaque object only when its type's chain has a last entry to draw it after
            bool base = count + need <= kLevelMax - kTypeReserve;
            if (!base && count + need <= kLevelMax && type < kLevelTypes)
                base = rd16(level + 0x3024u + type * 2u) == 0xFFFFu || (second && rd16(level + 0x3030u) == 0xFFFFu);
            if (!base)
            {
                if (!overOk || type >= kLevelTypes)
                {
                    g_stLevelFull.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (d2 > kOverRadius * kOverRadius)
                {
                    g_stLevelFull.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (type != 6 && rd16(level + 0x3024u + type * 2u) == 0xFFFFu)
                {
                    g_stOverNoTail.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (!dmaRoom(rdram, 0x400u + 0x10u * need))
                {
                    g_stOverNoRoom.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            const uint32_t e = base ? level + 0x1D64u + static_cast<uint32_t>(count) * 0x10u : dmaAlloc(rdram, 0x10u) & kRamMask;
            wr16(e + 6, rd16(entry + 0xE));
            wr32(e + 0xC, rd32(rdram, kLevelFrameValue));
            const uint32_t m = rd32(rdram, kLevelScratch) - 0x400u;
            if (!base)
                g_overBytes += 0x400;
            wr32(kLevelScratch, m);
            wr32(e + 8, m);
            // FUN_00204508(m, entity matrix, entry scale +0, entry offset +0x10): FUN_002D7FB0 rotates the offset
            const float ox = rdf(rdram, entry + 0x10u), oy = rdf(rdram, entry + 0x14u), oz = rdf(rdram, entry + 0x18u);
            auto M = [&](int i) { return rdf(rdram, entity + static_cast<uint32_t>(i) * 4u); };
            float rx = M(0) * ox, ry = M(1) * ox, rz = M(2) * ox;
            rx = rx + M(4) * oy, ry = ry + M(5) * oy, rz = rz + M(6) * oy;
            rz = rz + M(10) * oz, ry = ry + M(9) * oz, rx = rx + M(8) * oz;
            const float sx = rdf(rdram, entry), sy = rdf(rdram, entry + 4u), sz = rdf(rdram, entry + 8u);
            const float out[16] = {M(0) * sx, M(1) * sx, M(2) * sx, 0.0f, M(4) * sy, M(5) * sy, M(6) * sy, 0.0f,
                                   M(8) * sz, M(9) * sz, M(10) * sz, 0.0f,
                                   M(12) + rx + rdf(rdram, kWorldOffset), M(13) + ry + rdf(rdram, kWorldOffset + 4u),
                                   M(14) + rz + rdf(rdram, kWorldOffset + 8u), 1.0f};
            std::memcpy(rdram + (m & kRamMask), out, sizeof(out));
            const float dist = std::sqrt(d2);
            if (!base)
            {
                // the second list: the entry (its +4 chain link unused: -1), the alpha part as a type-6 extra
                wr16(e + 4, 0xFFFFu);
                pushOver(rdram, type, e);
                ++g_overTotal;
                g_stOverAdded.fetch_add(1, std::memory_order_relaxed);
                if (type == 6)
                {
                    wrf(rdram, e, dist);
                }
                else if (second)
                {
                    const uint32_t e2 = dmaAlloc(rdram, 0x10u) & kRamMask;
                    wr16(e2 + 6, second);
                    wr32(e2 + 0xC, rd32(rdram, e + 0xC));
                    wr32(e2 + 8, rd32(rdram, e + 8));
                    wr16(e2 + 4, 0xFFFFu);
                    wrf(rdram, e, dist);
                    pushOver(rdram, 6, e2);
                    ++g_overTotal;
                }
                if (g_overTotal > g_stOverMax.load(std::memory_order_relaxed))
                    g_stOverMax.store(g_overTotal, std::memory_order_relaxed);
                return;
            }
            const uint32_t head = level + 0x3024u + type * 2u;
            wr16(e + 4, rd16(head));
            wr16(head, static_cast<uint16_t>(count));
            g_baseType[count] = static_cast<int8_t>(type);
            int n = count + 1;
            wr32(level + 0x1D60u, static_cast<uint32_t>(n));
            if (type == 6)
            {
                wrf(rdram, e, dist);
                return;
            }
            if (second == 0)
                return;
            if (n >= kLevelMax)
            {
                // no room for its alpha part in the first list: in the second (the game drops it)
                if (overOk && dmaRoom(rdram, 0x10u))
                {
                    const uint32_t e2 = dmaAlloc(rdram, 0x10u) & kRamMask;
                    wr16(e2 + 6, second);
                    wr32(e2 + 0xC, rd32(rdram, e + 0xC));
                    wr32(e2 + 8, rd32(rdram, e + 8));
                    wr16(e2 + 4, 0xFFFFu);
                    wrf(rdram, e, dist);
                    pushOver(rdram, 6, e2);
                    ++g_overTotal;
                }
                return;
            }
            g_baseType[n] = 6;
            const uint32_t e2 = level + 0x1D64u + static_cast<uint32_t>(n) * 0x10u;
            wr16(e2 + 6, second);
            wr32(e2 + 0xC, rd32(rdram, e + 0xC));
            wr32(e2 + 8, rd32(rdram, e + 8));
            wr16(e2 + 4, rd16(level + 0x3030u));
            wrf(rdram, e, dist);
            wr16(level + 0x3030u, static_cast<uint16_t>(n));
            wr32(level + 0x1D60u, static_cast<uint32_t>(n + 1));
        }

        // LCS_PATH_STATS=1 (diagnostic, droidperf2): FUN_00147950 - the path search the AI calls (11 arguments, a float
        // distance limit in f12: VC's CPathFind::DoPathSearch shape; called from 0x149358 / 0x1493E4, 0x1E67CC, 0x219300 /
        // 0x219BC0 / 0x21DFB0 near the car spawner) - its calls and host time by caller, every 5 s. It was the
        // game-thread half of the 300-450 ms frames after teleports at 300% traffic / peds (VCS_PROFILE_SPIKES)
        constexpr uint32_t kPathSearch = 0x00147950u;
        PS2Runtime::RecompiledFunction g_origPathSearch = nullptr;
        PS2_GUEST_CC void pathSearch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            struct Caller
            {
                uint64_t calls = 0;
                double ms = 0.0;
            };
            static std::unordered_map<uint32_t, Caller> callers;
            static auto reportAt = ps2x::hostNow();
            static double frameMax = 0.0, frameMs = 0.0;
            static uint64_t lastFrame = 0;
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_frame != lastFrame)
            {
                frameMax = std::max(frameMax, frameMs);
                frameMs = 0.0;
                lastFrame = g_frame;
            }
            const auto t0 = ps2x::hostNow();
            g_origPathSearch(rdram, ctx, runtime);
            const double ms = std::chrono::duration<double, std::milli>(ps2x::hostNow() - t0).count();
            Caller &c = callers[ra];
            ++c.calls;
            c.ms += ms;
            frameMs += ms;
            if (ps2x::hostNow() - reportAt > std::chrono::seconds(5))
            {
                reportAt = ps2x::hostNow();
                std::string s;
                for (const auto &[at, k] : callers)
                {
                    char b[96];
                    std::snprintf(b, sizeof(b), " 0x%x: %llu calls %.1f ms;", at, (unsigned long long)k.calls, k.ms);
                    s += b;
                }
                std::fprintf(stderr, "[lcs-path] last 5 s:%s most in a frame %.1f ms\n", s.c_str(), frameMax);
                callers.clear();
                frameMax = 0.0;
            }
        }

        // MakeSpaceFor's last resort (counted only; the game goes on as it would)
        PS2_GUEST_CC void deleteRwObjects(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_pressure.fetch_add(1, std::memory_order_relaxed);
            g_origDeleteRw(rdram, ctx, runtime);
        }

        settings::Spec choice(const char *key, const char *label, const char *help, const char *env,
                              std::initializer_list<settings::Choice> choices, int def, std::atomic<int> &target)
        {
            settings::Spec s;
            s.key = key;
            s.page = "Enhancements";
            s.section = "World";
            s.label = label;
            s.help = help;
            s.type = settings::Type::Choice;
            s.choices = choices;
            s.defaultValue = def;
            s.env = env;
            s.onChange = [&target](int v) { target.store(v, std::memory_order_relaxed); };
            return s;
        }
    }

    void registerSettings()
    {
        settings::setRangeForRun("enhance.draw_distance", 100, kMaxPercent);
        settings::noteForRun("enhance.draw_distance",
                             "Liberty City Stories: up to 1000%; objects are drawn up to 300 m x the setting, as memory "
                             "allows (the game's own 300 m at 100%).");
        settings::add(choice("enhance.far_fog", "Fog and far plane",
                             "Original: the PS2's haze and view distance. With draw distance: the far plane and the fog "
                             "move out with the Draw distance setting (no change at 100%). + haze: the far plane moves "
                             "out, the haze starts where the PS2's does and fades the far city into the fog colour "
                             "(complete at about 1.5 times the PS2's view distance).",
                             "LCS_FAR_FOG", {{"Original", 0}, {"With draw distance", 1}, {"With draw distance + haze", 2}}, 2,
                             g_set.fog));
        settings::add(choice("enhance.stream_fast", "Model streaming",
                             "PS2 pace: one model loaded a frame, as the PS2 does (objects can appear late when driving "
                             "fast). Fast: as many as are waiting, a few milliseconds a frame at most.",
                             "LCS_STREAM_FAST", {{"PS2 pace", 0}, {"Fast", 1}}, 1, g_set.streaming));
    }

    float lodScale() { return g_effective; }

    // [tone] onFrame reads the setting each frame, so the both-cities horizon floor (dualworld.cpp applyDrawDistance) went
    // through VCS's draw distance spec only and never reached Liberty City behind: it is applied here instead
    namespace
    {
        std::atomic<int> g_floor{0};
    }
    void setFloor(int percent) { g_floor.store(std::clamp(percent, 0, kMaxPercent), std::memory_order_relaxed); }

    void onFrame(uint8_t *rdram)
    {
        if (!g_installed)
            return;
        ++g_frame;
        g_set.draw.store(std::max(settings::value("enhance.draw_distance"), g_floor.load(std::memory_order_relaxed)),
                         std::memory_order_relaxed);
        const float want = target();
        {
            float cam[3];
            for (int i = 0; i < 3; ++i)
                cam[i] = rdf(rdram, kTheCamera + 0x30u + 4u * i);
            const float dx = cam[0] - g_lastCam[0], dy = cam[1] - g_lastCam[1], dz = cam[2] - g_lastCam[2];
            if (dx * dx + dy * dy + dz * dz > 50.0f * 50.0f)
            {
                g_overBlock = 120;
                g_jumped = true;
            }
            else if (g_overBlock > 0)
                --g_overBlock;
            std::memcpy(g_lastCam, cam, sizeof(cam));
        }

        // the governor: free model memory and the render lists decide how much of the scale is used this frame
        const int heapFree = static_cast<int>(rd32(rdram, kHeap + 0x10u) - rd32(rdram, kHeap + 0xCu) - rd32(rdram, kHeap + 0x14u));
        const uint32_t visible = rd32(rdram, kVisibleCount), alpha = rd32(rdram, kAlphaCount); // last frame's
        const uint32_t pressure = g_pressure.exchange(0, std::memory_order_relaxed);
        g_stPressure.fetch_add(pressure, std::memory_order_relaxed);
        if (want <= 1.0f)
            g_effective = 1.0f;
        else if (!g_guardOn)
            g_effective = want;
        else
        {
            // back off when the game had to delete drawn objects to make room (or memory is all but gone, or the render
            // lists fill up); creep back up after 4 s without that
            // ... or the frame's DMA buffer was all but full when sent (less than 96 KB left: at 1000% the first frames
            // after a teleport left 5 KB without the second list and went 133 KB past its end with it - nothing in the
            // game checks that; the scan's scale is what fills it)
            static uint64_t calmSince = 0;
            const int dmaLeft = g_dmaLastSend;
            g_dmaLastSend = 1 << 30;
            // ... or the frames are slow (droidperf2): the game thread's busy time (game + render + IOP, the pacing wait
            // left out: ps2perf::lastFrame) or the whole frame (worker / GS stage / GPU bound) averaged over ~8 frames
            // above LCS_DRAW_SLOW_MS (40 ms, below 25 fps; the frame at 1.25x that). A phone at 1000% with 300% traffic
            // and peds sat at 45 ms of game thread, 18 fps, while nothing above said "tight"
            static float busyAvg = 0.0f, frameAvg = 0.0f;
            {
                float frameMs = 0.0f, busyMs = 0.0f;
                ps2perf::lastFrame(frameMs, busyMs);
                if (frameMs > 0.0f && frameMs < 2000.0f)
                {
                    busyAvg += (busyMs - busyAvg) * 0.125f;
                    frameAvg += (frameMs - frameAvg) * 0.125f;
                }
            }
            const bool slow = g_slowMs > 0.0f && g_frame > 300 && (busyAvg > g_slowMs || frameAvg > g_slowMs * 1.25f);
            if (slow)
                g_stSlow.fetch_add(1, std::memory_order_relaxed);
            g_stBusyAvg.store(busyAvg, std::memory_order_relaxed);
            const bool tight = pressure > 0 || std::max(visible, alpha) > kListCapacity * 85u / 100u || dmaLeft < 96 * 1024 || slow;
            if (g_jumped)
            {
                // a camera jump: the new place's first frames at half the scale (at most 4x), back up from 2 s on
                g_effective = std::max(1.0f, std::min(g_effective, std::min(4.0f, 1.0f + (want - 1.0f) * 0.5f)));
                calmSince = g_frame;
                g_jumped = false;
            }
            else if (tight)
            {
                g_effective = std::max(1.0f, g_effective - std::max(0.05f, (g_effective - 1.0f) * 0.1f));
                calmSince = g_frame;
                g_stBackoffs.fetch_add(1, std::memory_order_relaxed);
            }
            else if (g_frame - calmSince > 120)
                g_effective = std::min(want, g_effective + 0.02f);
            g_effective = std::min(g_effective, want);
        }

        g_stEffective.store(g_effective, std::memory_order_relaxed);
        g_stHeapFree.store(heapFree, std::memory_order_relaxed);
        if (heapFree < g_stHeapMin.load(std::memory_order_relaxed))
            g_stHeapMin.store(heapFree, std::memory_order_relaxed);
        g_stVisible.store(static_cast<int>(visible), std::memory_order_relaxed);
        g_stAlpha.store(static_cast<int>(alpha), std::memory_order_relaxed);
        if (static_cast<int>(std::max(visible, alpha)) > g_stVisibleMax.load(std::memory_order_relaxed))
            g_stVisibleMax.store(static_cast<int>(std::max(visible, alpha)), std::memory_order_relaxed);
        g_stRequested.store(static_cast<int32_t>(rd32(rdram, kNumRequested)), std::memory_order_relaxed);
        g_stFrames.fetch_add(1, std::memory_order_relaxed);

        // objects waiting longer than 10 s, or not seen for a while (out of view): dropped from the counters
        if ((g_frame & 63u) == 0)
        {
            for (auto it = g_waiting.begin(); it != g_waiting.end();)
                it = g_frame - it->second.since > 600 ? g_waiting.erase(it) : std::next(it);
        }
        g_stWaitingNow.store(static_cast<int>(g_waiting.size()), std::memory_order_relaxed);
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_DRAWDIST"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs-draw] off (LCS_DRAWDIST=0): the PS2's far plane, scan and streaming\n");
            return;
        }
        if (const char *e = std::getenv("LCS_DRAW_GUARD"); e && e[0] == '0')
            g_guardOn = false;
        g_set.fog.store(settings::value("enhance.far_fog"));
        g_set.streaming.store(settings::value("enhance.stream_fast"));
        struct Hook
        {
            uint32_t at;
            PS2Runtime::RecompiledFunction *orig;
            PS2Runtime::RecompiledFunction fn;
        };
        const Hook hooks[] = {
            {kSetFar, &g_origSetFar, &setFar},          {kSetFog, &g_origSetFog, &setFog},
            {kProjection, &g_origProjection, &projection}, {kTransform, &g_origTransform, &transform},
            {kVisibility, &g_origVisibility, &visibility}, {kLoadStep, &g_origLoadStep, &loadStep},
            {kDeleteRwObjects, &g_origDeleteRw, &deleteRwObjects}, {kLevelAdd, &g_origLevelAdd, &levelAdd},
            {kFogParams, &g_origFogParams, &fogParams},
            {kLevelDraw, &g_origLevelDraw, &levelDraw},   {kLevelReset, &g_origLevelReset, &levelReset},
            {kAlphaSort, &g_origAlphaSort, &alphaSort},   {kDmaKick, &g_origDmaKick, &dmaKick},
        };
        if (const char *e = std::getenv("LCS_LEVEL_LIST"); e && e[0] == '0')
            g_overOn = false;
        if (const char *e = std::getenv("LCS_LEVEL_MAX"); e && *e)
            g_levelTotalMax = std::clamp(std::atoi(e), kLevelMax, kLevelTotalMax);
        std::fill(std::begin(g_baseType), std::end(g_baseType), static_cast<int8_t>(-1));
        if (const char *s = std::getenv("LCS_DRAW_SLOW_MS"); s && *s)
            g_slowMs = std::max(0.0f, static_cast<float>(std::atof(s)));
        if (const char *k = std::getenv("LCS_HAZE_K"); k && *k)
            g_hazeK = std::clamp(static_cast<float>(std::atof(k)), 0.0f, 1.0f);
        for (const Hook &h : hooks)
            if (!(*h.orig = runtime.lookupFunction(h.at)))
            {
                std::fprintf(stderr, "[lcs-draw] 0x%x not found: long draw distance off\n", h.at);
                return;
            }
        for (const Hook &h : hooks)
            runtime.replaceFunction(h.at, h.fn);
        if (const char *e = std::getenv("LCS_PATH_STATS"); e && e[0] == '1')
            if ((g_origPathSearch = runtime.lookupFunction(kPathSearch)))
                runtime.replaceFunction(kPathSearch, &pathSearch);
        g_installed = true;
        std::fprintf(stderr, "[lcs-draw] far plane / fog %s, streaming %s, guard %s\n",
                     g_set.fog.load() == 2 ? "with draw distance + haze" : g_set.fog.load() ? "with draw distance" : "original",
                     g_set.streaming.load() ? "fast" : "PS2 pace",
                     g_guardOn ? "on" : "off");

        // lcsdraw [reset] -> ok draw eff far fog tcfar heapfree heapmin vis alpha vismax requested steps frames
        //   guarded backoffs pops popframes popgap waitframes waiting
        ps2control::addCommand("lcsdraw", [](const std::string &args, ps2control::Reply reply) {
            char text[768];
            const uint64_t pops = g_stPops.load();
            std::snprintf(text, sizeof(text),
                          "ok draw=%d eff=%.3f far=%.0f fog=%.0f tcfar=%.0f heapfree=%d heapmin=%d vis=%d alpha=%d "
                          "vismax=%d requested=%d steps=%llu frames=%llu guarded=%llu backoffs=%llu pops=%llu "
                          "popwait=%.1f popgap=%.1f waitframes=%llu waiting=%d fogset=%d stream=%d pressure=%llu "
                          "levelmax=%d levelfull=%llu levelfar=%llu levelcut=%llu haze=%.0f overmax=%d overadded=%llu "
                          "overdrawn=%llu overalpha=%llu notail=%llu noroom=%llu dmamin=%d dmaafter=%d alphamax=%d stale=%llu "
                          "slow=%llu busyavg=%.1f stepmax=%.2f loopmax=%.2f",
                          g_set.draw.load(), g_stEffective.load(), g_stFar.load(), g_stFog.load(), g_stTcFar.load(),
                          g_stHeapFree.load(), g_stHeapMin.load(), g_stVisible.load(), g_stAlpha.load(),
                          g_stVisibleMax.load(), g_stRequested.load(), (unsigned long long)g_stSteps.load(),
                          (unsigned long long)g_stFrames.load(), (unsigned long long)g_stGuarded.load(),
                          (unsigned long long)g_stBackoffs.load(), (unsigned long long)pops,
                          pops ? double(g_stPopFrames.load()) / pops : 0.0, pops ? g_stPopGapSum.load() / pops : 0.0,
                          (unsigned long long)g_stWaitFrames.load(), g_stWaitingNow.load(), g_set.fog.load(),
                          g_set.streaming.load(), (unsigned long long)g_stPressure.load(), g_stLevelMax.load(),
                          (unsigned long long)g_stLevelFull.load(), (unsigned long long)g_stLevelFar.load(),
                          (unsigned long long)g_stLevelCut.load(), g_stHaze.load(), g_stOverMax.load(),
                          (unsigned long long)g_stOverAdded.load(), (unsigned long long)g_stOverDrawn.load(),
                          (unsigned long long)g_stOverAlpha.load(), (unsigned long long)g_stOverNoTail.load(),
                          (unsigned long long)g_stOverNoRoom.load(), g_stDmaMin.load(), g_stDmaAfter.load(),
                          g_stAlphaMax.load(), (unsigned long long)g_stOverStale.load(),
                          (unsigned long long)g_stSlow.load(), g_stBusyAvg.load(), g_stStepMaxUs.load() / 1000.0,
                          g_stLoopMaxUs.load() / 1000.0);
            if (args.find("hist") != std::string::npos)
            {
                // the last 64 sends: frame freeAtLastAdd freeAtSend extraBytes extraEntries write top
                std::string h = "ok";
                for (uint32_t i = 0; i < 64 && i < g_histN; ++i)
                {
                    const Hist &x = g_hist[(g_histN - 64 + i) % 64];
                    char b[128];
                    std::snprintf(b, sizeof(b), " %llu:%d/%d/%d/%d/%x/%x%s", (unsigned long long)x.frame, x.freeAdd, x.freeKick,
                                  x.overBytes, x.overTotal, x.write, x.top, x.added ? "" : "n");
                    h += b;
                }
                return reply(h);
            }
            if (args.find("reset") != std::string::npos)
            {
                g_stHeapMin.store(1 << 30);
                g_stVisibleMax.store(0);
                for (auto *a : {&g_stSteps, &g_stFrames, &g_stGuarded, &g_stBackoffs, &g_stPops, &g_stPopFrames, &g_stWaitFrames,
                                &g_stPressure, &g_stLevelFull, &g_stLevelFar, &g_stLevelCut})
                    a->store(0);
                g_stLevelMax.store(0);
                g_stPopGapSum.store(0.0f);
            }
            reply(text);
        });
    }
}
