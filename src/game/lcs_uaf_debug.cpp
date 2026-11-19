// [lcs] Debug aid for use-after-free hunts, off unless LCS_UAF_LOG=1 (nothing is hooked otherwise).
// Records the last 256k CMemoryHeap frees (pointer, size, callers, frame) and every CEntity::DeleteRwObject; checks each
// element render - FUN_002E87A0 (one element) and FUN_002D6918 (a fading element) - for a geometry whose material list
// is sane (element +0x14 geometry; geometry +0xC materials, +0x10 count). A broken one is logged with the frees that
// covered the geometry / the element / its clump and the entity being drawn (CEntity::Render 0x305AF8); LCS_UAF_SKIP=1
// then skips its draw. It found the mission smoke test's freed special character (docs/game-internals.md 11i).
#include "game/lcs_uaf_debug.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::uafdebug
{
    namespace
    {
        constexpr uint32_t kFree = 0x00150740u; // CMemoryHeap::Free(heap, p): big blocks (p < heap+0x10) have their size at p-8
        constexpr uint32_t kRenderElement = 0x002E87A0u; // starts with for (i < geometry +0x10) {} (0x2E87E0)
        constexpr uint32_t kRenderAlpha = 0x002D6918u;   // alpha into each material +7 (0x2D6898 via 0x1F6960), draw, 0xFF
        constexpr uint32_t kFrames = 0x003D9B6Cu;        // CTimer frame counter (lcs_missions.py)
        struct Rec
        {
            uint32_t p, size, ra, frame, ras[4];
        };
        constexpr uint32_t kRing = 1u << 18;
        Rec *g_ring = nullptr;
        uint64_t g_n = 0;
        PS2Runtime::RecompiledFunction g_free = nullptr, g_render = nullptr, g_alpha = nullptr, g_entRender = nullptr,
                                       g_delRw = nullptr;
        int g_logged = 0;
        uint32_t g_curEntity = 0;
        struct Del
        {
            uint32_t entity, clump, vtbl, model, frame, ras[6];
        };
        constexpr uint32_t kDelRing = 1u << 20;
        Del *g_del = nullptr;
        uint64_t g_nDel = 0;
        constexpr uint32_t kEntityRender = 0x00305AF8u; // CEntity::Render: rwObject +0x50 (atomic type 1 / clump)
        constexpr uint32_t kDeleteRwObject = 0x00304B28u; // CEntity::DeleteRwObject: +0x50 = 0 (0x304BE8)

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFCu), 4);
            return v;
        }
        bool inRam(uint32_t a) { return (a & 0xFE000000u) == 0 && a >= 0x100000u; }

        void stackRas(const uint8_t *ram, const R5900Context *ctx, uint32_t *out, int n)
        {
            const uint32_t sp = GPR_U32(ctx, 29) & 0x01FFFFFFu;
            int k = 0;
            for (uint32_t off = 0; off < 0x300u && k < n && sp + off + 4 <= 0x02000000u; off += 4)
            {
                const uint32_t w = rd32(ram, sp + off);
                if (w >= 0x100000u && w < 0x3A0000u && (w & 3u) == 0 && (rd32(ram, w - 8) >> 26) == 3u)
                    out[k++] = w;
            }
            for (; k < n; ++k)
                out[k] = 0;
        }

        PS2_GUEST_CC void freeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t p = GPR_U32(ctx, 5), heap = GPR_U32(ctx, 4);
            if (p && inRam(p))
            {
                Rec &r = g_ring[g_n++ & (kRing - 1)];
                r.p = p;
                r.size = p < rd32(rdram, heap + 0x10) ? rd32(rdram, p - 8) : 0x10u; // 0x150750: below +0x10 = a big block
                r.ra = GPR_U32(ctx, 31);
                r.frame = rd32(rdram, kFrames);
                stackRas(rdram, ctx, r.ras, 4);
            }
            g_free(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void entRenderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_curEntity = GPR_U32(ctx, 4);
            g_entRender(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void delRwHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t e = GPR_U32(ctx, 4) & 0x01FFFFFFu;
            Del &d = g_del[g_nDel++ & (kDelRing - 1)];
            d.entity = e;
            d.clump = rd32(rdram, e + 0x50);
            d.vtbl = rd32(rdram, e + 0x5C);
            d.model = rd32(rdram, e + 0x58) & 0xFFFFu;
            d.frame = rd32(rdram, kFrames);
            stackRas(rdram, ctx, d.ras, 6);
            g_delRw(rdram, ctx, runtime);
        }

        void reportEntity(uint8_t *rdram)
        {
            const uint32_t e = g_curEntity & 0x01FFFFFFu;
            const uint32_t clump = rd32(rdram, e + 0x50);
            std::fprintf(stderr, "[uaf]   entity 0x%x vtbl 0x%x model %u rwObject 0x%x type %u flags(+0x40) %08x %08x pos %08x %08x %08x\n", e,
                         rd32(rdram, e + 0x5C), rd32(rdram, e + 0x58) & 0xFFFFu, clump, rdram[clump & 0x01FFFFFFu],
                         rd32(rdram, e + 0x40), rd32(rdram, e + 0x44), rd32(rdram, e + 0x30), rd32(rdram, e + 0x34),
                         rd32(rdram, e + 0x38));
            int shown = 0;
            const uint64_t lo = g_nDel > kDelRing ? g_nDel - kDelRing : 0;
            for (uint64_t i = g_nDel; i > lo && shown < 4; --i)
            {
                const Del &d = g_del[(i - 1) & (kDelRing - 1)];
                if (d.entity == e || (d.clump && d.clump == clump))
                {
                    std::fprintf(stderr, "[uaf]   DeleteRwObject #%llu of %llu frame %u: entity 0x%x clump 0x%x vtbl 0x%x model %u stack %x %x %x %x %x %x\n",
                                 static_cast<unsigned long long>(i), static_cast<unsigned long long>(g_nDel), d.frame, d.entity,
                                 d.clump, d.vtbl, d.model, d.ras[0], d.ras[1], d.ras[2], d.ras[3], d.ras[4], d.ras[5]);
                    ++shown;
                }
            }
        }

        void reportCovering(const char *what, uint32_t a)
        {
            int shown = 0;
            const uint64_t lo = g_n > kRing ? g_n - kRing : 0;
            for (uint64_t i = g_n; i > lo && shown < 4; --i)
            {
                const Rec &r = g_ring[(i - 1) & (kRing - 1)];
                if (a >= r.p && a < r.p + (r.size & 0x01FFFFFFu))
                {
                    std::fprintf(stderr, "[uaf]   %s 0x%x in freed block 0x%x+0x%x (free #%llu of %llu, frame %u) ra=0x%x stack %x %x %x %x\n",
                                 what, a, r.p, r.size, static_cast<unsigned long long>(i), static_cast<unsigned long long>(g_n),
                                 r.frame, r.ra, r.ras[0], r.ras[1], r.ras[2], r.ras[3]);
                    ++shown;
                }
            }
            if (!shown)
                std::fprintf(stderr, "[uaf]   %s 0x%x: no recorded free covers it\n", what, a);
        }

        bool check(uint8_t *rdram, R5900Context *ctx, const char *who)
        {
            const uint32_t el = GPR_U32(ctx, 4) & 0x01FFFFFFu;
            const uint32_t geo = rd32(rdram, el + 0x14);
            const bool bad = !inRam(geo) || rd32(rdram, geo + 0x10) > 256u ||
                             (rd32(rdram, geo + 0x10) && !inRam(rd32(rdram, geo + 0xC)));
            if (!bad)
                return true;
            if (g_logged < 12)
            {
                ++g_logged;
                uint32_t ras[8];
                stackRas(rdram, ctx, ras, 8);
                std::fprintf(stderr, "[uaf] %s frame %u: element 0x%x geometry 0x%x count 0x%x mats 0x%x (el words %08x %08x %08x %08x %08x %08x %08x %08x) stack %x %x %x %x %x %x %x %x\n",
                             who, rd32(rdram, kFrames), el, geo, rd32(rdram, geo + 0x10), rd32(rdram, geo + 0xC),
                             rd32(rdram, el), rd32(rdram, el + 4), rd32(rdram, el + 8), rd32(rdram, el + 0xC),
                             rd32(rdram, el + 0x10), rd32(rdram, el + 0x14), rd32(rdram, el + 0x18), rd32(rdram, el + 0x1C),
                             ras[0], ras[1], ras[2], ras[3], ras[4], ras[5], ras[6], ras[7]);
                reportCovering("geometry", geo);
                reportCovering("element", el);
                reportCovering("owner(+0x18)", rd32(rdram, el + 0x18));
                reportEntity(rdram);
                std::fflush(stderr);
            }
            return !std::getenv("LCS_UAF_SKIP");
        }

        PS2_GUEST_CC void renderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!check(rdram, ctx, "render"))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_render(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void alphaHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!check(rdram, ctx, "alpha"))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_alpha(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!std::getenv("LCS_UAF_LOG"))
            return;
        g_ring = new Rec[kRing]();
        g_free = runtime.lookupFunction(kFree);
        g_render = runtime.lookupFunction(kRenderElement);
        g_alpha = runtime.lookupFunction(kRenderAlpha);
        g_del = new Del[kDelRing]();
        g_entRender = runtime.lookupFunction(kEntityRender);
        g_delRw = runtime.lookupFunction(kDeleteRwObject);
        if (!g_free || !g_render || !g_alpha || !g_entRender || !g_delRw)
            return;
        runtime.replaceFunction(kEntityRender, &entRenderHook);
        runtime.replaceFunction(kDeleteRwObject, &delRwHook);
        runtime.replaceFunction(kFree, &freeHook);
        runtime.replaceFunction(kRenderElement, &renderHook);
        runtime.replaceFunction(kRenderAlpha, &alphaHook);
        std::fprintf(stderr, "[uaf] free ring and element checks on\n");
    }
}
