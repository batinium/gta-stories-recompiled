// [crossover] LCS's real vehicles in VCS on new model ids: see vcs_crossover.h. Addresses (2026-11-19), each with its
// evidence below.
#include "game/vcs_crossover.h"

#include "game/game_table.h"
#include "game/vcs_mods.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace vcs::crossover
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr const char *kMod = "stories_lcs_vehicles";

        // VCS's vehicle model info is 0x2A0 bytes; its handling record (the model info's +0x38 points at it, and every
        // vehicle constructor copies the pointer: vehicle +0x160 = mi +0x38, CREATE_CAR 0x3D03B0's constructors) 0xE0
        constexpr uint32_t kModelInfo = 0x2A0u, kHandling = 0xE0u;
        // new ids: VCS's vehicles are 170-280; 281-309 are null in the model info array with empty streaming entries
        constexpr int kNewFirst = 281, kNewCount = 17;
        // guest scratch 0x72000-0x76000 (as LCS's: the EE kernel area the recompiled game leaves alone): 17 model infos,
        // 17 handling records, 17 names
        constexpr uint32_t kScratchLo = 0x00072000u, kScratchHi = 0x00076000u;
        constexpr uint32_t kMiArea = kScratchLo, kHandArea = kMiArea + kNewCount * kModelInfo,
                           kNameArea = kHandArea + kNewCount * kHandling, kNameBytes = 0x20u;
        static_assert(kNameArea + kNewCount * kNameBytes <= kScratchHi, "crossover scratch overflows");

        constexpr uint32_t kModelInfoPtrs = 0x00486518u;  // CModelInfo::ms_modelInfoPtrs (`*(id * 4 + DAT_00486518)`)
        constexpr uint32_t kModelInfoCount = 0x004CD10Cu; // its count (7941)
        // CStreaming (*0x489FF8; GAME.DTZ header +0x7C): +0x18 vehicle models loaded, +0x1C their limit (12), +0x20 the
        // ring of 20, +200 the entries (0x18 each: +0xC state, +0xD flags, +0x10 sector, +0x14 count)
        constexpr uint32_t kStreaming = 0x00489FF8u;
        // CStreaming::RemoveModel(this, index) 0x3C8618: drops a vehicle model from the ring, deletes its objects; a
        // texture list (index < this +0xC) through RemoveTxd 0x3A5A30
        constexpr uint32_t kRemoveModel = 0x003C8618u;
        // CTxdStore's pool (*0x489ECC: {entries 0x1C, flags (0x80 free), size 2000}; RemoveTxd 0x3A5A30 reads it)
        constexpr uint32_t kTxdPool = 0x00489ECCu;
        constexpr uint32_t kTxdEntry = 0x1Cu;
        constexpr uint32_t kTextGet = 0x003FB1A8u;     // CText::Get(TheText *0x48A2A0, key) -> UTF-16 (HUD name 0x23B128)
        constexpr uint32_t kVehiclePool = 0x00487A98u; // -> CPool {entries, flags, size 40}, 0x8C0 a vehicle (0x478928)
        constexpr uint32_t kVehicleSize = 0x8C0u;
        constexpr uint32_t kPlayerPed = 0x004E4910u;   // CWorld::Players[0].m_pPed; its vehicle +0x480
        constexpr uint32_t kEntityModel = 0x56u;       // CEntity model index (int16; LCS +0x58)
        // the car colour palette: *(*0x48F04C + 0xC) + (index + 0x80) x 3, RGB, for indices below 0xF0 (0x395E08)
        constexpr uint32_t kPaletteHolder = 0x0048F04Cu;

        // VCS class (model info +0x54, int32: 1 / 2 boat, 4 heli, 5 plane, 6 bike, 8 BMX, 9 quad, else car) and each
        // class's donor when the pack's is of another: ADMIRAL, PCJ-600, SQUALO, MAVERICK, SKIMMER
        bool classFits(int c, const std::string &cls)
        {
            if (cls == "boat")
                return c == 1 || c == 2;
            if (cls == "heli")
                return c == 4;
            if (cls == "plane")
                return c == 5;
            if (cls == "bike")
                return c == 6 || c == 8;
            return c != 1 && c != 2 && c != 4 && c != 5 && c != 6 && c != 8;
        }
        int defaultDonor(const std::string &cls)
        {
            return cls == "bike" ? 212 : cls == "boat" ? 257 : cls == "heli" ? 213 : cls == "plane" ? 278 : 171;
        }

        struct Slot
        {
            int id = 0, txd = -1, other = 0, donor = 0;
            bool seenLoaded = false, idle = false, release = false;
            std::chrono::steady_clock::time_point at, idleSince;
        };

        std::mutex g_mutex;
        bool g_read = false;
        std::vector<Vehicle> g_vehicles;
        std::vector<Slot> g_slots;
        uint8_t *g_ram = nullptr;
        std::map<int, std::vector<uint8_t>> g_donorMi, g_donorHand; // first seen
        std::array<std::atomic<int>, kNewCount> g_bound{};          // the hooks' view: the LCS model a new id holds
        std::atomic<bool> g_hooked{false};
        PS2Runtime::RecompiledFunction g_textGet = nullptr, g_removeModel = nullptr;

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (a & kRamMask), 4);
            return v;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (a & kRamMask), 2);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & kRamMask), &v, 4); }
        void wr16(uint32_t a, uint16_t v) { std::memcpy(g_ram + (a & kRamMask), &v, 2); }
        uint8_t state(uint32_t entry) { return entry ? g_ram[(entry + 0xC) & kRamMask] : 0xFF; }
        uint32_t texIndex(int txd) { return vcs::mods::textureListBase() + static_cast<uint32_t>(txd); }

        void readPack()
        {
            if (g_read)
                return;
            g_read = true;
            if (const char *m = std::getenv("VCS_MODS"); m && *m == '0')
                return;
            const fs::path dir = PS2Runtime::getIoPaths().mcRoot.parent_path() / "mods" / kMod;
            std::ifstream in(dir / "vehicles.txt");
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::vector<std::string> col;
                std::stringstream ss(line);
                for (std::string c; std::getline(ss, c, '\t');)
                    col.push_back(c);
                if (col.size() < 5)
                    continue;
                Vehicle v;
                v.id = std::atoi(col[0].c_str());
                v.cls = col[1], v.label = col[2], v.dff = col[3], v.txd = col[4];
                if (col.size() > 5)
                {
                    std::istringstream h(col[5]);
                    for (std::string k, val; h >> k >> val;)
                        v.handling.emplace_back(k, val);
                }
                if (col.size() > 6)
                    v.donor = std::atoi(col[6].c_str());
                if (col.size() > 7)
                {
                    std::istringstream c(col[7]);
                    for (std::string pair; c >> pair;)
                        v.colours.push_back(pair);
                }
                if (v.id > 0)
                    g_vehicles.push_back(std::move(v));
            }
            for (int k = 0; k < kNewCount; ++k)
            {
                Slot s;
                s.id = kNewFirst + k;
                g_slots.push_back(s);
            }
            if (!g_vehicles.empty())
                std::fprintf(stderr, "[crossover] %zu LCS vehicles in %s; new model ids %d-%d\n", g_vehicles.size(),
                             dir.string().c_str(), kNewFirst, kNewFirst + kNewCount - 1);
        }

        bool fresh(const Slot &s)
        {
            return s.other && !s.seenLoaded && std::chrono::steady_clock::now() - s.at < std::chrono::seconds(30);
        }

        // VCS's paint: the nearest palette colours (indices 0..101) written as the model info's pairs (+0x161 count,
        // +0x163 + 2k {primary, secondary}; +0x278 the pair last used)
        void setColours(uint32_t mi, const std::vector<std::string> &pairs)
        {
            const uint32_t holder = rd32(kPaletteHolder) & kRamMask;
            const uint32_t pal = holder ? rd32(holder + 0xC) & kRamMask : 0;
            if (pal < 0x100000u || pairs.empty())
                return;
            auto nearest = [&](int r, int g, int b) {
                int best = 0;
                long bestD = -1;
                for (int i = 0; i < 102; ++i)
                {
                    const uint8_t *c = g_ram + ((pal + static_cast<uint32_t>(i + 0x80) * 3u) & kRamMask);
                    const long d = long(c[0] - r) * (c[0] - r) + long(c[1] - g) * (c[1] - g) + long(c[2] - b) * (c[2] - b);
                    if (bestD < 0 || d < bestD)
                        best = i, bestD = d;
                }
                return best;
            };
            int n = 0;
            for (const std::string &p : pairs)
            {
                int c[6];
                if (n >= 8 || std::sscanf(p.c_str(), "%d,%d,%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) != 6)
                    continue;
                g_ram[(mi + 0x163 + 2 * n) & kRamMask] = static_cast<uint8_t>(nearest(c[0], c[1], c[2]));
                g_ram[(mi + 0x164 + 2 * n) & kRamMask] = static_cast<uint8_t>(nearest(c[3], c[4], c[5]));
                ++n;
            }
            if (n)
                g_ram[(mi + 0x161) & kRamMask] = static_cast<uint8_t>(n), g_ram[(mi + 0x278) & kRamMask] = 0;
        }

        bool setUp(Slot &s, const Vehicle &v, std::string &why)
        {
            const int k = s.id - kNewFirst;
            const uint32_t miAt = kMiArea + static_cast<uint32_t>(k) * kModelInfo;
            const uint32_t handAt = kHandArea + static_cast<uint32_t>(k) * kHandling;
            // The texture list: VCS has texture-list streaming entries for slots below the collision base only (7534 +
            // 831) and no free slot there (the pool's free ones are 833..1999; 5 empty ones below; run v4). So a new id
            // SHARES its donor's texture list: the converter's list is the donor's own textures plus the LCS vehicle's
            // (vcs_modconv buildXet keeps the donor's), served for the donor's entry - the donor's own cars find theirs
            // in it. Hence a donor holds one LCS vehicle at a time, and its list must not be loaded when it is taken:
            // the pack's donor, the class's default, else any VCS vehicle of the class.
            // Wheels and dummies come from the donor: a donor of the same kind - the VCS class int32 (+0x54) of the
            // pack's nearest (else the class default), and never one of the odd ones (6-wheel ATV, baggage, bulldozer,
            // caddy, forklift) unless that is the nearest itself
            auto clsOf = [&](int id) {
                const uint32_t m = id > 0 ? vcs::mods::modelInfo(id) : 0;
                int32_t c = -1;
                if (m && g_ram[m + 0x10] == 6)
                    std::memcpy(&c, g_ram + m + 0x54, 4);
                return c;
            };
            auto odd = [](int id) { return id == 170 || id == 174 || id == 181 || id == 184 || id == 251; };
            const int want = classFits(clsOf(v.donor), v.cls) ? clsOf(v.donor) : clsOf(defaultDonor(v.cls));
            auto usable = [&](int id) {
                const uint32_t m = id > 0 ? vcs::mods::modelInfo(id) : 0;
                if (!m || g_ram[m + 0x10] != 6 || clsOf(id) != want || (odd(id) && id != v.donor))
                    return false;
                const int16_t t = static_cast<int16_t>(rd16(m + 0x1E));
                if (t < 0 || state(vcs::mods::streamEntry(texIndex(t))) != 0 || rd32(m + 0x38) < 0x100000u)
                    return false;
                for (const Slot &o : g_slots)
                    if (&o != &s && o.other && o.donor == id)
                        return false;
                return true;
            };
            int donor = usable(v.donor) ? v.donor : usable(defaultDonor(v.cls)) ? defaultDonor(v.cls) : 0;
            for (int id = 170; !donor && id <= 280; ++id)
                if (usable(id))
                    donor = id;
            if (!donor)
                return why = "No Vice City vehicle of its class is free to lend its model (leave some behind)", false;
            const uint32_t dmi = vcs::mods::modelInfo(donor);
            const uint32_t dhand = rd32(dmi + 0x38) & kRamMask;
            if (!g_donorMi.count(donor))
            {
                g_donorMi[donor].assign(g_ram + dmi, g_ram + dmi + kModelInfo);
                g_donorHand[donor].assign(g_ram + dhand, g_ram + dhand + kHandling);
            }
            s.txd = static_cast<int16_t>(rd16(dmi + 0x1E));
            const uint32_t e = vcs::mods::streamEntry(static_cast<uint32_t>(s.id));
            const uint32_t te = vcs::mods::streamEntry(texIndex(s.txd));
            if (!e || !te || state(e) || state(te))
                return why = "Model " + std::to_string(s.id) + " still loaded", false;
            // the model info: the donor's, its run-time state cleared (+0x04 / +0x0C / +0x28 its loaded objects, +0x1C
            // refs, +0x278 the colour cursor), the donor's texture list (+0x1E, above), out of the traffic's lists (+0x275 =
            // -1: the ring add 0x3C8CD8 calls 0x432E90 for a class != -1), its own text key (+0x158) and handling record
            std::memcpy(g_ram + miAt, g_donorMi[donor].data(), kModelInfo);
            for (uint32_t o : {0x04u, 0x0Cu, 0x28u})
                wr32(miAt + o, 0);
            wr16(miAt + 0x1C, 0);
            g_ram[miAt + 0x278] = 0;
            g_ram[miAt + 0x275] = 0xFF;
            char key[8] = {};
            std::snprintf(key, sizeof key, "LCS%d", s.id);
            std::memcpy(g_ram + miAt + 0x158, key, 8);
            std::memcpy(g_ram + handAt, g_donorHand[donor].data(), kHandling);
            wr32(miAt + 0x38, handAt);
            const uint32_t nameAt = kNameArea + static_cast<uint32_t>(k) * kNameBytes;
            std::memset(g_ram + nameAt, 0, kNameBytes);
            for (size_t i = 0; i < v.label.size() && i < kNameBytes / 2 - 1; ++i)
                wr16(nameAt + static_cast<uint32_t>(i) * 2, static_cast<uint8_t>(v.label[i]));
            for (uint32_t x : {e}) // (the texture list is the donor's: its entry is served by bindVehicle)
            {
                wr32(x + 4, 0), wr32(x + 8, 0);
                g_ram[(x + 0xD) & kRamMask] = 0;
                wr32(x + 0x10, 0), wr32(x + 0x14, 0);
            }
            if (static_cast<uint32_t>(s.id) < rd32(kModelInfoCount))
                wr32((rd32(kModelInfoPtrs) & kRamMask) + static_cast<uint32_t>(s.id) * 4u, miAt);
            std::string err;
            if (!vcs::mods::bindVehicle(g_ram, s.id, kMod, v.dff, v.txd, "lcs" + std::to_string(v.id), v.handling, err, donor))
            {
                wr32((rd32(kModelInfoPtrs) & kRamMask) + static_cast<uint32_t>(s.id) * 4u, 0);
                return why = v.label + ": " + err, false;
            }
            setColours(miAt, v.colours);
            s.donor = donor;
            g_bound[static_cast<size_t>(k)].store(v.id);
            return true;
        }

        // CText::Get: a new id's key "LCS281".. answers the LCS vehicle's name (the HUD reads the model info's +0x158)
        PS2_GUEST_CC void textHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t key = GPR_U32(ctx, 5) & kRamMask;
            if (key < 0x01FFFFF0u && rdram[key] == 'L' && rdram[key + 1] == 'C' && rdram[key + 2] == 'S')
            {
                const int id = std::atoi(reinterpret_cast<const char *>(rdram + key + 3));
                const int k = id - kNewFirst;
                if (k >= 0 && k < kNewCount && rdram[key + 6] == 0 && g_bound[static_cast<size_t>(k)].load(std::memory_order_relaxed))
                {
                    SET_GPR_U32(ctx, 2, kNameArea + static_cast<uint32_t>(k) * kNameBytes);
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
            }
            g_textGet(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (game::isLcs())
            return;
        if (const char *e = std::getenv("STORIES_NEWIDS"); e && *e == '0')
            return;
        g_textGet = runtime.lookupFunction(kTextGet);
        g_removeModel = runtime.lookupFunction(kRemoveModel);
        if (!g_textGet || !g_removeModel)
            return game::logUnavailable("LCS vehicles on new model ids (vcs_crossover: hooks not found)");
        runtime.replaceFunction(kTextGet, &textHook);
        g_hooked = true;
    }

    const std::vector<Vehicle> &vehicles()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hooked)
            readPack();
        return g_vehicles;
    }

    const Vehicle *find(int otherId)
    {
        for (const Vehicle &v : vehicles())
            if (v.id == otherId)
                return &v;
        return nullptr;
    }

    bool isNewId(int vcsModel) { return vcsModel >= kNewFirst && vcsModel < kNewFirst + kNewCount; }

    int bind(uint8_t *ram, int otherId, std::string &why, std::vector<int32_t> *evict, bool *retry)
    {
        if (retry)
            *retry = false;
        const Vehicle *v = find(otherId);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ram = ram;
        if (!v)
            return why = "Not in the Liberty City pack (tools/stories_pack.py)", 0;
        vcs::mods::setScratch(kScratchLo, kScratchHi);
        for (Slot &s : g_slots)
            if (s.other == otherId)
            {
                s.at = std::chrono::steady_clock::now();
                return s.id;
            }
        Slot *pick = nullptr;
        for (Slot &s : g_slots)
        {
            if (!s.other && s.txd < 0)
            {
                pick = &s;
                break;
            }
            const uint32_t mi = vcs::mods::modelInfo(s.id);
            if (state(vcs::mods::streamEntry(static_cast<uint32_t>(s.id))) != 0 || (mi && rd16(mi + 0x1C) != 0) || fresh(s))
                continue;
            if (!pick || (!s.other && pick->other))
                pick = &s;
        }
        if (pick)
        {
            Slot &s = *pick;
            const int was = s.other;
            s.other = 0;
            g_bound[static_cast<size_t>(s.id - kNewFirst)].store(0);
            if (!setUp(s, *v, why))
                return 0;
            s.other = otherId, s.seenLoaded = false, s.at = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[crossover] LCS %s (%d) bound to new VCS model %d (donor %d, texture list %d%s)\n",
                         v->label.c_str(), otherId, s.id, s.donor, s.txd, was ? (", was LCS " + std::to_string(was)).c_str() : "");
            return s.id;
        }
        // every id holds a vehicle: one with none left is unloaded (onFrame), else the oldest LCS model's vehicles (none
        // the player's; far ones first) are deleted, and the id taken on the next try
        if (retry)
        {
            Slot *idle = nullptr, *old = nullptr;
            std::vector<int32_t> oldCars;
            uint32_t cur = rd32(0x004CD128u);
            if (cur > 1)
                cur = 0;
            const uint32_t me = rd32(kPlayerPed + cur * 400u) & kRamMask;
            const uint32_t myCar = me ? rd32(me + 0x480) & kRamMask : 0;
            float mp[3] = {0, 0, 0};
            if (me)
                std::memcpy(mp, g_ram + me + 0x30, 12);
            const uint32_t vp = rd32(kVehiclePool) & kRamMask;
            const uint32_t entries = vp ? rd32(vp) & kRamMask : 0, flags = vp ? rd32(vp + 4) & kRamMask : 0;
            const uint32_t n = vp ? std::min<uint32_t>(rd32(vp + 8), 256) : 0;
            bool oldNear = true;
            for (Slot &s : g_slots)
            {
                if (!s.other || fresh(s))
                    continue;
                const uint32_t mi = vcs::mods::modelInfo(s.id);
                if (mi && rd16(mi + 0x1C) == 0)
                {
                    if (!idle || s.at < idle->at)
                        idle = &s;
                    continue;
                }
                std::vector<int32_t> cars;
                bool mine = false, near = false;
                for (uint32_t i = 0; i < n && entries && flags; ++i)
                {
                    if (g_ram[flags + i] & 0x80)
                        continue;
                    const uint32_t e = entries + i * kVehicleSize;
                    if (static_cast<int16_t>(rd16(e + kEntityModel)) != s.id)
                        continue;
                    mine = mine || e == myCar;
                    float p[3];
                    std::memcpy(p, g_ram + e + 0x30, 12);
                    near = near || std::hypot(p[0] - mp[0], p[1] - mp[1]) < 40.0f;
                    cars.push_back(static_cast<int32_t>(i << 8 | g_ram[flags + i])); // CPool::GetIndex 0x478928
                }
                if (mine || cars.empty())
                    continue;
                if (!old || (oldNear && !near) || (oldNear == near && s.at < old->at))
                    old = &s, oldCars = std::move(cars), oldNear = near;
            }
            if (idle)
                return idle->release = true, *retry = true, why = "Freeing a model", 0;
            if (old && evict)
            {
                old->release = true;
                *evict = std::move(oldCars);
                *retry = true;
                return why = "Removing an older Liberty City vehicle", 0;
            }
        }
        return why = "Every model id holds a Liberty City vehicle you are using", 0;
    }

    int boundOther(int vcsModel)
    {
        const int k = vcsModel - kNewFirst;
        return k >= 0 && k < kNewCount ? g_bound[static_cast<size_t>(k)].load(std::memory_order_relaxed) : 0;
    }

    void onFrame(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_hooked)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_slots.empty())
            return;
        g_ram = ram;
        // CStreaming's limit of vehicle models loaded at once (+0x1C: 12; its ring at +0x20 holds 20): raised to 20
        if (const uint32_t st = rd32(kStreaming) & kRamMask; st >= 0x100000u && rd32(st + 0x1C) == 12u)
            wr32(st + 0x1C, 20u);
        const auto now = std::chrono::steady_clock::now();
        for (Slot &s : g_slots)
        {
            if (!s.other)
                continue;
            const uint32_t entry = vcs::mods::streamEntry(static_cast<uint32_t>(s.id));
            const uint8_t st = state(entry);
            if (st == 1)
                s.seenLoaded = true;
            // no vehicle of it left for 3 s (or evicted): its model and texture list unloaded (CStreaming::RemoveModel
            // with the frame kick's context saved and restored); a wreck in the world keeps a reference
            const uint32_t mi = vcs::mods::modelInfo(s.id);
            const bool unused = entry && mi && rd16(mi + 0x1C) == 0 && !(g_ram[(entry + 0xD) & kRamMask] & 0x03) && st != 2 && st != 3;
            if (!unused || fresh(s))
                s.idle = false;
            else if (!s.idle)
                s.idle = true, s.idleSince = now;
            if (!(s.idle && g_removeModel && ctx && (s.release || now - s.idleSince > std::chrono::seconds(3))))
                continue;
            bool any = false;
            for (uint32_t index : {static_cast<uint32_t>(s.id), texIndex(s.txd)})
            {
                if (state(vcs::mods::streamEntry(index)) != 1)
                    continue;
                if (index != static_cast<uint32_t>(s.id))
                {
                    const uint32_t pool = rd32(kTxdPool) & kRamMask;
                    if (!pool || rd16((rd32(pool) & kRamMask) + static_cast<uint32_t>(s.txd) * kTxdEntry + 4) != 0)
                        continue;
                }
                const R5900Context saved = *ctx;
                SET_GPR_U32(ctx, 4, rd32(kStreaming));
                SET_GPR_U32(ctx, 5, index);
                SET_GPR_U32(ctx, 31, kRemoveModel);
                ctx->pc = kRemoveModel;
                g_removeModel(ram, ctx, runtime);
                *ctx = saved;
                any = true;
            }
            if (any)
                std::fprintf(stderr, "[crossover] VCS model %d (LCS %d): no vehicle of it left, unloaded (id free)\n", s.id, s.other);
            s.idle = false, s.release = false, s.seenLoaded = false;
        }
    }
}
