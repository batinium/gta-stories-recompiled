// [lcs] Audio files for the sound mods (game/lcs_soundmods.h): WAV / MP3 / OGG / FLAC decoding through the vendored
// single-file decoders in third_party/audio_decoders (dr_wav, dr_mp3, dr_flac: public domain or MIT-0, github.com/
// mackron/dr_libs; stb_vorbis: public domain or MIT, github.com/nothings/stb), linear resampling and PS2 ADPCM encoding.
// Game-independent (it builds in VCS builds too and is only used by LCS's sound mods).
#include "game/lcs_soundmods.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "../../third_party/audio_decoders/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../third_party/audio_decoders/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "../../third_party/audio_decoders/dr_flac.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/audio_decoders/stb_vorbis.c"
#undef L
#undef C
#undef R
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lcs::audiofile
{
    namespace
    {
        struct Wav final : Decoder
        {
            std::vector<uint8_t> bytes;
            drwav wav{};
            bool ok = false;
            ~Wav() override
            {
                if (ok)
                    drwav_uninit(&wav);
            }
            uint32_t channels() const override { return std::min<uint32_t>(wav.channels, 2); }
            uint32_t rate() const override { return wav.sampleRate; }
            size_t read(int16_t *out, size_t frames) override { return readCut(out, frames); }
            size_t readCut(int16_t *out, size_t frames)
            {
                if (wav.channels <= 2)
                    return static_cast<size_t>(drwav_read_pcm_frames_s16(&wav, frames, out));
                std::vector<int16_t> all(frames * wav.channels);
                const size_t n = static_cast<size_t>(drwav_read_pcm_frames_s16(&wav, frames, all.data()));
                for (size_t i = 0; i < n; ++i)
                    out[i * 2] = all[i * wav.channels], out[i * 2 + 1] = all[i * wav.channels + 1];
                return n;
            }
        };

        struct Mp3 final : Decoder
        {
            std::vector<uint8_t> bytes;
            drmp3 mp3{};
            bool ok = false;
            ~Mp3() override
            {
                if (ok)
                    drmp3_uninit(&mp3);
            }
            uint32_t channels() const override { return std::min<uint32_t>(mp3.channels, 2); }
            uint32_t rate() const override { return mp3.sampleRate; }
            size_t read(int16_t *out, size_t frames) override
            {
                if (mp3.channels <= 2)
                    return static_cast<size_t>(drmp3_read_pcm_frames_s16(&mp3, frames, out));
                std::vector<int16_t> all(frames * mp3.channels);
                const size_t n = static_cast<size_t>(drmp3_read_pcm_frames_s16(&mp3, frames, all.data()));
                for (size_t i = 0; i < n; ++i)
                    out[i * 2] = all[i * mp3.channels], out[i * 2 + 1] = all[i * mp3.channels + 1];
                return n;
            }
        };

        struct Flac final : Decoder
        {
            std::vector<uint8_t> bytes;
            drflac *flac = nullptr;
            ~Flac() override
            {
                if (flac)
                    drflac_close(flac);
            }
            uint32_t channels() const override { return std::min<uint32_t>(flac->channels, 2); }
            uint32_t rate() const override { return flac->sampleRate; }
            size_t read(int16_t *out, size_t frames) override
            {
                if (flac->channels <= 2)
                    return static_cast<size_t>(drflac_read_pcm_frames_s16(flac, frames, out));
                std::vector<int16_t> all(frames * flac->channels);
                const size_t n = static_cast<size_t>(drflac_read_pcm_frames_s16(flac, frames, all.data()));
                for (size_t i = 0; i < n; ++i)
                    out[i * 2] = all[i * flac->channels], out[i * 2 + 1] = all[i * flac->channels + 1];
                return n;
            }
        };

        struct Ogg final : Decoder
        {
            std::vector<uint8_t> bytes;
            stb_vorbis *v = nullptr;
            stb_vorbis_info info{};
            ~Ogg() override
            {
                if (v)
                    stb_vorbis_close(v);
            }
            uint32_t channels() const override { return std::min<uint32_t>(static_cast<uint32_t>(info.channels), 2); }
            uint32_t rate() const override { return info.sample_rate; }
            size_t read(int16_t *out, size_t frames) override
            {
                const int ch = static_cast<int>(channels());
                return static_cast<size_t>(stb_vorbis_get_samples_short_interleaved(v, ch, out, static_cast<int>(frames) * ch));
            }
        };

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::unique_ptr<Decoder> tryWav(std::vector<uint8_t> &bytes)
        {
            auto d = std::make_unique<Wav>();
            d->bytes = std::move(bytes);
            if (drwav_init_memory(&d->wav, d->bytes.data(), d->bytes.size(), nullptr) && d->wav.channels > 0 && d->wav.sampleRate > 0)
            {
                d->ok = true;
                return d;
            }
            bytes = std::move(d->bytes);
            return nullptr;
        }
        std::unique_ptr<Decoder> tryMp3(std::vector<uint8_t> &bytes)
        {
            auto d = std::make_unique<Mp3>();
            d->bytes = std::move(bytes);
            if (drmp3_init_memory(&d->mp3, d->bytes.data(), d->bytes.size(), nullptr))
            {
                d->ok = true;
                if (d->mp3.channels > 0 && d->mp3.sampleRate > 0)
                    return d;
                drmp3_uninit(&d->mp3);
                d->ok = false;
            }
            bytes = std::move(d->bytes);
            return nullptr;
        }
        std::unique_ptr<Decoder> tryFlac(std::vector<uint8_t> &bytes)
        {
            auto d = std::make_unique<Flac>();
            d->bytes = std::move(bytes);
            d->flac = drflac_open_memory(d->bytes.data(), d->bytes.size(), nullptr);
            if (d->flac && d->flac->channels > 0 && d->flac->sampleRate > 0)
                return d;
            bytes = std::move(d->bytes);
            return nullptr;
        }
        std::unique_ptr<Decoder> tryOgg(std::vector<uint8_t> &bytes)
        {
            auto d = std::make_unique<Ogg>();
            d->bytes = std::move(bytes);
            int err = 0;
            d->v = stb_vorbis_open_memory(d->bytes.data(), static_cast<int>(d->bytes.size()), &err, nullptr);
            if (d->v)
            {
                d->info = stb_vorbis_get_info(d->v);
                if (d->info.channels > 0 && d->info.sample_rate > 0)
                    return d;
            }
            bytes = std::move(d->bytes);
            return nullptr;
        }
    }

    bool supportedName(const std::filesystem::path &p)
    {
        const std::string e = lower(p.extension().string());
        return e == ".wav" || e == ".mp3" || e == ".ogg" || e == ".flac";
    }

    std::unique_ptr<Decoder> open(std::vector<uint8_t> bytes, const std::string &nameForOrder, std::string &error)
    {
        if (bytes.size() < 16 || bytes.size() > 0x7FFFFFF0u)
        {
            error = "empty or too large";
            return nullptr;
        }
        using Try = std::unique_ptr<Decoder> (*)(std::vector<uint8_t> &);
        // by content: RIFF/WAVE, fLaC, OggS first; MP3 (no reliable magic: ID3 or a frame sync anywhere) last
        std::vector<Try> order;
        const bool riff = !std::memcmp(bytes.data(), "RIFF", 4) || !std::memcmp(bytes.data(), "RF64", 4);
        const bool flac = !std::memcmp(bytes.data(), "fLaC", 4);
        const bool ogg = !std::memcmp(bytes.data(), "OggS", 4);
        if (riff)
            order = {tryWav};
        else if (flac)
            order = {tryFlac};
        else if (ogg)
            order = {tryOgg};
        else
            order = {tryMp3};
        (void)nameForOrder;
        for (Try t : order)
            if (auto d = t(bytes))
                return d;
        error = riff ? "not a WAV the decoder reads" : flac ? "damaged FLAC" : ogg ? "not an Ogg Vorbis file (Opus?)" : "not WAV / MP3 / OGG / FLAC";
        return nullptr;
    }

    void Resampler::run(const int16_t *in, size_t frames, double step, std::vector<int16_t> &out)
    {
        if (!frames)
            return;
        if (!primed)
        {
            prev[0] = in[0];
            prev[1] = in[1];
            pos = 0.0;
            primed = true;
        }
        // input frame k: k = 0 is `prev`, k >= 1 is in[k - 1]
        auto at = [&](size_t k, int c) { return k == 0 ? prev[c] : in[(k - 1) * 2 + c]; };
        while (pos < static_cast<double>(frames))
        {
            const size_t k = static_cast<size_t>(pos);
            const double f = pos - static_cast<double>(k);
            for (int c = 0; c < 2; ++c)
            {
                const double v = at(k, c) + (at(k + 1, c) - at(k, c)) * f;
                out.push_back(static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L)));
            }
            pos += step;
        }
        pos -= static_cast<double>(frames);
        prev[0] = in[(frames - 1) * 2];
        prev[1] = in[(frames - 1) * 2 + 1];
    }

    std::vector<int16_t> decodeMono(Decoder &d, uint32_t rate)
    {
        const uint32_t ch = d.channels();
        std::vector<int16_t> buf(4096 * 2), stereo, out;
        Resampler rs;
        const double step = static_cast<double>(d.rate()) / static_cast<double>(rate);
        for (;;)
        {
            const size_t n = d.read(buf.data(), 4096);
            if (!n)
                break;
            stereo.resize(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                stereo[i * 2] = buf[i * ch];
                stereo[i * 2 + 1] = buf[i * ch + (ch > 1 ? 1 : 0)];
            }
            std::vector<int16_t> part;
            rs.run(stereo.data(), n, step, part);
            for (size_t i = 0; i + 1 < part.size(); i += 2)
                out.push_back(static_cast<int16_t>((part[i] + part[i + 1]) / 2));
            if (out.size() > 48000u * 600u) // ten minutes is not an effect
                break;
        }
        return out;
    }

    void encodeBlock(const int16_t *x, uint8_t *block, uint8_t flags, AdpcmState &st)
    {
        // the SPU2's prediction filters (iop_spu2.cpp kFilterPos / kFilterNeg): s = (n << 12 >> shift) +
        // ((h1 * pos + h2 * neg + 32) >> 6), clamped to 16 bits
        static constexpr int32_t kPos[5] = {0, 60, 115, 98, 122};
        static constexpr int32_t kNeg[5] = {0, 0, -52, -55, -60};
        int64_t bestErr = INT64_MAX;
        int bestFilter = 0, bestShift = 12;
        int8_t bestN[28] = {};
        int32_t bestH1 = st.hist1, bestH2 = st.hist2;
        for (int f = 0; f < 5; ++f)
        {
            // the largest residual with the input as history picks the shift; that one and the next coarser are tried
            int32_t h1 = st.hist1, h2 = st.hist2, maxR = 0;
            for (int i = 0; i < 28; ++i)
            {
                const int32_t r = x[i] - ((h1 * kPos[f] + h2 * kNeg[f] + 32) >> 6);
                maxR = std::max(maxR, std::abs(r));
                h2 = h1;
                h1 = x[i];
            }
            int k = 0; // step 2^k
            while (k < 12 && maxR > 7 * (1 << k))
                ++k;
            for (int kk = k; kk <= std::min(k + 1, 12); ++kk)
            {
                const int shift = 12 - kk;
                const int32_t step = 1 << kk;
                int32_t a1 = st.hist1, a2 = st.hist2;
                int64_t err = 0;
                int8_t n[28];
                for (int i = 0; i < 28; ++i)
                {
                    const int32_t pred = (a1 * kPos[f] + a2 * kNeg[f] + 32) >> 6;
                    const int32_t r = x[i] - pred;
                    int32_t q = static_cast<int32_t>(std::floor((static_cast<double>(r) + step / 2.0) / step));
                    q = std::clamp(q, -8, 7);
                    const int32_t d = std::clamp(q * step + pred, -32768, 32767);
                    err += static_cast<int64_t>(x[i] - d) * (x[i] - d);
                    n[i] = static_cast<int8_t>(q);
                    a2 = a1;
                    a1 = d;
                }
                if (err < bestErr)
                {
                    bestErr = err;
                    bestFilter = f;
                    bestShift = shift;
                    std::memcpy(bestN, n, sizeof n);
                    bestH1 = a1;
                    bestH2 = a2;
                }
            }
        }
        std::memset(block, 0, 16);
        block[0] = static_cast<uint8_t>(bestShift | bestFilter << 4);
        block[1] = flags;
        for (int i = 0; i < 28; ++i)
            block[2 + i / 2] |= static_cast<uint8_t>((bestN[i] & 0xF) << ((i & 1) * 4));
        st.hist1 = bestH1;
        st.hist2 = bestH2;
    }
}
