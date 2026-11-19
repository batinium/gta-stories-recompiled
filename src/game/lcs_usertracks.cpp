// [lcs] User tracks: the player's own music files as one of Liberty City Stories' radio stations (VC's MP3 player, as
// a station the wheel already has).
//
// How LCS plays a station (game/lcs_sound.cpp, the host port of CDSTREAM.IRX's music server 0x65686578): the game opens
// music file <n> (command 0x80200, n = the module's name table at 0x5884: 0 HEAD, 1 DOUBLE, 2 KJAH, 3 RISE, 4 LIPS,
// 5 MUNDO, 6 MSX, 7 FLASH, 8 LCJ, 9 LCFR - \AUDIO\MUSIC\*.VB) at a position the game keeps per station; the music
// thread (0x22E4) reads the .VB 256 KB at a time into two IOP buffers and feeds two SPU2 voices (core 1 voices 22 / 23,
// 32 kHz, pitch 0xAAA) 8 KB at a time through 16 KB rings at SPU2 0x5010 (left) / 0x9010 (right), refilled on the
// ring's IRQ. A .VB is headerless PS2 ADPCM in 16 KB pairs: 8 KB of the left channel, then 8 KB of the right (the
// thread sends buffer offsets 0 / 0x4000 / ... to the left ring and 0x2000 / 0x6000 / ... to the right; markChunk only
// rewrites block flags).
//
// The seam: the disc read of the stream (lcs_sound.cpp readStream). While the chosen station's stream is open its bytes
// come from fill() - the player's files decoded (dr_mp3 / dr_wav / dr_flac / stb_vorbis), resampled to 32 kHz and
// encoded to ADPCM in that same layout - and everything else (sizes, positions, the game's station logic, the SPU2
// side, volume, mono) stays the game's. The station keeps its name and its place on the wheel; its DJ and adverts are
// in the same .VB and go with it. The music plays on where it stopped when the station is chosen again (a decoder thread
// keeps ~40 s ready, then waits).
//
// Files: <data>/music (data = the folder holding mc/ and mods/; VCS_DATA_DIR in tests), .mp3 .ogg .flac .wav, in a
// shuffled order (VCS_USER_TRACKS_SHUFFLE=0: by name), the list read again after each round. Setting Audio > User
// tracks (audio.user_tracks; env VCS_USER_TRACKS: 0 off, n + 1 = station n, 11 every station): which station they
// replace, default LCFR (the talk station). Nothing changes when the folder has no playable file. News bulletins
// (NEWS_*.VB, music files 25-38) and cutscene music are their own streams and stay the game's.
#include "game/lcs_soundmods.h"

