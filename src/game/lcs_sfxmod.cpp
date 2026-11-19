// [lcs] Effect sounds replaced from audio files: the active.txt "sound" directive (docs/LCS.md "Mods").
//
// How LCS's effects are stored (game/lcs_sound.cpp, CDSTREAM.IRX's effects server 0x65686579): 67 banks, each a
// \AUDIO\SETn\SFXm_PS.RAW (the sounds back to back) and .SDT (12 bytes a sound: offset in the .RAW, bytes, rate in Hz).
// At sound init (0x43C0) every .SDT goes into one sound table of 0x161D sounds whose ids are global (bank m's first id is
// the module's table 0x3704 / kBankFirstSound: bank 0 = ids 0-0x138, the general effects - weapons, horns, engines,
// collisions; bank 2 = the speech; 3-0x27 the per-vehicle engine banks that share one SPU2 area; ...). A bank is copied
// whole into SPU2 RAM at its address (module data 0x6B48: bank 0 at 0x78810) by the loader 0x4BD8, 0x20800 bytes at a
// time; a voice plays sound id at bank address + offset + 0x30. Each sound in a .RAW is a VAG: a 48-byte header (its
// name at +0x20, e.g. "SFX_COLT45_LEFT.") then PS2 ADPCM blocks; its byte count includes the header (offset + bytes =
// the next sound's offset). Looping sounds (engines, sirens) have block flags 6 ... 2 ... 3, one-shots end in flag 1.
//
// The override: the chunk of the .RAW on its way to SPU2 RAM gets the new sound's ADPCM written over the slot (every
// time the bank is loaded), so addresses, the table and the game's indexing stay as they are. The file is decoded,
// mixed to mono and resampled to the slot's own rate (the game may play a sound at a rate of its own, so the slot's
// rate is kept), encoded with the slot's kind of ending (a loop when the original loops). It must fit the slot: longer
// is cut (logged); shorter ends early (the table's byte count is lowered to match, so the length the game asks for -
// 0xA0B00 - is the new one). Speech (bank 2: loaded sound by sound into slots) is not supported.
//
//   sound id=<global id> file=<path>                  one sound
//   sound bank=<bank> index=<i> file=<path>           the same by bank and index in the bank
//   sound name=<VAG name prefix> file=<path>          every sound whose VAG header name starts so (case ignored),
//                                                     e.g. name=SFX_COLT45 = the pistol's left and right shots
// optional: gain=<percent> (default 100).
//
// tools/mods.py sounds <iso> lists every id with its bank, rate, length, loop and VAG name; VCS_SFX_LOG=1 logs each
// voice keyed on (id, bank, rate), to find a sound by playing it.
#include "game/lcs_soundmods.h"

