// [lcs] See lcs_inflate.h. SLUS_214.23 (LCS USA).
//
// FUN_0022BE20(strm a0, start a1) is zlib 1.2.x's inflate_fast (inffast.c, PUP / OFF = 1 pre-increment form), as read
// from its Ghidra decompile and disassembly: strm->next_in +0x0, avail_in +0x4, next_out +0x10, avail_out +0x14 (the EE
// ABI's 64-bit total_in at +0x8), msg +0x20, state +0x24; the inflate_state's mode +0x0 (TYPE = 0xB, BAD = 0x1B),
// wsize +0x2C, whave +0x30, write +0x34, window +0x38, hold +0x40 (64 bits: `ld` / `sd`), bits +0x48, lencode +0x58,
// distcode +0x5C, lenbits +0x60, distbits +0x64; a code is {op u8, bits u8, val u16}. The game inflates in it when a
// teleport / load streams its compressed data: ~70-110 profiler samples of the ~200 ms frame of every teleport at 1000%
// draw distance (VCS_PROFILE_SPIKES, droidperf2 runs M1ww0), a guest loop of byte loads and stores.
//
// Native: the same decode on the guest's memory (same bytes out, same strm / state left). A corrupt stream (the three
// zlib errors) is left to the guest's code: nothing is written back to strm / state then, and the guest's run writes the
// same output bytes the native run wrote before it stopped, then the error. LCS_INFLATE=0: the guest's code;
// LCS_INFLATE=verify: native on a scratch copy, the guest's on the real memory, compared (output bytes, strm, state).
// Native and guest inflate the same way in place too (output over its own input, behind the read position): byte by
// byte, in the same order.
#include "game/lcs_inflate.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_host_std.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lcs::inflate
{
    namespace
    {
        constexpr uint32_t kInflateFast = 0x0022BE20u;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kRamSize = 0x02000000u;

        PS2Runtime::RecompiledFunction g_orig = nullptr;
        bool g_verify = false;
        std::atomic<uint64_t> g_calls{0}, g_fallbacks{0}, g_same{0}, g_differ{0};

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        // a guest range [a, a + n) inside RAM, no wrap
        bool inRam(uint32_t a, uint32_t n) { return (a & kRamMask) + uint64_t(n) <= kRamSize; }

        struct Code
        {
            uint8_t op, bits;
            uint16_t val;
        };

        // inflate_fast; false: a zlib error (or a pointer outside RAM) - nothing written to strm / state
        bool inflateFast(uint8_t *ram, uint32_t strm, uint32_t start)
        {
            const uint32_t state = rd32(ram, strm + 0x24u);
            const uint32_t nextIn = rd32(ram, strm + 0x0u), availIn = rd32(ram, strm + 0x4u);
            const uint32_t nextOut = rd32(ram, strm + 0x10u), availOut = rd32(ram, strm + 0x14u);
            const uint32_t wsize = rd32(ram, state + 0x2Cu), whave = rd32(ram, state + 0x30u), write = rd32(ram, state + 0x34u);
            const uint32_t windowG = rd32(ram, state + 0x38u);
            uint64_t hold;
            std::memcpy(&hold, ram + ((state + 0x40u) & kRamMask), 8);
            uint32_t bits = rd32(ram, state + 0x48u);
            const uint32_t lcodeG = rd32(ram, state + 0x58u), dcodeG = rd32(ram, state + 0x5Cu);
            const uint32_t lenbits = rd32(ram, state + 0x60u), distbits = rd32(ram, state + 0x64u);
            if (availIn < 6u || availOut < 258u || start < availOut || lenbits > 15u || distbits > 15u || !inRam(nextIn, availIn) ||
                !inRam(nextOut - (start - availOut), start) || (wsize && !inRam(windowG, wsize)) || !inRam(lcodeG, 4u << lenbits) ||
                !inRam(dcodeG, 4u << distbits))
                return false;
            const uint8_t *in = ram + (nextIn & kRamMask) - 1;   // PUP: pre-increment
            const uint8_t *last = in + (availIn - 5u);
            uint8_t *out = ram + (nextOut & kRamMask) - 1;
            const uint8_t *beg = out - (start - availOut);
            const uint8_t *end = out + (availOut - 257u);
            const uint8_t *window = ram + (windowG & kRamMask);
            const auto *lcode = reinterpret_cast<const uint8_t *>(ram + (lcodeG & kRamMask));
            const auto *dcode = reinterpret_cast<const uint8_t *>(ram + (dcodeG & kRamMask));
            const uint32_t lmask = (1u << lenbits) - 1u, dmask = (1u << distbits) - 1u;
            auto code = [](const uint8_t *table, uint32_t i) {
                Code c;
                std::memcpy(&c, table + size_t(i) * 4u, 4);
                return c;
            };
            Code here;
            uint32_t op, len, dist;
            const uint8_t *from;
            do
            {
                if (bits < 15u)
                {
                    hold += uint64_t(*++in) << bits;
                    bits += 8u;
                    hold += uint64_t(*++in) << bits;
                    bits += 8u;
                }
                here = code(lcode, uint32_t(hold) & lmask);
            dolen:
                op = here.bits;
                hold >>= op;
                bits -= op;
                op = here.op;
                if (op == 0u)
                    *++out = static_cast<uint8_t>(here.val); // literal
                else if (op & 16u)
                {
                    len = here.val; // length base
                    op &= 15u;
                    if (op)
                    {
                        if (bits < op)
                        {
                            hold += uint64_t(*++in) << bits;
                            bits += 8u;
                        }
                        len += uint32_t(hold) & ((1u << op) - 1u);
                        hold >>= op;
                        bits -= op;
                    }
                    if (bits < 15u)
                    {
                        hold += uint64_t(*++in) << bits;
                        bits += 8u;
                        hold += uint64_t(*++in) << bits;
                        bits += 8u;
                    }
                    here = code(dcode, uint32_t(hold) & dmask);
                dodist:
                    op = here.bits;
                    hold >>= op;
                    bits -= op;
                    op = here.op;
                    if (op & 16u)
                    {
                        dist = here.val; // distance base
                        op &= 15u;
                        if (bits < op)
                        {
                            hold += uint64_t(*++in) << bits;
                            bits += 8u;
                            if (bits < op)
                            {
                                hold += uint64_t(*++in) << bits;
                                bits += 8u;
                            }
                        }
                        dist += uint32_t(hold) & ((1u << op) - 1u);
                        hold >>= op;
                        bits -= op;
                        op = static_cast<uint32_t>(out - beg); // max distance in output
                        if (dist > op)
                        {
                            op = dist - op; // distance back in window
                            if (op > whave)
                                return false; // "invalid distance too far back"
                            from = window - 1;
                            if (write == 0u)
                            {
                                from += wsize - op;
                                if (op < len)
                                {
                                    len -= op;
                                    do
                                        *++out = *++from;
                                    while (--op);
                                    from = out - dist;
                                }
                            }
                            else if (write < op)
                            {
                                from += wsize + write - op;
                                op -= write;
                                if (op < len)
                                {
                                    len -= op;
                                    do
                                        *++out = *++from;
                                    while (--op);
                                    from = window - 1;
                                    if (write < len)
                                    {
                                        op = write;
                                        len -= op;
                                        do
                                            *++out = *++from;
                                        while (--op);
                                        from = out - dist;
                                    }
                                }
                            }
                            else
                            {
                                from += write - op;
                                if (op < len)
                                {
                                    len -= op;
                                    do
                                        *++out = *++from;
                                    while (--op);
                                    from = out - dist;
                                }
                            }
                            while (len > 2u)
                            {
                                *++out = *++from;
                                *++out = *++from;
                                *++out = *++from;
                                len -= 3u;
                            }
                            if (len)
                            {
                                *++out = *++from;
                                if (len > 1u)
                                    *++out = *++from;
                            }
                        }
                        else
                        {
                            from = out - dist; // copy direct from output
                            do
                            {
                                *++out = *++from;
                                *++out = *++from;
                                *++out = *++from;
                                len -= 3u;
                            } while (len > 2u);
                            if (len)
                            {
                                *++out = *++from;
                                if (len > 1u)
                                    *++out = *++from;
                            }
                        }
                    }
                    else if ((op & 64u) == 0u)
                    {
                        here = code(dcode, here.val + (uint32_t(hold) & ((1u << op) - 1u))); // 2nd level distance code
                        goto dodist;
                    }
                    else
                        return false; // "invalid distance code"
                }
                else if ((op & 64u) == 0u)
                {
                    here = code(lcode, here.val + (uint32_t(hold) & ((1u << op) - 1u))); // 2nd level length code
                    goto dolen;
                }
                else if (op & 32u)
                {
                    wr32(ram, state + 0x0u, 0xBu); // end-of-block: mode = TYPE
                    break;
                }
                else
                    return false; // "invalid literal/length code"
            } while (in < last && out < end);

            // return unused bytes (on entry, bits < 8, so in won't go too far back)
            len = bits >> 3;
            in -= len;
            bits -= len << 3;
            hold &= static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>((1u << bits) - 1u)));
            const uint32_t inOff = static_cast<uint32_t>(in + 1 - ram), outOff = static_cast<uint32_t>(out + 1 - ram);
            wr32(ram, strm + 0x0u, (nextIn & ~kRamMask) | inOff);
            wr32(ram, strm + 0x10u, (nextOut & ~kRamMask) | outOff);
            wr32(ram, strm + 0x4u, in < last ? 5u + static_cast<uint32_t>(last - in) : 5u - static_cast<uint32_t>(in - last));
            wr32(ram, strm + 0x14u, out < end ? 257u + static_cast<uint32_t>(end - out) : 257u - static_cast<uint32_t>(out - end));
            std::memcpy(ram + ((state + 0x40u) & kRamMask), &hold, 8);
            wr32(ram, state + 0x48u, bits);
            return true;
        }

        void report()
        {
            static auto at = ps2x::hostNow();
            if (ps2x::hostNow() - at < std::chrono::seconds(5))
                return;
            at = ps2x::hostNow();
            std::fprintf(stderr, "[lcs-inflate] %llu calls, %llu left to the guest%s", (unsigned long long)g_calls.load(),
                         (unsigned long long)g_fallbacks.load(), g_verify ? "" : "\n");
            if (g_verify)
                std::fprintf(stderr, "; verify: %llu same, %llu differ\n", (unsigned long long)g_same.load(),
                             (unsigned long long)g_differ.load());
        }

        PS2_GUEST_CC void inflateFastHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t strm = GPR_U32(ctx, 4), start = GPR_U32(ctx, 5), ra = GPR_U32(ctx, 31);
            g_calls.fetch_add(1, std::memory_order_relaxed);
            if (!g_verify)
            {
                if (inflateFast(rdram, strm, start))
                {
                    report();
                    ctx->pc = ra;
                    return;
                }
                g_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return g_orig(rdram, ctx, runtime);
            }
            // verify: native on a scratch copy of what it reads (strm, state, input, output buffer, window, tables at
            // their own offsets), the guest's run on the real memory. A guest run that ends here is compared at once
            // (output bytes, strm, state); one that stops part-way (an EE checkpoint in its loop) is compared at the
            // next call on the same strm (its output bytes: inflate's slow path writes only past them)
            static std::vector<uint8_t> scratch(kRamSize);
            struct Pending
            {
                uint32_t strm = 0, outAt = 0;
                std::vector<uint8_t> out;
            };
            static Pending pending;
            auto compareOut = [&](const Pending &p, const char *when) {
                const bool same = inRam(p.outAt, static_cast<uint32_t>(p.out.size())) &&
                                  std::memcmp(p.out.data(), rdram + p.outAt, p.out.size()) == 0;
                (same ? g_same : g_differ).fetch_add(1, std::memory_order_relaxed);
                static int logged = 0;
                if (!same && logged++ < 10)
                    std::fprintf(stderr, "[lcs-inflate] verify: call %llu output differs %s (strm 0x%x, %zu bytes)\n",
                                 (unsigned long long)g_calls.load(), when, p.strm, p.out.size());
            };
            // the same stream going on: its output continues at or after the pending run's end (a z_stream on the stack
            // is reused by the next inflate, whose buffer the game has consumed / rewritten - not compared)
            if (pending.strm == strm && (rd32(rdram, strm + 0x10u) & kRamMask) >= pending.outAt + pending.out.size() &&
                (rd32(rdram, strm + 0x10u) & kRamMask) - (pending.outAt + pending.out.size()) < 0x10000u)
                compareOut(pending, "(checked at the next call)");
            pending.strm = 0;
            const uint32_t state = rd32(rdram, strm + 0x24u);
            const uint32_t inAt = rd32(rdram, strm) & kRamMask, inLen = rd32(rdram, strm + 0x4u);
            const uint32_t outStart = rd32(rdram, strm + 0x10u), availOut = rd32(rdram, strm + 0x14u);
            const uint32_t outAt = (outStart - (start - availOut)) & kRamMask, outLen = start;
            const uint32_t window = rd32(rdram, state + 0x38u) & kRamMask, wsize = rd32(rdram, state + 0x2Cu);
            const uint32_t lcode = rd32(rdram, state + 0x58u) & kRamMask, dcode = rd32(rdram, state + 0x5Cu) & kRamMask;
            const uint32_t lenbits = rd32(rdram, state + 0x60u), distbits = rd32(rdram, state + 0x64u);
            if (start < availOut || lenbits > 15u || distbits > 15u || !inRam(inAt, inLen + 1u) || !inRam(outAt, outLen + 1u) ||
                !inRam(window, wsize) || !inRam(lcode, 4u << lenbits) || !inRam(dcode, 4u << distbits) ||
                !inRam(strm & kRamMask, 0x28u) || !inRam(state & kRamMask, 0x68u))
            {
                g_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return g_orig(rdram, ctx, runtime);
            }
            auto copy = [&](uint32_t at, uint32_t n) { std::memcpy(scratch.data() + at, rdram + at, n); };
            copy(strm & kRamMask, 0x28u);
            copy(state & kRamMask, 0x68u);
            copy(window, wsize);
            copy(lcode, 4u << lenbits);
            copy(dcode, 4u << distbits);
            copy(outAt, outLen + 1u);
            copy(inAt, inLen + 1u); // last: the input wins where the output buffer overlaps it (in place)
            if (!inflateFast(scratch.data(), strm, start))
            {
                g_fallbacks.fetch_add(1, std::memory_order_relaxed);
                return g_orig(rdram, ctx, runtime);
            }
            const uint32_t nativeOutEnd = rd32(scratch.data(), strm + 0x10u);
            Pending p;
            p.strm = strm;
            p.outAt = outStart & kRamMask;
            p.out.assign(scratch.data() + p.outAt, scratch.data() + p.outAt + (nativeOutEnd - outStart));
            g_orig(rdram, ctx, runtime);
            if (ctx->pc != ra)
            {
                pending = std::move(p); // stopped part-way: compared at the next call on this strm
                return;
            }
            compareOut(p, "(at once)");
            bool fields = true;
            for (uint32_t f : {0x0u, 0x4u, 0x10u, 0x14u})
                fields = fields && rd32(rdram, strm + f) == rd32(scratch.data(), strm + f);
            for (uint32_t f : {0x0u, 0x40u, 0x44u, 0x48u})
                fields = fields && rd32(rdram, state + f) == rd32(scratch.data(), state + f);
            if (!fields)
            {
                g_differ.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[lcs-inflate] verify: call %llu strm / state differ\n", (unsigned long long)g_calls.load());
            }
            report();
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("LCS_INFLATE");
        if (e && e[0] == '0')
            return;
        g_verify = e && !std::strcmp(e, "verify");
        if (!(g_orig = runtime.lookupFunction(kInflateFast)))
        {
            std::fprintf(stderr, "[lcs-inflate] 0x%x not found: the guest's inflate\n", kInflateFast);
            return;
        }
        runtime.replaceFunction(kInflateFast, &inflateFastHook);
        std::fprintf(stderr, "[lcs-inflate] inflate_fast native%s\n", g_verify ? " (verify: both, compared)" : "");
    }
}
