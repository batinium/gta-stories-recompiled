// [lcs] See lcs_particle_timing.h. LCS's particle system is VCS's one engine generation earlier, and counts frames in
// the same two places (game/vcs_particle_timing.cpp has the whole story):
// 1. CParticle::Update 0x1CE1B0 (best.py twin of VCS's 0x1E9C90, 0.976) changes every particle once a frame by its own
//    rates: brightness -= +0x38 (+0x2C), alpha -= +0x3A (+0x2D; freed at 0), size += +0x34 x (speed x step + 1)
//    (+0x30; speed doubled for types 0x21 / 0x41; splashes 0x2C / 0x2D past size 1.2 shrink and lose 0.15 of vertical
//    speed instead), rotation += +0x3E (+0x40), the animation counter +0x3C += 1 against the type's period. The
//    particle (0x50 bytes, the next one at +0x48) is VCS's field for field; the pool is 750 at 0x4182B0 (Initialise
//    0x1CC6B8 links it; free list 0x3D9DB0). The systems differ: a pointer at 0x3D9AF8 to 82 (`slti 0x52`) of 0x94
//    bytes each, the period at +0x42 and the list head at +0x90 (VCS 0x84 / +0x32 / +0x80).
// 2. The continuous emitters call AddParticle 0x1CD5B8 (the overload without a colour 0x1CD580, AddJetExplosion
//    0x1CFF58) once a frame. The throttled return addresses below come from the 333 calls (scratchpad sites.py:
//    every VCS site in vcs_particle_timing's list mapped through its function's best.py twin and an instruction
//    alignment; LCS-only emitters by reading their callers: per-frame vtable methods / `frame & 1` gates throttled,
//    ms timers and events left), each checked not to read $v0 after the call (the particle object 0x1F9B28 does, as
//    VCS's 0x229BBC: left out).
// Only while frames are shorter than the PS2's and the fix is on (as VCS's: independent timing or a cap other than 30;
// VCS_PARTICLE_TIMING=0 / Reduce Post Processing Effects off). VCS_PARTICLE_LOG=1 prints the calls per second by return
// address and the particles' lifetimes by type every 10 s of game time.
#include "game/lcs_particle_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace lcs::particle_timing
{
    namespace
    {
        constexpr uint32_t kAddParticle = 0x001CD5B8u;  // CParticle::AddParticle(type a0, pos a1, dir a2, entity a3, ...)
        constexpr uint32_t kAddParticle2 = 0x001CD580u; // the overload without a colour (calls kAddParticle)
        constexpr uint32_t kAddJetExplosion = 0x001CFF58u;
        constexpr uint32_t kUpdate = 0x001CE1B0u;       // CParticle::Update
        constexpr uint32_t kSystems = 0x003D9AF8u;      // pointer to the particle systems
        constexpr uint32_t kSystemCount = 82u, kSystemSize = 0x94u, kSysPeriod = 0x42u, kSysList = 0x90u;
        constexpr uint32_t kPool = 0x004182B0u, kPoolSize = 750u, kParticleSize = 0x50u;
        constexpr uint32_t kTimeStep = 0x003D9B8Cu;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kTimeMs = 0x003D9B70u;       // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kFrameCounter = 0x003D9B6Cu; // CTimer::m_FrameCounter
        constexpr uint32_t kPausedA = 0x003D9CB4u, kPausedB = 0x003D9C68u, kPausedC = 0x003D8BA4u; // Update's returns
        constexpr float kPs2Step = 50.0f / 30.0f;

        // The continuous emitters' calls: return addresses (jal + 8), by function (type numbers are LCS's)
        constexpr uint32_t kThrottled[] = {
            // 0x127458 car in water (VCS 0x138B68): splashes, smoke
            0x00127E3Cu, 0x001282F4u, 0x00128374u,
            // 0x128408 CAutomobile::ProcessControl (VCS 0x13A0A0): engine steam / smoke, car fire
            0x0012A598u, 0x0012A630u, 0x0012A6C8u, 0x0012A760u, 0x0012A824u, 0x0012A8E4u, 0x0012A9A4u, 0x0012AA64u,
            0x0012D1A0u, 0x0012D24Cu, 0x0012D284u, 0x0012DFDCu,
            // 0x12EB50 damaged engine: sparks, smoke, steam (VCS 0x140B28; LCS has 9 of its 15 calls)
            0x0012F018u, 0x0012F1A0u, 0x0012F2C4u, 0x0012F328u, 0x0012F3BCu, 0x0012F580u, 0x0012F624u, 0x0012F670u,
            0x0012F704u,
            // 0x12F738 wheel dirt / sand / spray (VCS 0x1419D0)
            0x0012F89Cu, 0x0012F994u, 0x0012FA7Cu, 0x0012FBE8u, 0x0012FD14u,
            // 0x12FD58 CAutomobile::PreRender (VCS 0x141FE8): tyre smoke, rim sparks, rain on the body, exhaust, splashes
            0x0012FF8Cu, 0x00130008u, 0x001305DCu, 0x00130650u, 0x001306C4u, 0x001307ACu, 0x00130888u, 0x00130B60u,
            0x00130BD0u, 0x00130C40u, 0x00130C90u, 0x00130E20u, 0x00130F1Cu, 0x0013102Cu, 0x00131170u, 0x0013126Cu,
            0x0013137Cu, 0x00131714u, 0x001319DCu, 0x00131A14u, 0x00131A78u, 0x00131AB8u, 0x00135060u, 0x00135184u,
            0x001354A4u, 0x001355C8u, 0x001364ECu, 0x00136610u, 0x00136958u, 0x00136A7Cu,
            // 0x152898 teargas (VCS 0x181208)
            0x00152B44u, 0x00152B84u, 0x00152BB8u,
            // 0x157990 bike ProcessControl: fire / smoke (VCS 0x337208)
            0x0015C2E8u, 0x0015C328u, 0x0015C3B8u,
            // 0x15D2C0 bike engine smoke / steam (VCS 0x33D5A0)
            0x0015D5ACu, 0x0015D660u, 0x0015D720u, 0x0015D830u, 0x0015D914u,
            // 0x15D938 bike wheel dirt / spray (VCS 0x33DCA0)
            0x0015DA88u, 0x0015DB70u, 0x0015DC4Cu, 0x0015DD68u, 0x0015DE84u,
            // 0x15DEB8 bike PreRender: tyre smoke, sparks, exhaust (VCS 0x33F010 / 0x399FA0)
            0x0015E0DCu, 0x0015E1A0u, 0x0015E21Cu, 0x0015F7B8u, 0x0015F824u, 0x0015F890u, 0x0015F8DCu, 0x001607CCu,
            0x001607F8u,
            // boat ProcessControl 0x17ECA0 (vtable 0x3ACEE4): fire / smoke (VCS 0x1A5648), propeller, bow splashes
            0x0017F878u, 0x0017F91Cu, 0x0017FA00u, 0x0017FA8Cu, 0x00180AB8u, 0x00180B34u, 0x00181FB0u, 0x00182018u,
            0x001827CCu, 0x00182834u,
            // 0x220B28 a vehicle method (4 vtables): the wake's BOAT_SPLASH (LCS only)
            0x00221048u, 0x00221204u, 0x00221324u, 0x00221450u, 0x0022172Cu, 0x002218E8u, 0x00221A14u,
            // 0x1DC188 (from CPed::ProcessControl 0x1D6F30): a ped in water, splashes on `frame & 1` (LCS only)
            0x001DD128u, 0x001DD154u, 0x001DD214u, 0x001DD29Cu,
            // 0x1E18C0 ped PreRender: blood from a limb, rain on peds (VCS 0x200660)
            0x001E2140u, 0x001E2408u,
            // 0x1F9450 particle objects: fountains, hydrants, steam, smoke (VCS 0x2294A8; 0x1F9B28 reads the particle back)
            0x001F9694u, 0x001F974Cu, 0x001F981Cu, 0x001F98D4u, 0x001F9A8Cu, 0x001F9BE0u, 0x001F9D24u, 0x001F9F10u,
            0x001F9F48u, 0x001FA020u, 0x001FA058u, 0x001FA130u, 0x001FA168u, 0x001FA224u, 0x001FA25Cu, 0x001FA408u,
            0x001FA4FCu, 0x001FA5F0u, 0x001FA6E4u, 0x001FA8FCu, 0x001FA928u, 0x001FAA04u, 0x001FAA30u, 0x001FAB0Cu,
            0x001FAB38u, 0x001FAC14u, 0x001FAC40u, 0x001FADB8u, 0x001FAE80u, 0x001FAF48u, 0x001FB010u, 0x001FB104u,
            0x001FB14Cu, 0x001FB360u, 0x001FB408u, 0x001FB4B0u, 0x001FB550u, 0x001FB590u, 0x001FB744u, 0x001FB804u,
            0x001FB8C4u, 0x001FB96Cu, 0x001FBA64u, 0x001FBB98u, 0x001FBBE8u,
            // 0x2CDB18 the water cannon (from 0x2CD310, once a frame): steam / splash where it hits (LCS only)
            0x002CDEA8u, 0x002CDF74u,
            // 0x307AC0 CFireManager: water on a fire (VCS 0x2BEAB8)
            0x00307C58u, 0x00307C94u,
            // 0x325758 / 0x325A08 ground steam after rain, 0x325CA8 rain on screen (VCS 0x3E4270 / 0x3E4528 / 0x3E47B8)
            0x003259B0u, 0x00325C54u, 0x00325F18u, 0x00325FDCu, 0x00326074u,
            // 0x326EB0 insects (VCS 0x3E5B20)
            0x00327000u,
            // 0x32D5E8 rotor blade sparks (VCS 0x3EE308), 0x32E6D8 rotor downwash (VCS 0x3EF3B8)
            0x0032DB58u, 0x0032DBA8u, 0x0032DC30u, 0x0032DE78u, 0x0032DEC4u, 0x0032EB58u, 0x0032EC2Cu, 0x0032ECFCu,
            // 0x1CE1B0 CParticle::Update itself: the fountain at (557, -4, 151)
            0x001CE380u,
            // 0x163788 CExplosion::Update (VCS 0x2E23A0): a burning car's smoke on odd steps and its jets (1 in 16 a frame)
            0x001639D0u, 0x00163A60u, 0x00163AFCu,
        };

        PS2Runtime::RecompiledFunction g_origAdd = nullptr, g_origAdd2 = nullptr, g_origUpdate = nullptr, g_origJet = nullptr;

        std::atomic<bool> g_reduced{false};

        bool envOn()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_PARTICLE_TIMING");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        bool enabled() { return envOn() && !g_reduced.load(std::memory_order_relaxed); }
        bool active() { return enabled() && (vcs::timing() == vcs::Timing::Independent || vcs::frameCap() != 30); }
        bool logOn()
        {
            static const bool on = std::getenv("VCS_PARTICLE_LOG") != nullptr;
            return on;
        }

        template <typename T>
        T rd(const uint8_t *rdram, uint32_t addr)
        {
            T v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), sizeof(T));
            return v;
        }
        template <typename T>
        void wr(uint8_t *rdram, uint32_t addr, T v)
        {
            std::memcpy(rdram + (addr & 0x1FFFFFFu), &v, sizeof(T));
        }

        // the frame's share of a PS2 frame; 1 at about 30 fps and below (the game's own amounts)
        float frameScale(const uint8_t *rdram)
        {
            if (!active())
                return 1.0f;
            const float step = rd<float>(rdram, kTimeStep);
            return step > 0.0f && step < kPs2Step * 0.9f ? step / kPs2Step : 1.0f;
        }

        // ---- 2. the emitters ----------------------------------------------------------------------------------------
        bool throttled(uint32_t ra)
        {
            static const std::vector<uint32_t> sorted = [] {
                std::vector<uint32_t> v(std::begin(kThrottled), std::end(kThrottled));
                std::sort(v.begin(), v.end());
                return v;
            }();
            return std::binary_search(sorted.begin(), sorted.end(), ra);
        }

        struct Emitter
        {
            float sum = 1.0f; // the first call passes
            uint32_t frame = 0;
        };
        std::unordered_map<uint64_t, Emitter> g_emitters;
        std::unordered_map<uint64_t, uint32_t> g_callsThisFrame; // (site, entity) -> calls so far this frame
        uint32_t g_callsFrame = ~0u;

        std::unordered_map<uint32_t, uint64_t> g_logCalls, g_logPassed;
        std::unordered_map<uint32_t, int> g_logType;
        uint64_t g_logFrames = 0;

        // whether this call spawns: the PS2's count per second at any frame rate
        bool emitterPasses(uint8_t *rdram, uint32_t ra, uint32_t entity)
        {
            const float scale = frameScale(rdram);
            if (scale >= 1.0f || !throttled(ra))
                return true;
            const uint32_t frame = rd<uint32_t>(rdram, kFrameCounter);
            if (frame != g_callsFrame)
            {
                g_callsFrame = frame;
                g_callsThisFrame.clear();
                if ((frame & 0x3FFu) == 0u) // forget emitters not seen for a while
                    for (auto it = g_emitters.begin(); it != g_emitters.end();)
                        it = frame - it->second.frame > 600u ? g_emitters.erase(it) : std::next(it);
            }
            const uint64_t site = static_cast<uint64_t>(ra) << 32 | entity;
            const uint32_t n = g_callsThisFrame[site]++;
            Emitter &e = g_emitters[site ^ (static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull)];
            if (frame - e.frame > 30u) // one that stopped and starts again starts like on the PS2: its first call passes
                e.sum = 1.0f;
            e.frame = frame;
            e.sum += scale;
            if (e.sum < 1.0f + 1e-4f)
            {
                e.sum = std::max(e.sum, 0.0f);
                return false;
            }
            e.sum = std::min(e.sum - 1.0f, 1.0f);
            return true;
        }

        template <PS2Runtime::RecompiledFunction *Original>
        PS2_GUEST_CC void addParticle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const bool passes = !enabled() || emitterPasses(rdram, ra, GPR_U32(ctx, 7));
            if (logOn())
            {
                ++g_logCalls[ra];
                g_logPassed[ra] += passes ? 1u : 0u;
                g_logType[ra] = Original == &g_origJet ? -1 : static_cast<int>(GPR_U32(ctx, 4));
            }
            if (!passes)
            {
                SET_GPR_U32(ctx, 2, 0u); // as AddParticle with the pool full (these callers ignore it)
                ctx->pc = ra;
                return;
            }
            (*Original)(rdram, ctx, runtime);
        }

        // ---- 1. the update -------------------------------------------------------------------------------------------
        struct Slot
        {
            bool valid = false;
            uint32_t destroy = 0;                    // +0x20: the particle's destroy time (ms)
            int16_t orig[3] = {}, wrote[3] = {};     // fade to black +0x38, alpha fade +0x3A, rotation +0x3E
            float acc[3] = {};                       // what the dithering carries
            float origRate = 0.0f, wroteRate = 0.0f; // size rate +0x34
            uint32_t spawnMs = 0, seen = 0;          // log: first seen (game clock); the last walk it was in
            int type = 0;
        };
        uint32_t g_walk = 0;
        std::unordered_map<int, std::pair<uint64_t, uint64_t>> g_lifetimes; // log: type -> (count, ms)
        Slot g_slots[kPoolSize];
        constexpr uint32_t kRateOffsets[3] = {0x38u, 0x3Au, 0x3Eu};

        struct Period
        {
            bool valid = false;
            uint16_t orig = 0, wrote = 0;
        };
        Period g_periods[kSystemCount];

        void particleDied(const Slot &s, uint32_t nowMs)
        {
            auto &l = g_lifetimes[s.type];
            ++l.first;
            l.second += nowMs - s.spawnMs;
        }

        int16_t dither(int16_t orig, float scale, float &acc)
        {
            if (scale >= 1.0f)
            {
                acc = 0.0f;
                return orig;
            }
            const float want = static_cast<float>(orig) * scale + acc;
            const float whole = std::trunc(want);
            acc = want - whole;
            return static_cast<int16_t>(whole);
        }

        void scaleParticle(uint8_t *rdram, uint32_t p, int type, float scale, float step, bool write, uint32_t nowMs)
        {
            const uint32_t i = (p - kPool) / kParticleSize;
            if (p < kPool || i >= kPoolSize || (p - kPool) % kParticleSize)
                return;
            Slot &s = g_slots[i];
            const uint32_t destroy = rd<uint32_t>(rdram, p + 0x20);
            int16_t cur[3];
            for (int k = 0; k < 3; ++k)
                cur[k] = rd<int16_t>(rdram, p + kRateOffsets[k]);
            const float rate = rd<float>(rdram, p + 0x34);
            bool fresh = !s.valid || s.destroy != destroy || rate != s.wroteRate;
            for (int k = 0; k < 3 && !fresh; ++k)
                fresh = cur[k] != s.wrote[k];
            if (fresh)
            {
                if (s.valid)
                    particleDied(s, nowMs);
                s.spawnMs = nowMs;
                s.type = type;
                s.valid = true;
                s.destroy = destroy;
                for (int k = 0; k < 3; ++k)
                    s.orig[k] = cur[k], s.acc[k] = 0.0f;
                s.origRate = rate;
            }
            s.seen = g_walk;
            if (!write)
            {
                for (int k = 0; k < 3; ++k)
                    s.wrote[k] = cur[k];
                s.wroteRate = rate;
                return;
            }
            for (int k = 0; k < 3; ++k)
            {
                s.wrote[k] = dither(s.orig[k], scale, s.acc[k]);
                wr<int16_t>(rdram, p + kRateOffsets[k], s.wrote[k]);
            }
            // size: Update adds rate x (speed x step + 1) a frame; the PS2 added rate x (speed x 50/30 + 1) per 1/30 s
            float newRate = s.origRate;
            if (scale < 1.0f && s.origRate != 0.0f)
            {
                const float vx = rd<float>(rdram, p + 0x10), vy = rd<float>(rdram, p + 0x14), vz = rd<float>(rdram, p + 0x18);
                float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (type == 0x21 || type == 0x41)
                    speed *= 2.0f;
                newRate = s.origRate > 0.0f ? s.origRate * scale * (speed * kPs2Step + 1.0f) / (speed * step + 1.0f)
                                            : s.origRate * scale;
                // car / boat splashes past size 1.2 shrink and lose 0.15 of vertical speed a frame: the PS2's per 1/30 s
                if ((type == 0x2C || type == 0x2D) && s.origRate > 0.0f && rd<float>(rdram, p + 0x30) > 1.2f)
                    wr<float>(rdram, p + 0x18, vz + 0.15f * (1.0f - scale));
            }
            s.wroteRate = newRate;
            wr<float>(rdram, p + 0x34, newRate);
        }

        void scaleParticles(uint8_t *rdram)
        {
            static bool wasWriting = false;
            bool write = active();
            const bool restore = !write && wasWriting; // switched off: one walk at scale 1 puts the game's rates back
            wasWriting = write;
            write = write || restore;
            if ((!write && !logOn()) || rd<int32_t>(rdram, kPausedA) || rd<int32_t>(rdram, kPausedB) ||
                rd<uint8_t>(rdram, kPausedC))
                return;
            ++g_walk;
            const uint32_t nowMs = rd<uint32_t>(rdram, kTimeMs);
            const float scale = write && !restore ? frameScale(rdram) : 1.0f, step = rd<float>(rdram, kTimeStep);
            const uint32_t systems = rd<uint32_t>(rdram, kSystems);
            if (!systems)
                return;
            for (uint32_t t = 0; t < kSystemCount; ++t)
            {
                const uint32_t sys = systems + t * kSystemSize;
                // the animation period: the counter +0x3C counts up to it and resets the frame after, so a picture
                // lasts period + 2 frames - on the PS2 (period + 2) / 30 s
                Period &per = g_periods[t];
                const uint16_t cur = rd<uint16_t>(rdram, sys + kSysPeriod);
                if (!per.valid || cur != per.wrote)
                    per = {true, cur, cur};
                if (per.orig && write)
                {
                    const float frames = (per.orig + 2.0f) / scale - 2.0f;
                    per.wrote = scale >= 1.0f ? per.orig : static_cast<uint16_t>(std::clamp(std::lround(frames), 1l, 0xFFFFl));
                    wr<uint16_t>(rdram, sys + kSysPeriod, per.wrote);
                }
                const int type = rd<int32_t>(rdram, sys);
                uint32_t p = rd<uint32_t>(rdram, sys + kSysList);
                for (uint32_t n = 0; p && n < kPoolSize; ++n)
                {
                    scaleParticle(rdram, p, type, scale, step, write, nowMs);
                    p = rd<uint32_t>(rdram, p + 0x48);
                }
            }
            for (Slot &sl : g_slots) // not on any list now: freed by the last Update
                if (sl.valid && sl.seen != g_walk)
                {
                    if (logOn())
                        particleDied(sl, nowMs);
                    sl.valid = false;
                }
        }

        void logRates(uint8_t *rdram)
        {
            if (!logOn())
                return;
            static int64_t lastMs = -1;
            const int64_t ms = static_cast<int64_t>(rd<uint32_t>(rdram, kTimeMs));
            ++g_logFrames;
            if (lastMs < 0 || ms < lastMs)
                lastMs = ms;
            if (ms - lastMs < 10000)
                return;
            const double secs = (ms - lastMs) / 1000.0;
            std::vector<std::pair<uint32_t, uint64_t>> rows(g_logCalls.begin(), g_logCalls.end());
            std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second > b.second; });
            std::fprintf(stderr, "[lcs-particles] %.1f s, %.1f frames/s, AddParticle calls/s by return address [type] (passed):",
                         secs, g_logFrames / secs);
            for (const auto &[ra, calls] : rows)
                std::fprintf(stderr, " %x[%d]:%.1f(%.1f)%s", ra, g_logType[ra], calls / secs, g_logPassed[ra] / secs,
                             throttled(ra) ? "*" : "");
            std::fprintf(stderr, "\n");
            std::vector<std::pair<int, std::pair<uint64_t, uint64_t>>> lives(g_lifetimes.begin(), g_lifetimes.end());
            std::sort(lives.begin(), lives.end(), [](auto &a, auto &b) { return a.second.first > b.second.first; });
            std::fprintf(stderr, "[lcs-particles] lifetimes by type (died/s, mean ms):");
            for (const auto &[type, l] : lives)
                std::fprintf(stderr, " %d:%.1f/%.0f", type, l.first / secs, static_cast<double>(l.second) / l.first);
            std::fprintf(stderr, "\n");
            g_lifetimes.clear();
            g_logCalls.clear();
            g_logPassed.clear();
            g_logFrames = 0;
            lastMs = ms;
        }

        PS2_GUEST_CC void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Reduce Post Processing Effects (VCS's setting; its live switch only reaches VCS's modules)
            g_reduced.store(settings::value("enhance.reduce_postfx") != 0, std::memory_order_relaxed);
            scaleParticles(rdram);
            logRates(rdram);
            g_origUpdate(rdram, ctx, runtime);
        }
    }

    bool reduced() { return g_reduced.load(std::memory_order_relaxed); }

    void install(PS2Runtime &runtime)
    {
        g_origAdd = runtime.lookupFunction(kAddParticle);
        g_origAdd2 = runtime.lookupFunction(kAddParticle2);
        g_origUpdate = runtime.lookupFunction(kUpdate);
        g_origJet = runtime.lookupFunction(kAddJetExplosion);
        if (!g_origAdd || !g_origAdd2 || !g_origUpdate || !g_origJet)
        {
            std::fprintf(stderr, "[lcs-particle-timing] AddParticle / Update not found: particles stay on the frame rate\n");
            return;
        }
        runtime.replaceFunction(kAddParticle, &addParticle<&g_origAdd>);
        runtime.replaceFunction(kAddParticle2, &addParticle<&g_origAdd2>);
        runtime.replaceFunction(kAddJetExplosion, &addParticle<&g_origJet>);
        runtime.replaceFunction(kUpdate, &update);
        std::fprintf(stderr, "[lcs-particle-timing] %s: %zu emitter sites on the PS2's rate above 30 fps\n",
                     envOn() ? "on" : "off (VCS_PARTICLE_TIMING=0)", sizeof(kThrottled) / sizeof(kThrottled[0]));
    }
}