#include "game/vcs_modfiles.h"
#include "ps2_runtime.h"
#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace lcs::sfxmod
{
    namespace
    {
        constexpr uint32_t kSpeechBank = 2; // loaded sound by sound into slots (lcs_sound.cpp requestSpecial)

        struct Directive
        {
            std::string mod;
            std::map<std::string, std::string> keys;
        };

        struct Override
        {
            uint32_t bank = 0, id = 0;
            uint32_t at = 0;            // byte offset in the bank's .RAW (past the VAG header)
            std::vector<uint8_t> adpcm; // the blocks written there
            uint32_t loads = 0;
        };

        std::vector<uint32_t> g_first;  // first id of each bank
        std::vector<Override> g_overrides;

        fs::path modsDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path() / "mods"; }

        // active.txt's "sound" lines (vcs_mods.cpp reads the same file for the rest; a mod whose "needs" original is
        // missing is skipped as there; VCS_MODS=0 turns them off too)
        std::vector<Directive> readDirectives()
        {
            std::vector<Directive> out;
            const char *env = std::getenv("VCS_MODS");
            if (env && *env == '0')
                return out;
            std::ifstream in(modsDir() / "active.txt");
            std::string line, mod = "?";
            std::vector<std::string> skipped;
            std::vector<Directive> all;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::istringstream s(line);
                std::string kind;
                if (!(s >> kind) || kind[0] == '#')
                    continue;
                if (kind == "mod")
                {
                    s >> mod;
                    continue;
                }
                Directive d;
                d.mod = mod;
                std::string tok;
                while (s >> tok)
                    if (const size_t eq = tok.find('='); eq != std::string::npos)
                        d.keys[tok.substr(0, eq)] = tok.substr(eq + 1);
                if (kind == "needs")
                {
                    const vcs::mods::files::Location l = vcs::mods::files::resolve(d.keys.count("file") ? d.keys["file"] : "", modsDir());
                    if (!vcs::mods::files::available(l))
                        skipped.push_back(mod);
                }
                else if (kind == "sound")
                    all.push_back(std::move(d));
            }
            for (Directive &d : all)
                if (std::find(skipped.begin(), skipped.end(), d.mod) == skipped.end())
                    out.push_back(std::move(d));
            return out;
        }

        uint32_t bankOf(uint32_t id)
        {
            uint32_t bank = 0;
            for (uint32_t b = 0; b < g_first.size(); ++b)
                if (g_first[b] <= id)
                    bank = b;
            return bank;
        }

        bool readDisc(uint64_t offset, size_t bytes, void *dest)
        {
            IPS2DiscImage *disc = ps2_disc::image();
            return disc && disc->readBytes(offset, bytes, dest);
        }

        std::string upper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        bool parseU32(const std::string &s, uint32_t &v)
        {
            char *end = nullptr;
            const unsigned long x = std::strtoul(s.c_str(), &end, 0);
            if (s.empty() || !end || *end)
                return false;
            v = static_cast<uint32_t>(x);
            return true;
        }
    }

    bool logging()
    {
        static const bool on = [] {
            const char *e = std::getenv("VCS_SFX_LOG");
            return e && *e == '1';
        }();
        return on;
    }

    void logKeyOn(int32_t id, int32_t bank, int32_t rate)
    {
        const int32_t index = bank >= 0 && static_cast<size_t>(bank) < g_first.size() ? id - static_cast<int32_t>(g_first[bank]) : -1;
        std::fprintf(stderr, "[sfx] key on: sound %d (bank %d, index %d), rate %d\n", id, bank, index, rate);
    }

    void prepare(std::vector<uint32_t> &table, const uint32_t *bankFirstSound, uint32_t banks, uint32_t sounds, const uint32_t *rawLsn,
                 const uint32_t *rawSize)
    {
        g_first.assign(bankFirstSound, bankFirstSound + banks);
        g_overrides.clear();
        const std::vector<Directive> directives = readDirectives();
        if (directives.empty())
            return;
        auto entry = [&](uint32_t id) { return table.data() + static_cast<size_t>(id) * 3u; };
        // VAG names, read once if a directive asks by name
        std::vector<std::string> names;
        auto nameOf = [&](uint32_t id) -> const std::string & {
            if (names.empty())
            {
                names.assign(sounds, std::string());
                for (uint32_t i = 0; i < sounds; ++i)
                {
                    const uint32_t bank = bankOf(i);
                    if (bank == kSpeechBank)
                        continue;
                    char header[48] = {};
                    if (readDisc(static_cast<uint64_t>(rawLsn[bank]) * 2048u + entry(i)[0], sizeof header, header) && !std::memcmp(header, "VAGp", 4))
                        names[i] = upper(std::string(header + 0x20, strnlen(header + 0x20, 16)));
                }
            }
            return names[id];
        };
        for (const Directive &d : directives)
        {
            const auto key = [&](const char *k) { return d.keys.count(k) ? d.keys.at(k) : std::string(); };
            std::vector<uint32_t> ids;
            uint32_t v = 0, w = 0;
            if (parseU32(key("id"), v))
                ids.push_back(v);
            else if (parseU32(key("bank"), v) && parseU32(key("index"), w) && v < banks)
                ids.push_back(g_first[v] + w);
            else if (!key("name").empty())
            {
                const std::string want = upper(key("name"));
                for (uint32_t i = 0; i < sounds; ++i)
                    if (!nameOf(i).empty() && nameOf(i).compare(0, want.size(), want) == 0)
                        ids.push_back(i);
                if (ids.empty())
                    std::fprintf(stderr, "[sfxmod] %s: no sound named %s*\n", d.mod.c_str(), want.c_str());
            }
            else
            {
                std::fprintf(stderr, "[sfxmod] %s: sound directive without id= / bank= index= / name=: skipped\n", d.mod.c_str());
                continue;
            }
            const vcs::mods::files::Location l = vcs::mods::files::resolve(key("file"), modsDir());
            std::vector<uint8_t> bytes;
            std::string error;
            if (!l.error.empty() || !vcs::mods::files::read(l, bytes, 256u << 20, error))
            {
                std::fprintf(stderr, "[sfxmod] %s: %s: %s\n", d.mod.c_str(), l.shown.c_str(), l.error.empty() ? error.c_str() : l.error.c_str());
                continue;
            }
            const double gain = key("gain").empty() ? 1.0 : std::atof(key("gain").c_str()) / 100.0;
            for (const uint32_t id : ids)
            {
                const uint32_t bank = bankOf(id);
                if (id >= sounds || bank == kSpeechBank)
                {
                    std::fprintf(stderr, "[sfxmod] %s: sound %u: %s\n", d.mod.c_str(), id, id >= sounds ? "no such id" : "speech (bank 2) is not supported");
                    continue;
                }
                uint32_t *e = entry(id);
                const uint32_t offset = e[0], size = e[1], rate = e[2];
                if (size < 48u + 32u || offset + size > rawSize[bank] || rate == 0)
                {
                    std::fprintf(stderr, "[sfxmod] %s: sound %u: slot too small (%u bytes)\n", d.mod.c_str(), id, size);
                    continue;
                }
                // the original's ending: a loop (last block flag 3) or a one-shot
                uint8_t last[16] = {};
                const bool loop = readDisc(static_cast<uint64_t>(rawLsn[bank]) * 2048u + offset + size - 16u, 16, last) && last[1] == 3;
                std::unique_ptr<audiofile::Decoder> dec = audiofile::open(bytes, l.shown, error);
                if (!dec)
                {
                    std::fprintf(stderr, "[sfxmod] %s: %s: %s\n", d.mod.c_str(), l.shown.c_str(), error.c_str());
                    break;
                }
                std::vector<int16_t> pcm = audiofile::decodeMono(*dec, rate);
                for (int16_t &s : pcm)
                    s = static_cast<int16_t>(std::clamp(static_cast<long>(s * gain), -32768L, 32767L));
                const uint32_t capacity = (size - 48u) / 16u; // blocks, the leading silent block included
                uint32_t blocks = static_cast<uint32_t>((pcm.size() + 27u) / 28u);
                if (blocks == 0)
                    blocks = 1;
                const bool cut = blocks + 1u > capacity;
                if (cut)
                    blocks = capacity - 1u;
                pcm.resize(static_cast<size_t>(blocks) * 28u, 0);
                Override o;
                o.bank = bank;
                o.id = id;
                o.at = offset + 48u;
                o.adpcm.assign(static_cast<size_t>(blocks + 1u) * 16u, 0); // block 0: silence, flag 0 (as the game's)
                audiofile::AdpcmState st;
                for (uint32_t b = 0; b < blocks; ++b)
                {
                    uint8_t flags = 0;
                    if (loop)
                        flags = b == 0 ? 6 : (b + 1 == blocks ? 3 : 2);
                    else if (b + 1 == blocks)
                        flags = 1;
                    if (loop && blocks == 1)
                        flags = 7; // loop start + repeat + end in one block
                    audiofile::encodeBlock(pcm.data() + static_cast<size_t>(b) * 28u, o.adpcm.data() + (b + 1u) * 16u, flags, st);
                }
                const uint32_t newSize = 48u + static_cast<uint32_t>(o.adpcm.size());
                if (newSize < size)
                    e[1] = newSize;
                std::fprintf(stderr, "[sfxmod] %s: sound %u (bank %u, index %u%s%s) <- %s: %u Hz, %.2f s%s%s\n", d.mod.c_str(), id, bank,
                             id - g_first[bank], names.empty() || names[id].empty() ? "" : ", ", names.empty() ? "" : names[id].c_str(),
                             l.shown.c_str(), rate, blocks * 28.0 / rate, loop ? ", looped" : "",
                             cut ? ", cut to the slot's length" : "");
                g_overrides.push_back(std::move(o));
            }
        }
    }

    void patchChunk(uint32_t bank, uint32_t offset, uint8_t *data, uint32_t size)
    {
        for (Override &o : g_overrides)
        {
            if (o.bank != bank)
                continue;
            const uint32_t from = std::max(o.at, offset), to = std::min<uint32_t>(o.at + static_cast<uint32_t>(o.adpcm.size()), offset + size);
            if (from >= to)
                continue;
            std::memcpy(data + (from - offset), o.adpcm.data() + (from - o.at), to - from);
            if (to == o.at + o.adpcm.size() && o.loads++ == 0)
                std::fprintf(stderr, "[sfxmod] sound %u written into bank %u's SPU2 copy\n", o.id, bank);
        }
    }
}