#include "ps2_runtime.h"
#include "settings/settings.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace lcs::usertracks
{
    namespace
    {
        constexpr uint32_t kRate = 32000;                          // the music voices' rate (setupVoice 0x20C8: pitch 0xAAA)
        constexpr uint32_t kHalf = 0x2000;                         // one channel's 8 KB of a 16 KB pair
        constexpr uint32_t kFramesPerPair = kHalf / 16u * 28u;     // 14336 frames
        constexpr size_t kRingFrames = static_cast<size_t>(kRate) * 40u; // decoded ahead + kept behind
        constexpr size_t kReserve = static_cast<size_t>(kRate) * 16u;    // kept behind: more than the stream's 2 x 256 KB
        constexpr size_t kChunkFrames = 4096;                      // decoder steps
        constexpr int kStations = 10;                              // music files 0-9 are the stations
        constexpr int kEvery = kStations + 1;                      // the setting's "On every station"
        const char *const kStationName[kStations] = {"Head Radio", "Double Clef FM", "K-Jah Radio West", "Rise FM",
                                                      "Lips 106", "Radio Del Mundo", "MSX 98", "Flashback FM",
                                                      "The Liberty Jam", "LCFR"};

        fs::path musicDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path() / "music"; }

        std::vector<fs::path> scan()
        {
            std::vector<fs::path> list;
            std::error_code ec;
            const fs::path dir = musicDir();
            if (!fs::is_directory(dir, ec))
                return list;
            for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
                 it.increment(ec))
                if (it->is_regular_file(ec) && audiofile::supportedName(it->path()))
                    list.push_back(it->path());
            std::sort(list.begin(), list.end());
            const char *shuffle = std::getenv("VCS_USER_TRACKS_SHUFFLE");
            if (!(shuffle && *shuffle == '0'))
                std::shuffle(list.begin(), list.end(), std::mt19937(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
            return list;
        }

        bool readFile(const fs::path &p, std::vector<uint8_t> &out)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return false;
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size <= 0 || size > 0x7FFFFFF0)
                return false;
            out.resize(static_cast<size_t>(size));
            in.seekg(0);
            return static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), size));
        }

        class Player
        {
        public:
            // the decoder thread, once: false when the folder has nothing playable
            bool start()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_started)
                    return !m_dead;
                m_started = true;
                m_list = scan();
                if (m_list.empty())
                {
                    m_dead = true;
                    return false;
                }
                std::fprintf(stderr, "[usertracks] %zu file(s) in %s\n", m_list.size(), musicDir().string().c_str());
                m_ring.assign(kRingFrames * 2, 0);
                std::thread([this] { run(); }).detach();
                return true;
            }

            bool dead()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_dead;
            }

            // `frames` stereo frames out of the ring; waits for the decoder up to `waitMs`, then silence
            void take(int16_t *out, size_t frames, int waitMs)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
                size_t done = 0;
                while (done < frames)
                {
                    if (!m_count)
                    {
                        if (m_dead || !m_room.wait_until(lock, until, [&] { return m_count > 0 || m_dead; }) || !m_count)
                        {
                            ++m_underruns;
                            std::memset(out + done * 2, 0, (frames - done) * 4);
                            break;
                        }
                    }
                    const size_t n = std::min({frames - done, m_count, kRingFrames - m_head});
                    std::memcpy(out + done * 2, m_ring.data() + m_head * 2, n * 4);
                    m_head = (m_head + n) % kRingFrames;
                    m_count -= n;
                    m_recent = std::min(m_recent + n, kReserve);
                    done += n;
                }
                m_fed += frames;
                m_space.notify_one();
            }

            // `frames` taken but never heard (the stream stopped with them still in its buffers): back to the front,
            // so the music goes on where it was heard last (the decoder never writes over the last kReserve taken)
            void unread(size_t frames)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                frames = std::min(frames, m_recent);
                m_head = (m_head + kRingFrames - frames) % kRingFrames;
                m_count += frames;
                m_recent -= frames;
                m_fed -= std::min<uint64_t>(m_fed, frames);
            }

            uint64_t fedFrames()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_fed;
            }

        private:
            // the next track's decoder (the thread, without the lock); nullptr when a whole round failed
            std::unique_ptr<audiofile::Decoder> nextTrack()
            {
                size_t failures = 0;
                for (;;)
                {
                    fs::path p;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_next >= m_list.size())
                        {
                            std::vector<fs::path> again = scan();
                            if (!again.empty())
                                m_list = std::move(again);
                            m_next = 0;
                        }
                        if (failures >= m_list.size())
                            return nullptr;
                        p = m_list[m_next++];
                    }
                    std::vector<uint8_t> bytes;
                    std::string error;
                    std::unique_ptr<audiofile::Decoder> d;
                    if (readFile(p, bytes))
                        d = audiofile::open(std::move(bytes), p.filename().string(), error);
                    else
                        error = "cannot read";
                    if (d)
                    {
                        std::fprintf(stderr, "[usertracks] playing %s (%u Hz, %u channel(s))\n", p.filename().string().c_str(), d->rate(),
                                     d->channels());
                        return d;
                    }
                    std::fprintf(stderr, "[usertracks] skipped %s: %s\n", p.filename().string().c_str(), error.c_str());
                    ++failures;
                }
            }

            void run()
            {
                std::vector<int16_t> raw(kChunkFrames * 2), stereo(kChunkFrames * 2), out;
                std::unique_ptr<audiofile::Decoder> dec;
                audiofile::Resampler rs;
                for (;;)
                {
                    if (!dec)
                    {
                        dec = nextTrack();
                        rs = {};
                        if (!dec)
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            m_dead = true;
                            std::fprintf(stderr, "[usertracks] no file in %s could be played: the station plays as usual\n",
                                         musicDir().string().c_str());
                            m_room.notify_all();
                            return;
                        }
                    }
                    const size_t n = dec->read(raw.data(), kChunkFrames);
                    if (!n)
                    {
                        dec.reset();
                        continue;
                    }
                    const uint32_t ch = dec->channels();
                    for (size_t i = 0; i < n; ++i)
                    {
                        stereo[i * 2] = raw[i * ch];
                        stereo[i * 2 + 1] = raw[i * ch + (ch > 1 ? 1 : 0)];
                    }
                    out.clear();
                    rs.run(stereo.data(), n, static_cast<double>(dec->rate()) / kRate, out);
                    const size_t frames = out.size() / 2;
                    std::unique_lock<std::mutex> lock(m_mutex);
                    // (after an unread() the ring may hold more than kRingFrames - kReserve: wait until it doesn't)
                    m_space.wait(lock, [&] { return m_count + frames + kReserve <= kRingFrames; });
                    for (size_t i = 0; i < frames;)
                    {
                        const size_t tail = (m_head + m_count) % kRingFrames;
                        const size_t k = std::min(frames - i, kRingFrames - tail);
                        std::memcpy(m_ring.data() + tail * 2, out.data() + i * 2, k * 4);
                        m_count += k;
                        i += k;
                    }
                    m_room.notify_all();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_room, m_space; // frames there / room for more
            bool m_started = false, m_dead = false;
            std::vector<fs::path> m_list;
            size_t m_next = 0;
            std::vector<int16_t> m_ring;
            size_t m_head = 0, m_count = 0;
            size_t m_recent = 0; // frames taken lately that unread() may give back (at most kReserve)
            uint64_t m_fed = 0, m_underruns = 0;
        };

        Player &player()
        {
            static Player *p = new Player(); // lives until exit with its thread
            return *p;
        }

        // the encoder side (the EE thread, under the sound engine's lock)
        bool g_active = false;
        int g_file = -1;
        audiofile::AdpcmState g_state[2];
        std::vector<uint8_t> g_pair(2 * kHalf);
        size_t g_pairAt = 2 * kHalf; // bytes of g_pair already handed out
        uint64_t g_openFed = 0;

        void makePair()
        {
            std::vector<int16_t> frames(kFramesPerPair * 2);
            player().take(frames.data(), kFramesPerPair, 2000);
            int16_t block[2][28];
            for (uint32_t b = 0; b < kHalf / 16u; ++b)
            {
                for (int i = 0; i < 28; ++i)
                {
                    block[0][i] = frames[(b * 28u + i) * 2];
                    block[1][i] = frames[(b * 28u + i) * 2 + 1];
                }
                audiofile::encodeBlock(block[0], g_pair.data() + b * 16u, 0, g_state[0]);
                audiofile::encodeBlock(block[1], g_pair.data() + kHalf + b * 16u, 0, g_state[1]);
            }
            g_pairAt = 0;
        }
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "audio.user_tracks";
        s.page = "Audio";
        s.section = "User tracks";
        s.label = "User tracks";
        s.help = "Your own music as a radio station: put .mp3, .ogg, .flac or .wav files in the music folder next to the "
                 "game's mods folder (the station plays as usual while it is empty). Choose the station they replace; "
                 "its DJ and adverts go too. Applies at the next tune-in.";
        s.type = settings::Type::Choice;
        s.choices.push_back({"Off", 0});
        for (int i = 0; i < kStations; ++i)
            s.choices.push_back({std::string("Instead of ") + kStationName[i], i + 1});
        s.choices.push_back({"On every station", kEvery});
        s.defaultValue = 10; // LCFR, the talk station
        s.env = "VCS_USER_TRACKS";
        settings::add(s);
    }

    bool open(int file)
    {
        close(0);
        const int choice = settings::value("audio.user_tracks");
        if (file < 0 || file >= kStations || !(choice == kEvery || choice == file + 1))
            return false;
        if (!player().start())
            return false;
        g_active = true;
        g_file = file;
        g_state[0] = g_state[1] = {}; // the voices start afresh at key-on
        g_pairAt = 2 * kHalf;
        g_openFed = player().fedFrames();
        std::fprintf(stderr, "[usertracks] station %d (%s) tuned: playing the music folder\n", file, kStationName[file]);
        return true;
    }

    void close(uint32_t unsentBytes)
    {
        if (!g_active)
            return;
        g_active = false;
        // what the stream's buffers and the half-handed-out pair still held: not heard, so given back
        const size_t pairs = (unsentBytes + static_cast<uint32_t>(g_pair.size() - g_pairAt)) / (2u * kHalf);
        player().unread(pairs * kFramesPerPair);
        g_pairAt = g_pair.size();
        std::fprintf(stderr, "[usertracks] station %d closed after %.1f s heard\n", g_file,
                     static_cast<double>(player().fedFrames() - g_openFed) / kRate);
    }

    void fill(uint8_t *dest, uint32_t bytes)
    {
        while (bytes)
        {
            if (g_pairAt >= g_pair.size())
                makePair();
            const uint32_t n = std::min<uint32_t>(bytes, static_cast<uint32_t>(g_pair.size() - g_pairAt));
            std::memcpy(dest, g_pair.data() + g_pairAt, n);
            g_pairAt += n;
            dest += n;
            bytes -= n;
        }
    }
}
