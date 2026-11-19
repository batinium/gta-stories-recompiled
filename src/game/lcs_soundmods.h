// [lcs] Sound mods for Liberty City Stories' native sound driver (game/lcs_sound.cpp): the player's own music as a radio
// station (game/lcs_usertracks.cpp) and effect sounds replaced from audio files (game/lcs_sfxmod.cpp, the active.txt
// "sound" directive). Shared pieces (game/lcs_audiofile.cpp): decoding WAV / MP3 / OGG / FLAC (third_party/
// audio_decoders: dr_wav, dr_mp3, dr_flac - public domain or MIT-0 - and stb_vorbis - public domain or MIT), resampling
// and PS2 ADPCM (VAG) encoding. docs/LCS.md "Mods" / "Sound".
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lcs
{
    namespace audiofile
    {
        // A file's sound as interleaved 16-bit frames (1 or 2 channels; more are cut to the first two), read in steps
        struct Decoder
        {
            virtual ~Decoder() = default;
            virtual uint32_t channels() const = 0;
            virtual uint32_t rate() const = 0;
            // up to `frames` frames into `out` (frames x channels samples); 0 at the end or on an error
            virtual size_t read(int16_t *out, size_t frames) = 0;
        };
        // .wav / .mp3 / .ogg / .flac by their content (the extension only decides the order of the tries); nullptr and
        // `error` when it is none of them. Takes the bytes (kept for the decoder's life).
        std::unique_ptr<Decoder> open(std::vector<uint8_t> bytes, const std::string &nameForOrder, std::string &error);
        bool supportedName(const std::filesystem::path &p); // by extension: .wav .mp3 .ogg .flac (any case)

        // The whole sound, mono, at `rate` Hz (linear interpolation; channels averaged)
        std::vector<int16_t> decodeMono(Decoder &d, uint32_t rate);

        // A streaming linear resampler, stereo: `in` (frames x 2) -> appended to `out` at `step` input frames per
        // output frame; the state carries the last input frame and the position between calls
        struct Resampler
        {
            double pos = 0.0; // position of the next output frame, in input frames from `prev` (index 0)
            int16_t prev[2] = {0, 0};
            bool primed = false;
            void run(const int16_t *in, size_t frames, double step, std::vector<int16_t> &out);
        };

        // PS2 SPU2 ADPCM: 28 samples -> one 16-byte block (header shift | filter << 4, `flags`), the encoder's history
        // carried from block to block as the SPU2's decoder carries it (iop_spu2.cpp's formula, so the history matches)
        struct AdpcmState
        {
            int32_t hist1 = 0, hist2 = 0;
        };
        void encodeBlock(const int16_t *samples28, uint8_t *block16, uint8_t flags, AdpcmState &state);
    }

    namespace usertracks
    {
        // The Audio setting "User tracks" (audio.user_tracks): before settings::load in an LCS run (main.cpp)
        void registerSettings();
        // Music driver seam (lcs_sound.cpp): stream `file` (CDSTREAM's music file index) opens; true when it is the
        // user-tracks station and <data>/music has playable files - the stream's data then comes from fill()
        bool open(int file);
        // the stream stopped; `unsentBytes` of what fill() gave were still in its buffers, never sent to the SPU2 (they
        // play first at the next tune-in)
        void close(uint32_t unsentBytes);
        // `bytes` of the stream's layout (16 KB pairs: 8 KB left ADPCM, 8 KB right, 32 kHz) into `dest`
        void fill(uint8_t *dest, uint32_t bytes);
    }

    namespace sfxmod
    {
        // After the sound table (offset in the bank's .RAW, bytes, rate per sound; ids global) is read:
        // the active.txt "sound" directives resolved and encoded. rawLsn[bank] / rawSize[bank]: the banks' .RAW files.
        void prepare(std::vector<uint32_t> &table, const uint32_t *bankFirstSound, uint32_t banks, uint32_t sounds,
                     const uint32_t *rawLsn, const uint32_t *rawSize);
        // A chunk of bank `bank`'s .RAW (from byte `offset`, `size` bytes) on its way into SPU2 RAM: replaced sounds
        // written over it
        void patchChunk(uint32_t bank, uint32_t offset, uint8_t *data, uint32_t size);
        // VCS_SFX_LOG=1: a voice keyed on with sound `id` of bank `bank` (to find a sound's id by playing it)
        bool logging();
        void logKeyOn(int32_t id, int32_t bank, int32_t rate);
    }
}
