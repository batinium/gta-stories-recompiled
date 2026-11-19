// Based on Blackline Interactive implementation
#include "runtime/ps2_vu1_tap.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu_code_log.h" // [gta-recomp]
#include "runtime/ps2_render_worker.h" // [gta-recomp]
#include "runtime/ps2_chain_observer.h" // [gta-recomp]
#include "runtime/gs/gs_frontend.h"     // [gta-recomp] GSImageBytes, gsSetCurrentGifPath
#include <cstring>
#include "runtime/ps2_host_std.h"

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>

// [gta-recomp] PS2X_LOG_MPG=1 logs every distinct VU1 microcode upload once;
// PS2X_DUMP_MPG_DIR=<dir> also writes each one to <dir>/mpg_<vuaddr>_<hash>.bin.
static void ps2xLogMpgUpload(uint32_t destAddr, uint32_t pairs, const uint8_t *code, uint32_t bytes)
{
    static const bool enabled = std::getenv("PS2X_LOG_MPG") != nullptr || std::getenv("PS2X_DUMP_MPG_DIR") != nullptr;
    if (!enabled)
        return;
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < bytes; ++i)
        hash = (hash ^ code[i]) * 1099511628211ull;
    static std::mutex mutex;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(hash ^ destAddr).second)
        return;
    std::fprintf(stderr, "[vif1:mpg] new microcode vu=0x%04x pairs=%u hash=%016llx\n", destAddr, pairs,
                 static_cast<unsigned long long>(hash));
    if (const char *dir = std::getenv("PS2X_DUMP_MPG_DIR"))
    {
        char name[512];
        std::snprintf(name, sizeof(name), "%s/mpg_%04x_%016llx.bin", dir, destAddr, static_cast<unsigned long long>(hash));
        if (FILE *f = std::fopen(name, "wb"))
        {
            std::fwrite(code, 1, bytes, f);
            std::fclose(f);
        }
    }
}

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }

    // [gta-recomp] UNPACK with the format known at compile time, for every case the general loop in processVIF1Data
    // decodes (all but V2/V3-5 = vl 3 with vn != 3): masks, CL != WL (skipping and filling writes), MODE 1 / 2. The same
    // writes as that loop, vector for vector - without its per-vector divisions (cycle position, block), per-vector format
    // switch and per-field mask shifts. LCS's world and model packets are mostly masked / CL != WL UNPACKs: the general
    // loop was ~30% of the render worker in Fort Staunton. PS2X_VIF_UNPACK_FAST=1: only the older unmasked CL == WL fast
    // path; 0: the general loop alone; verify: this one checked against the general loop.
    template <uint32_t VL, uint32_t VN>
    inline void unpackDecode(const uint8_t *v, uint32_t (&out)[4], bool zeroExtend)
    {
        constexpr uint32_t comps = VN + 1u;
        if constexpr (VL == 0u)
        {
            if constexpr (comps == 1u)
            {
                uint32_t s;
                std::memcpy(&s, v, 4u);
                out[0] = out[1] = out[2] = out[3] = s;
            }
            else
                std::memcpy(out, v, comps * 4u);
        }
        else if constexpr (VL == 1u)
        {
            for (uint32_t c = 0; c < comps; ++c)
            {
                uint16_t raw;
                std::memcpy(&raw, v + c * 2u, 2u);
                out[c] = zeroExtend ? raw : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
            }
            if constexpr (comps == 1u)
                out[1] = out[2] = out[3] = out[0];
        }
        else if constexpr (VL == 2u)
        {
            for (uint32_t c = 0; c < comps; ++c)
                out[c] = zeroExtend ? v[c] : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v[c])));
            if constexpr (comps == 1u)
                out[1] = out[2] = out[3] = out[0];
        }
        else
        {
            static_assert(VN == 3u, "V4-5 only");
            uint16_t packed;
            std::memcpy(&packed, v, 2u);
            out[0] = (packed & 0x001Fu) << 3u;
            out[1] = (packed & 0x03E0u) >> 2u;
            out[2] = (packed & 0x7C00u) >> 7u;
            out[3] = (packed & 0x8000u) >> 8u;
        }
    }

    struct UnpackArgs
    {
        uint8_t *vu;        // VU1 data memory (16 KB)
        const uint8_t *src; // the UNPACK's data
        uint32_t vuAddr, writeCount, cl, wl, mode, mask;
        bool maskEnable, zeroExtend;
        uint32_t *row;       // vif1 ROW (MODE 2 writes it)
        const uint32_t *col; // vif1 COL
    };

    template <uint32_t VL, uint32_t VN>
    void unpackTyped(const UnpackArgs &a)
    {
        constexpr uint32_t comps = VN + 1u;
        constexpr uint32_t bytesPerVector = VL == 3u ? 2u : comps * (VL == 0u ? 4u : VL == 1u ? 2u : 1u);
        constexpr bool canAdd = VL != 3u;
        // the mask's choice per field for cycle positions 0..3 (later positions use 3's, as the general loop)
        uint8_t spec[4][4];
        for (uint32_t c = 0; c < 4u; ++c)
            for (uint32_t f = 0; f < 4u; ++f)
                spec[c][f] = a.maskEnable ? static_cast<uint8_t>((a.mask >> ((c * 4u + f) * 2u)) & 3u) : 0u;
        const bool add = canAdd && (a.mode == 1u || a.mode == 2u);
        const bool accumulate = a.mode == 2u;
        const bool skipping = a.cl >= a.wl; // else filling: positions >= CL take no source vector
        uint32_t cyc = 0u, blockBase = a.vuAddr, srcIndex = 0u;
        for (uint32_t w = 0; w < a.writeCount; ++w)
        {
            const uint32_t dest = (skipping ? blockBase + cyc : a.vuAddr + w) & 0x3FFu;
            uint8_t *dst = a.vu + dest * 16u;
            const uint32_t clamp = cyc > 3u ? 3u : cyc;
            const uint8_t *sp = spec[clamp];
            uint32_t lanes[4];
            std::memcpy(lanes, dst, sizeof(lanes));
            if (skipping || cyc < a.cl)
            {
                uint32_t dec[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                unpackDecode<VL, VN>(a.src + srcIndex * bytesPerVector, dec, a.zeroExtend);
                ++srcIndex;
                for (uint32_t f = 0; f < 4u; ++f)
                {
                    switch (sp[f])
                    {
                    case 0u:
                    {
                        uint32_t v = dec[f];
                        if (add)
                        {
                            v += a.row[f];
                            if (accumulate)
                                a.row[f] = v;
                        }
                        lanes[f] = v;
                        break;
                    }
                    case 1u:
                        lanes[f] = a.row[f];
                        break;
                    case 2u:
                        lanes[f] = a.col[clamp];
                        break;
                    default:
                        break; // write-protect
                    }
                }
            }
            else
            {
                for (uint32_t f = 0; f < 4u; ++f) // a filling write: data selections take ROW
                {
                    const uint8_t s = sp[f];
                    if (s <= 1u)
                        lanes[f] = a.row[f];
                    else if (s == 2u)
                        lanes[f] = a.col[clamp];
                }
            }
            std::memcpy(dst, lanes, sizeof(lanes));
            if (++cyc == a.wl)
            {
                cyc = 0u;
                blockBase += a.cl;
            }
        }
    }

    // [gta-recomp] The format's fields of one vector as four lanes (`old` where the format supplies none, as the typed
    // path's decode leaves them)
    template <uint32_t VL, uint32_t VN>
    inline __m128i unpackDecodeVector(const uint8_t *v, __m128i old, bool zeroExtend)
    {
        constexpr uint32_t comps = VN + 1u;
        if constexpr (VL == 0u)
        {
            if constexpr (comps == 1u)
            {
                int32_t s;
                std::memcpy(&s, v, 4u);
                return _mm_set1_epi32(s);
            }
            else if constexpr (comps == 4u)
                return _mm_loadu_si128(reinterpret_cast<const __m128i *>(v));
            else
            {
                alignas(16) uint32_t lanes[4];
                _mm_store_si128(reinterpret_cast<__m128i *>(lanes), old);
                std::memcpy(lanes, v, comps * 4u);
                return _mm_load_si128(reinterpret_cast<const __m128i *>(lanes));
            }
        }
        else if constexpr (VL == 3u)
        {
            uint16_t packed;
            std::memcpy(&packed, v, 2u);
            return _mm_setr_epi32(int((packed & 0x001Fu) << 3u), int((packed & 0x03E0u) >> 2u), int((packed & 0x7C00u) >> 7u),
                                  int((packed & 0x8000u) >> 8u));
        }
        else
        {
            // 16- / 8-bit fields widened to 32 bits, sign- or zero-extended
            __m128i raw;
            if constexpr (VL == 1u)
            {
                uint64_t bits = 0;
                std::memcpy(&bits, v, comps * 2u);
                raw = _mm_cvtsi64_si128(static_cast<long long>(bits));
                raw = zeroExtend ? _mm_unpacklo_epi16(raw, _mm_setzero_si128()) : _mm_srai_epi32(_mm_unpacklo_epi16(raw, raw), 16);
            }
            else
            {
                uint32_t bits = 0;
                std::memcpy(&bits, v, comps);
                raw = _mm_cvtsi32_si128(static_cast<int>(bits));
                const __m128i w16 = _mm_unpacklo_epi8(raw, raw);
                raw = zeroExtend ? _mm_srli_epi32(_mm_unpacklo_epi16(w16, w16), 24) : _mm_srai_epi32(_mm_unpacklo_epi16(w16, w16), 24);
            }
            if constexpr (comps == 1u)
                return _mm_shuffle_epi32(raw, 0x00);
            else if constexpr (comps == 4u)
                return raw;
            else
            {
                const __m128i fmt = comps == 2u ? _mm_setr_epi32(-1, -1, 0, 0) : _mm_setr_epi32(-1, -1, -1, 0);
                return _mm_or_si128(_mm_and_si128(raw, fmt), _mm_andnot_si128(fmt, old));
            }
        }
    }

    // [gta-recomp] unpackTyped without MODE 1 / 2 (no ROW arithmetic): each write a blend of the decoded vector, ROW, the
    // cycle's COL and the old contents by lane masks made once per UNPACK (the typed path's per-field mask switch was
    // ~20% of the render worker in Fort Staunton). PS2X_VIF_UNPACK_SIMD=0: the typed path for every mode.
    template <uint32_t VL, uint32_t VN>
    void unpackBlend(const UnpackArgs &a)
    {
        constexpr uint32_t comps = VN + 1u;
        constexpr uint32_t bytesPerVector = VL == 3u ? 2u : comps * (VL == 0u ? 4u : VL == 1u ? 2u : 1u);
        // per cycle position: the lanes taking the data, the ROW / COL constants of the others, the write-protected lanes;
        // a filling write (no source vector) takes ROW for data selections too
        __m128i dataMask[4], constVal[4], keepMask[4], fillConst[4];
        const __m128i row = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a.row));
        for (uint32_t c = 0; c < 4u; ++c)
        {
            alignas(16) int32_t d[4], r[4], k[4], p[4], f[4];
            for (uint32_t field = 0; field < 4u; ++field)
            {
                const uint32_t s = a.maskEnable ? (a.mask >> ((c * 4u + field) * 2u)) & 3u : 0u;
                d[field] = s == 0u ? -1 : 0;
                r[field] = s == 1u ? -1 : 0;
                k[field] = s == 2u ? -1 : 0;
                p[field] = s == 3u ? -1 : 0;
                f[field] = s <= 1u ? -1 : 0;
            }
            const __m128i col = _mm_set1_epi32(static_cast<int>(a.col[c]));
            const __m128i colLanes = _mm_and_si128(col, _mm_load_si128(reinterpret_cast<const __m128i *>(k)));
            dataMask[c] = _mm_load_si128(reinterpret_cast<const __m128i *>(d));
            constVal[c] = _mm_or_si128(_mm_and_si128(row, _mm_load_si128(reinterpret_cast<const __m128i *>(r))), colLanes);
            keepMask[c] = _mm_load_si128(reinterpret_cast<const __m128i *>(p));
            fillConst[c] = _mm_or_si128(_mm_and_si128(row, _mm_load_si128(reinterpret_cast<const __m128i *>(f))), colLanes);
        }
        const bool skipping = a.cl >= a.wl;
        uint32_t cyc = 0u, blockBase = a.vuAddr, srcIndex = 0u;
        for (uint32_t w = 0; w < a.writeCount; ++w)
        {
            const uint32_t dest = (skipping ? blockBase + cyc : a.vuAddr + w) & 0x3FFu;
            __m128i *dst = reinterpret_cast<__m128i *>(a.vu + dest * 16u);
            const uint32_t clamp = cyc > 3u ? 3u : cyc;
            const __m128i old = _mm_loadu_si128(dst);
            __m128i out = _mm_and_si128(old, keepMask[clamp]);
            if (skipping || cyc < a.cl)
            {
                const __m128i dec = unpackDecodeVector<VL, VN>(a.src + srcIndex * bytesPerVector, old, a.zeroExtend);
                ++srcIndex;
                out = _mm_or_si128(out, _mm_or_si128(_mm_and_si128(dec, dataMask[clamp]), constVal[clamp]));
            }
            else
                out = _mm_or_si128(out, fillConst[clamp]);
            _mm_storeu_si128(dst, out);
            if (++cyc == a.wl)
            {
                cyc = 0u;
                blockBase += a.cl;
            }
        }
    }

    // false: a format the typed paths do not take (vl 3 with vn != 3)
    bool unpackTypedDispatch(uint32_t vl, uint32_t vn, const UnpackArgs &a)
    {
        static const bool simd = [] {
            const char *e = std::getenv("PS2X_VIF_UNPACK_SIMD");
            return !(e && *e == '0');
        }();
        if (simd && a.mode != 1u && a.mode != 2u)
        {
            switch (vl * 4u + vn)
            {
            case 0u: unpackBlend<0u, 0u>(a); return true;
            case 1u: unpackBlend<0u, 1u>(a); return true;
            case 2u: unpackBlend<0u, 2u>(a); return true;
            case 3u: unpackBlend<0u, 3u>(a); return true;
            case 4u: unpackBlend<1u, 0u>(a); return true;
            case 5u: unpackBlend<1u, 1u>(a); return true;
            case 6u: unpackBlend<1u, 2u>(a); return true;
            case 7u: unpackBlend<1u, 3u>(a); return true;
            case 8u: unpackBlend<2u, 0u>(a); return true;
            case 9u: unpackBlend<2u, 1u>(a); return true;
            case 10u: unpackBlend<2u, 2u>(a); return true;
            case 11u: unpackBlend<2u, 3u>(a); return true;
            case 15u: unpackBlend<3u, 3u>(a); return true;
            default: return false;
            }
        }
        switch (vl * 4u + vn)
        {
        case 0u: unpackTyped<0u, 0u>(a); return true;
        case 1u: unpackTyped<0u, 1u>(a); return true;
        case 2u: unpackTyped<0u, 2u>(a); return true;
        case 3u: unpackTyped<0u, 3u>(a); return true;
        case 4u: unpackTyped<1u, 0u>(a); return true;
        case 5u: unpackTyped<1u, 1u>(a); return true;
        case 6u: unpackTyped<1u, 2u>(a); return true;
        case 7u: unpackTyped<1u, 3u>(a); return true;
        case 8u: unpackTyped<2u, 0u>(a); return true;
        case 9u: unpackTyped<2u, 1u>(a); return true;
        case 10u: unpackTyped<2u, 2u>(a); return true;
        case 11u: unpackTyped<2u, 3u>(a); return true;
        case 15u: unpackTyped<3u, 3u>(a); return true;
        default: return false;
        }
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            static const bool logMpg = std::getenv("PS2X_VU0_CODE_LOG") != nullptr; // [gta-recomp] debug
            if (logMpg)
                std::fprintf(stderr, "[vif0] MPG dest=0x%04x count=%u bytes left=%u%s\n", destAddr, instructionCount,
                             sizeBytes - pos, pos + mpgBytes > sizeBytes ? " (TRUNCATED)" : "");
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

// [gta-recomp] Feed DIRECT/DIRECTHL data to GIF PATH2. An IMAGE GIFtag left short by an
// earlier DIRECT is finished first: its data arrives through later DIRECTs, never as raw
// VIF stream bytes (those are VIFcodes, e.g. the NOP/DIRECTHL in the next DMAtag).
void PS2Memory::submitVif1DirectData(const uint8_t *data, uint32_t qwCount, bool directHl)
{
    if (m_vif1PendingPath2ImageQwc != 0u && qwCount != 0u)
    {
        const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, qwCount);
        thread_local std::vector<uint8_t> imagePacket; // [gta-recomp] reused: one allocation per texture upload before
        imagePacket.resize(16u + static_cast<size_t>(chunkQw) * 16u);
        std::memset(imagePacket.data() + 8u, 0, 8u);
        const uint64_t imageTag =
            static_cast<uint64_t>(chunkQw & 0x7FFFu) |
            ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
            (static_cast<uint64_t>(kGifFmtImage) << 58);
        std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
        std::memcpy(imagePacket.data() + 16u, data, static_cast<size_t>(chunkQw) * 16u);
        submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);
        m_vif1PendingPath2ImageQwc -= chunkQw;
        if (m_vif1PendingPath2ImageQwc == 0u)
            m_vif1PendingPath2DirectHl = false;
        data += chunkQw * 16u;
        qwCount -= chunkQw;
    }
    if (qwCount == 0u)
        return;

    { // [gta-recomp] PS2X_TRACE_PATH2_TAGS=1: DIRECT packets by the shape of their first GIFtag, every 5 s
        static const bool trace = std::getenv("PS2X_TRACE_PATH2_TAGS") != nullptr;
        if (trace)
        {
            static std::map<std::string, uint64_t> shapes;
            static auto last = ps2x::hostNow();
            uint64_t lo, hi;
            std::memcpy(&lo, data, 8);
            std::memcpy(&hi, data + 8, 8);
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            const uint64_t bytes = flg == 0u ? 16ull + uint64_t(nloop) * (nreg ? nreg : 16u) * 16ull : 0ull;
            char key[160];
            std::snprintf(key, sizeof(key), "flg %u nreg %u regs %0*llx pre %u prim 0x%03llx %s%s", flg, nreg, int(nreg ? nreg : 16),
                          (unsigned long long)(nreg ? hi & ((1ull << (4 * nreg)) - 1ull) : hi), unsigned((lo >> 46) & 1u),
                          (unsigned long long)((lo >> 47) & 0x7FFu), bytes == qwCount * 16ull ? "one tag" : "more",
                          ((lo >> 15) & 1u) ? "" : " no-eop");
            ++shapes[key];
            if (flg == 0u && nreg == 1u && (hi & 0xFu) == 0xEu) // A+D: the register sequence of one that draws (PRIM written)
            {
                std::string regs;
                bool prim = false;
                for (uint32_t i = 0; i < nloop && i < 24u && 16u + (i + 1u) * 16u <= qwCount * 16u; ++i)
                {
                    uint64_t value, addr;
                    std::memcpy(&value, data + 16u + i * 16u, 8);
                    std::memcpy(&addr, data + 24u + i * 16u, 8);
                    char r[32];
                    std::snprintf(r, sizeof(r), addr == 0u ? " PRIM=%llx" : " %llx", (unsigned long long)(addr == 0u ? value : addr & 0xFFu));
                    regs += r;
                    prim |= (addr & 0xFFu) == 0u;
                }
                if (prim)
                    ++shapes["A+D drawing, nloop " + std::to_string(nloop) + ":" + regs];
            }
            const auto now = ps2x::hostNow();
            if (now - last > std::chrono::seconds(5))
            {
                last = now;
                for (const auto &[k, n] : shapes)
                    std::fprintf(stderr, "[path2] %8llu  %s\n", (unsigned long long)n, k.c_str());
                shapes.clear();
            }
        }
    }
    submitGifPacket(GifPathId::Path2, data, qwCount * 16u, true, directHl);
    const uint32_t pendingImageQw = pendingGifImageQwc(data, qwCount * 16u);
    if (pendingImageQw != 0u)
    {
        m_vif1PendingPath2ImageQwc = pendingImageQw;
        m_vif1PendingPath2DirectHl = directHl;
    }
}

// [gta-recomp] MSCAL: TOP / ITOP from TOPS / ITOPS, the double buffer flipped, then the HLE hooks / the VU1 (shared by the
// interpreter and PS2Memory::vu1Mscal)
void PS2Memory::vu1MscalInternal(uint32_t startPC, const ps2hle::ChainSegment *segment, bool survey, const uint8_t *data, uint32_t sizeBytes)
{
    const uint32_t runTop = vif1_regs.tops & 0x3FFu;
    const uint32_t runItop = vif1_regs.itops & 0x3FFu;
    vif1_regs.top = runTop;
    vif1_regs.itop = runItop;

    const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
    if (dbf)
        vif1_regs.tops = vif1_regs.base & 0x3FFu;
    else
        vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
    vif1_regs.stat ^= (1u << 7); // toggle DBF

    const ps2hle::MscalBefore hleBefore = ps2hle::mscalBefore(); // [gta-recomp] game HLE / checkers
    const ps2hle::MscalAfter hleAfter = ps2hle::mscalAfter();
    ps2hle::MscalContext hle;
    hle.startPc = startPC;
    hle.top = runTop;
    hle.itop = runItop;
    hle.vuCode = m_vu1Code;
    hle.vuData = m_vu1Data;
    hle.codeGeneration = m_vu1CodeGeneration.load(std::memory_order_relaxed);
    hle.segment = segment;
    if (survey)
        ps2hle::beforeMscal(segment, startPC, m_vu1Code, m_vu1Data, data, sizeBytes);
    const bool handled = hleBefore && hleBefore(hle);
    if (!handled && m_vu1MscalCallback)
        m_vu1MscalCallback(startPC, runTop, runItop);
    if (!handled && hleAfter)
        hleAfter(hle);
    if (survey)
    {
        ps2vu1tap::g_mscalNative = handled; // [gta-recomp] runtime/ps2_vu1_tap.h
        ps2hle::afterMscal(m_vu1Data);
    }
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (ps2_render_worker::enabled() && !ps2_render_worker::onWorker()) // [gta-recomp] a game-thread path not moved to the worker
        ps2_render_worker::fence(ps2_render_worker::FenceReason::Other);
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    // [gta-recomp] PS2X_HLE_SURVEY: the CALLed packet (segment) the VIFcode at `at` belongs to
    const bool survey = ps2hle::surveyEnabled();
    size_t segCursor = 0;
    auto segmentAt = [&](uint32_t at) -> const ps2hle::ChainSegment *
    {
        if (!m_vif1Segments)
            return nullptr;
        const auto &segs = *m_vif1Segments;
        while (segCursor < segs.size() && segs[segCursor].end <= at)
            ++segCursor;
        const ps2hle::ChainSegment *inner = nullptr;
        for (size_t i = segCursor; i < segs.size() && segs[i].begin <= at; ++i) // nested CALLs follow their parent
            if (at < segs[i].end)
                inner = &segs[i];
        return inner;
    };

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1DirectRemainQwc != 0u)
        {
            // Rest of a DIRECT cut off by the previous buffer.
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
                break;
            const uint32_t chunkQw = std::min<uint32_t>(m_vif1DirectRemainQwc, availableQw);
            submitVif1DirectData(data + pos, chunkQw, m_vif1DirectRemainHl);
            pos += chunkQw * 16u;
            m_vif1DirectRemainQwc -= chunkQw;
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        if (ps2chain::VifRecorder *recorder = ps2chain::vifRecorder()) // [gta-recomp] every code but MSCAL / UNPACK (reported there)
            if (opcode != VIF_MSCAL && (opcode & 0x60u) != 0x60u)
                recorder->other(cmd);
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            // [gta-recomp] a marked REF's / CALL's work (runtime/ps2_chain_observer.h)
            pos += std::min<uint32_t>(ps2chain::notifyMark(*this, imm, data + pos, sizeBytes - pos), sizeBytes - pos);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;
            if (ps2chain::VifRecorder *recorder = ps2chain::vifRecorder()) // [gta-recomp] runtime/ps2_chain_observer.h
                recorder->mscal(startPC);
            ps2chain::setStreamOffset(pos - 4u); // [gta-recomp] the MSCAL's place in the stream (runtime/ps2_chain_observer.h)
            vu1MscalInternal(startPC, survey ? segmentAt(pos - 4u) : nullptr, survey, data, sizeBytes);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (ps2chain::VifRecorder *recorder = ps2chain::vifRecorder()) // [gta-recomp] the code, for a recording's replay
                recorder->mpg(cmd, data + pos, std::min<uint32_t>(mpgBytes, sizeBytes - pos), pos + mpgBytes <= sizeBytes);
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                // [gta-recomp] Games (VCS) re-upload the same microcode every frame; only an actual change
                // may invalidate the VU1 decode cache (a full re-decode per upload cost ~5% of the frame).
                if (pos + copyBytes <= sizeBytes && std::memcmp(m_vu1Code + destAddr, data + pos, copyBytes) != 0)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    ps2xLogMpgUpload(destAddr, instructionCount, data + pos, copyBytes);
                    markVU1CodeModified();
                    // [gta-recomp] the range, so the decode cache re-decodes only it (ps2_vu_code_log)
                    ps2_vu_code_log::record(this, getVU1CodeGeneration(), destAddr, destAddr + copyBytes);
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == ps2chain::kVifChainImage) // [gta-recomp] an interned DIRECT payload (runtime/ps2_chain_observer.h)
        {
            if (pos + 8u > sizeBytes)
                break;
            uint64_t raw;
            std::memcpy(&raw, data + pos, 8u);
            pos += 8u;
            // When it is exactly the pixels of the IMAGE upload a header DIRECT left waiting, and PATH2 goes straight to the
            // GS (submitGifPacket's in-place case), the GS gets the interned bytes as a reference (gsSubmitImageShared:
            // hash known, nothing copied); otherwise its bytes go the DIRECT way.
            GSImageBytes *image = reinterpret_cast<GSImageBytes *>(static_cast<uintptr_t>(raw));
            const uint32_t qwCount = imm == 0u ? 65536u : imm;
            const bool directHl = (num & 1u) != 0u;
            if (m_vif1PendingPath2ImageQwc == qwCount && image->bytes.size() == uint64_t(qwCount) * 16u && m_gifArbiter &&
                m_gifArbiter->empty() && m_gifPacketCallback)
            {
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                gsSetCurrentGifPath(2u);
                gsSubmitImageShared(image);
                gsSetCurrentGifPath(3u);
                continue;
            }
            if (image->bytes.size() >= uint64_t(qwCount) * 16u)
                submitVif1DirectData(image->bytes.data(), qwCount, directHl);
            image->release();
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            const bool directHl = (opcode == VIF_DIRECTHL);
            if (ps2chain::VifRecorder *recorder = ps2chain::vifRecorder()) // [gta-recomp] runtime/ps2_chain_observer.h
                recorder->direct(data + pos, qwCount, directHl, !truncated);
            ps2chain::notifyDirect(data + pos, qwCount * 16u); // [gta-recomp] runtime/ps2_chain_observer.h
            if (qwCount > 0)
                submitVif1DirectData(data + pos, qwCount, directHl);

            pos += qwCount * 16;
            if (truncated)
            {
                m_vif1DirectRemainQwc = (imm == 0u ? 65536u : imm) - qwCount;
                m_vif1DirectRemainHl = directHl;
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;
            if (survey)
                ps2hle::onUnpack(segmentAt(pos - 4u), opcode, num, imm);

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            // PS2X_VIF_UNPACK_FAST=0: the general loop only; =verify: the typed path and the general loop, compared
            // (reported as [vif1-unpack-verify]); =1: the unmasked CL == WL fast path below, the general loop for the rest;
            // default: the typed path (unpackTyped, above) for every format it takes
            static const int unpackFast = [] {
                const char *e = std::getenv("PS2X_VIF_UNPACK_FAST");
                return !e ? 3 : *e == '0' ? 0 : *e == 'v' ? 2 : *e == '1' ? 1 : 3;
            }();
            bool fastDone = false;
            std::vector<uint8_t> verifyBefore, verifyFast;
            uint32_t verifyRowBefore[4] = {}, verifyRowFast[4] = {};
            if (unpackFast >= 2 && m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && (vl != 3u || vn == 3u)) // [gta-recomp]
            {
                if (unpackFast == 2)
                {
                    verifyBefore.assign(m_vu1Data, m_vu1Data + PS2_VU1_DATA_SIZE);
                    std::memcpy(verifyRowBefore, vif1_regs.row, sizeof(verifyRowBefore));
                }
                UnpackArgs a;
                a.vu = m_vu1Data;
                a.src = data + pos;
                a.vuAddr = vuAddr;
                a.writeCount = writeVectorCount;
                a.cl = cl;
                a.wl = wl;
                a.mode = vif1_regs.mode & 3u;
                a.mask = vif1_regs.mask;
                a.maskEnable = maskEnable;
                a.zeroExtend = zeroExtend;
                a.row = vif1_regs.row;
                a.col = vif1_regs.col;
                fastDone = unpackTypedDispatch(vl, vn, a);
                if (unpackFast == 2)
                {
                    verifyFast.assign(m_vu1Data, m_vu1Data + PS2_VU1_DATA_SIZE);
                    std::memcpy(verifyRowFast, vif1_regs.row, sizeof(verifyRowFast));
                    std::memcpy(m_vu1Data, verifyBefore.data(), PS2_VU1_DATA_SIZE);
                    std::memcpy(vif1_regs.row, verifyRowBefore, sizeof(verifyRowBefore));
                    fastDone = false;
                }
            }
            else if (unpackFast == 1 && m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && !maskEnable && cl == wl &&
                (vl != 3u || vn == 3u))
            {
                if (unpackFast == 2)
                {
                    verifyBefore.assign(m_vu1Data, m_vu1Data + PS2_VU1_DATA_SIZE);
                    std::memcpy(verifyRowBefore, vif1_regs.row, sizeof(verifyRowBefore));
                }
                fastDone = true;
                // [gta-recomp] The common UNPACK - no mask, CL == WL, so vector i goes to VU address + i - with one
                // format switch per UNPACK and no divisions per vector. The same writes as the general loop below
                // (fields the format doesn't supply keep their old value, MODE 1 / 2 add / accumulate ROW).
                const uint8_t *src = data + pos;
                const uint32_t mode = vif1_regs.mode & 3u;
                const bool add = vl != 3u && (mode == 1u || mode == 2u);
                const uint32_t limit = components > 4 ? 4u : static_cast<uint32_t>(components);
                for (uint32_t i = 0; i < writeVectorCount; ++i)
                {
                    uint8_t *dst = m_vu1Data + ((vuAddr + i) & 0x3FFu) * 16u;
                    const uint8_t *v = src + i * bytesPerVector;
                    uint32_t lanes[4];
                    std::memcpy(lanes, dst, sizeof(lanes));
                    switch (vl)
                    {
                    case 0u:
                        if (components == 1)
                        {
                            uint32_t scalar;
                            std::memcpy(&scalar, v, 4u);
                            lanes[0] = lanes[1] = lanes[2] = lanes[3] = scalar;
                        }
                        else
                            std::memcpy(lanes, v, limit * 4u);
                        break;
                    case 1u:
                        for (uint32_t c = 0; c < limit; ++c)
                        {
                            uint16_t raw;
                            std::memcpy(&raw, v + (components == 1 ? 0u : c * 2u), 2u);
                            lanes[c] = zeroExtend ? raw : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                        }
                        if (components == 1)
                            lanes[1] = lanes[2] = lanes[3] = lanes[0];
                        break;
                    case 2u:
                        for (uint32_t c = 0; c < limit; ++c)
                            lanes[c] = zeroExtend ? v[c] : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v[c])));
                        if (components == 1)
                            lanes[1] = lanes[2] = lanes[3] = lanes[0];
                        break;
                    default:
                    {
                        uint16_t packed;
                        std::memcpy(&packed, v, 2u);
                        lanes[0] = (packed & 0x001Fu) << 3u;
                        lanes[1] = (packed & 0x03E0u) >> 2u;
                        lanes[2] = (packed & 0x7C00u) >> 7u;
                        lanes[3] = (packed & 0x8000u) >> 8u;
                        break;
                    }
                    }
                    if (add)
                        for (uint32_t field = 0u; field < 4u; ++field)
                        {
                            lanes[field] += vif1_regs.row[field];
                            if (mode == 2u)
                                vif1_regs.row[field] = lanes[field];
                        }
                    std::memcpy(dst, lanes, sizeof(lanes));
                }
                if (unpackFast == 2)
                {
                    verifyFast.assign(m_vu1Data, m_vu1Data + PS2_VU1_DATA_SIZE);
                    std::memcpy(verifyRowFast, vif1_regs.row, sizeof(verifyRowFast));
                    std::memcpy(m_vu1Data, verifyBefore.data(), PS2_VU1_DATA_SIZE);
                    std::memcpy(vif1_regs.row, verifyRowBefore, sizeof(verifyRowBefore));
                    fastDone = false;
                }
            }
            if (!fastDone && m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: RGBA5551 expanded to 8-bit positions, as the VIF does
                        // ([gta-recomp] upstream kept the raw 5/1-bit fields: colours 8x too
                        // dark and alpha 1 instead of 0x80, so alpha-tested world geometry vanished).
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = (packed & 0x001Fu) << 3u;
                        decompressed[1] = (packed & 0x03E0u) >> 2u;
                        decompressed[2] = (packed & 0x7C00u) >> 7u;
                        decompressed[3] = (packed & 0x8000u) >> 8u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            if (!verifyFast.empty())
            {
                static uint64_t checked = 0u, differing = 0u;
                ++checked;
                if (std::memcmp(verifyFast.data(), m_vu1Data, PS2_VU1_DATA_SIZE) != 0 ||
                    std::memcmp(verifyRowFast, vif1_regs.row, sizeof(verifyRowFast)) != 0)
                {
                    if (differing++ < 20u)
                        std::fprintf(stderr, "[vif1-unpack-verify] UNPACK 0x%02x num %u imm 0x%04x cl %u wl %u mode %u differs%c", opcode,
                                     num, imm, cl, wl, vif1_regs.mode & 3u, 10);
                }
                if ((checked & 0xFFFFu) == 0u)
                    std::fprintf(stderr, "[vif1-unpack-verify] %llu checked, %llu differ%c", static_cast<unsigned long long>(checked),
                                 static_cast<unsigned long long>(differing), 10);
            }
            if (ps2chain::VifRecorder *recorder = ps2chain::vifRecorder()) // [gta-recomp] runtime/ps2_chain_observer.h
            {
                // the fields a CL = WL = 1 write sets (mask row 0): the format's, then the mask's choice
                const uint32_t formatLanes = (components == 1 || (vl == 3u && vn == 3u)) ? 0xFu : ((1u << (components > 4 ? 4 : components)) - 1u);
                uint32_t lanes = formatLanes;
                bool fromCol = false;
                if (maskEnable)
                {
                    lanes = 0u;
                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        const uint32_t spec = (vif1_regs.mask >> (field * 2u)) & 0x3u;
                        if ((spec == 0u && (formatLanes >> field) & 1u) || spec == 1u || spec == 2u)
                            lanes |= 1u << field;
                        fromCol |= spec == 2u;
                    }
                }
                recorder->unpack(vuAddr, writeVectorCount, (imm & 0x8000u) != 0u, vif1_regs.tops & 0x3FFu, vif1_regs.cycle,
                                 maskEnable, vif1_regs.mode, lanes, fromCol, m_vu1Data);
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            // [gta-recomp] PS2X_TRACE_VIF=1: report VIFcodes this interpreter does not know
            static const bool trace = std::getenv("PS2X_TRACE_VIF") != nullptr;
            static int reported = 0;
            if (trace && reported++ < 50)
                std::fprintf(stderr, "[vif1] unknown VIFcode 0x%08x (cmd 0x%02x) at +0x%x of %u bytes\n", cmd, opcode, pos - 4u, sizeBytes);
            continue;
        }
    }
}
