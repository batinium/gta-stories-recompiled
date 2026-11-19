// [crossover] VCS's real vehicles in LCS, bound on demand to new model ids (or pool slots): see lcs_crossover.h.
#include "game/lcs_crossover.h"

#include "game/game_table.h"
#include "game/vcs_mods.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace lcs::crossover
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr const char *kMod = "stories_vcs_vehicles";
        // LCS's model info is 0x190 bytes, a handling record 0xF0 (docs/LCS.md "Mods"); a streaming entry (stride 0x14):
        // +4 / +8 list links, +0xC load state (1 loaded; 0 not), +0xD flags, +0xE next index on the disc (int16, -1),
        // +0x10 first sector, +0x14 sector count (vcs_mods.cpp Profile)
        constexpr uint32_t kModelInfo = 0x190u, kHandling = 0xF0u;

        // ---- new model ids ------------------------------------------------------------------------------------------
        // LCS's vehicle ids end at 216 (VCNMAV); 217-236 (VC's last vehicle ids) have no model info (null in the model
        // info array, *0x3D8720; count *0x3D9BA0 = 4118) and empty streaming entries (read in GAME.DTZ and a run,
        // 2026-11-18). A VCS vehicle gets one of them: a copy of its donor's model info (the nearest LCS vehicle of its
        // class) in guest scratch, a texture list slot of its own (a free slot of the TXD pool), a handling record of
        // its own, and the converted model / textures served for its two streaming entries. Up to 20 at once; one whose
        // model is unloaded again (no vehicle of it left) takes the next VCS vehicle.
        constexpr int kNewFirst = 217, kNewCount = 20;
        // Guest scratch (docs/game-internals.md: 0x72000-0x76000 reserved for this; the EE kernel area the recompiled
        // game never uses): 20 model infos, 20 handling records, 20 HUD names (UTF-16, 15 characters)
        constexpr uint32_t kScratchLo = 0x00072000u, kScratchHi = 0x00076000u;
        constexpr uint32_t kMiArea = kScratchLo, kHandArea = kMiArea + kNewCount * kModelInfo,
                           kNameArea = kHandArea + kNewCount * kHandling, kNameBytes = 0x20u;
        static_assert(kNameArea + kNewCount * kNameBytes <= kScratchHi, "crossover scratch overflows");

        constexpr uint32_t kModelInfoPtrs = 0x003D8720u;  // CModelInfo::ms_modelInfoPtrs (0x18DBC8, 0x18E4B8)
        constexpr uint32_t kModelInfoCount = 0x003D9BA0u; // its count (4118)
        // CTxdStore's pool (*0x3D7CA0: entries 0x1C bytes, flags, size 1200 "TexList"; 0x2EAD30 LoadTxd, 0x2EADC0
        // RemoveTxd: a slot is used when its flag byte has no 0x80): +0 the texture dictionary, +4 refs (int16), +6,
        // +8 its name. 110 slots are free in a run (0..39 among them, streaming entries empty).
        constexpr uint32_t kTxdPool = 0x003D7CA0u;
        constexpr uint32_t kTxdEntry = 0x1Cu;

        // Each class's donor when the pack has none of its class: KURUMA, PCJ600, REEFER, MAVERIC, DODO. LCS class
        // byte: vehicle model info +0x38 (0 car, 1 boat, 2 train, 3 heli, 4 plane, 5 bike).
        int defaultDonor(const std::string &cls)
        {
            return cls == "bike" ? 205 : cls == "boat" ? 195 : cls == "heli" ? 214 : cls == "plane" ? 164 : 152;
        }
        int classByte(const std::string &cls)
        {
            return cls == "bike" ? 5 : cls == "boat" ? 1 : cls == "heli" ? 3 : cls == "plane" ? 4 : 0;
        }

        // ---- the pool (the fallback when no new id is free or one can't be set up) ------------------------------------
        // LCS models whose handling record no other model shares and that the traffic doesn't spawn. Cars: the special
        // cars ESPRIT 187 (handling id 60), SHELBY 185 (58), PONTIAC 186 (59), HOTROD 189 (62), SINDACO 190 (63),
        // FORELLI 191 (64); bikes ANGEL2 208 (70), SANCH2 209 (69); boats REEFER 195 (74), SPEEDER 194 (73).
        struct PoolDef
        {
            const char *cls;
            int ids[6];
        };
        constexpr PoolDef kPools[] = {
            {"car", {187, 185, 186, 189, 190, 191}},
            {"bike", {208, 209, 0, 0, 0, 0}},
            {"boat", {195, 194, 0, 0, 0, 0}},
        };

        struct Snapshot
        {
            std::vector<uint8_t> mi, hand;
            uint32_t ent[2][2] = {}; // model / texture list: {sector, count}
        };
        struct Slot
        {
            int id = 0;
            std::string cls;
            bool fresh = false; // a new model id (else a pool slot)
            int txd = -1;       // fresh: its texture list slot
            int other = 0;      // the VCS model it holds now
            int donor = 0;      // fresh: the LCS model it was copied from
            bool haveStock = false, seenLoaded = false;
            Snapshot stock;
            std::map<int, Snapshot> bound; // pool: VCS model -> the slot's values once converted (a later bind: copied back)
            std::chrono::steady_clock::time_point at;
            // fresh: no vehicle of it left (reference count 0) since `idleSince`; `release`: unload at once (evicted)
            bool idle = false, release = false;
            std::chrono::steady_clock::time_point idleSince;
        };

        std::mutex g_mutex;
        bool g_read = false;
        std::vector<Vehicle> g_vehicles;
        std::vector<Slot> g_slots;
        uint8_t *g_ram = nullptr;
        std::map<int, Snapshot> g_donorStock; // LCS donor model -> its model info / handling record as first seen

        // what the hooks read (game thread, without the lock): per new id its donor (0: none bound) and its record
        std::array<std::atomic<int>, kNewCount> g_newDonor{};
        std::array<std::atomic<uint32_t>, kNewCount> g_newHandling{};
        std::atomic<bool> g_hooked{false};
        PS2Runtime::RecompiledFunction g_removeModel = nullptr;
        constexpr uint32_t kRemoveModel = 0x0018C428u; // CStreaming::RemoveModel(index): 0x18C748 / 0x18C8A0 call it
        constexpr uint32_t kVehiclePool = 0x003D5AECu;  // -> CPool {entries, flags, size}, 0x780 a vehicle (lcs_population)
        constexpr uint32_t kVehicleSize = 0x780u;
        constexpr uint32_t kPlayerPed = 0x00408E40u;    // CWorld::Players[0].m_pPed; its vehicle +0x534

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

        uint32_t texEntry(int id)
        {
            const uint32_t mi = vcs::mods::modelInfo(id);
            if (!mi)
                return 0;
            return vcs::mods::streamEntry(vcs::mods::textureListBase() + rd16(mi + 0x1E));
        }

        bool take(int id, Snapshot &s)
        {
            const uint32_t mi = vcs::mods::modelInfo(id), hand = vcs::mods::handlingRecord(id);
            const uint32_t e[2] = {vcs::mods::streamEntry(static_cast<uint32_t>(id)), texEntry(id)};
            if (!mi || !hand || !e[0] || !e[1])
                return false;
            s.mi.assign(g_ram + mi, g_ram + mi + kModelInfo);
            s.hand.assign(g_ram + hand, g_ram + hand + kHandling);
            for (int k = 0; k < 2; ++k)
                s.ent[k][0] = rd32(e[k] + 0x10), s.ent[k][1] = rd32(e[k] + 0x14);
            return true;
        }

        void put(int id, const Snapshot &s)
        {
            const uint32_t mi = vcs::mods::modelInfo(id), hand = vcs::mods::handlingRecord(id);
            const uint32_t e[2] = {vcs::mods::streamEntry(static_cast<uint32_t>(id)), texEntry(id)};
            if (mi && s.mi.size() == kModelInfo)
                std::memcpy(g_ram + mi, s.mi.data(), kModelInfo);
            if (hand && s.hand.size() == kHandling)
                std::memcpy(g_ram + hand, s.hand.data(), kHandling);
            for (int k = 0; k < 2; ++k)
                if (e[k])
                    wr32(e[k] + 0x10, s.ent[k][0]), wr32(e[k] + 0x14, s.ent[k][1]);
        }

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
                if (col.size() > 6) // the pack's donor (stories_pack.py: the nearest LCS vehicle; 0 none)
                    v.donor = std::atoi(col[6].c_str());
                if (col.size() > 7) // its paint colours (VCS's palette)
                {
                    std::istringstream c(col[7]);
                    for (std::string pair; c >> pair;)
                        v.colours.push_back(pair);
                }
                if (v.id > 0)
                    g_vehicles.push_back(std::move(v));
            }
            if (!std::getenv("STORIES_NEWIDS") || std::getenv("STORIES_NEWIDS")[0] != '0')
                for (int k = 0; k < kNewCount; ++k)
                {
                    Slot s;
                    s.id = kNewFirst + k;
                    s.fresh = true;
                    g_slots.push_back(std::move(s));
                }
            for (const PoolDef &p : kPools)
                for (int id : p.ids)
                    if (id)
                    {
                        Slot s;
                        s.id = id;
                        s.cls = p.cls;
                        g_slots.push_back(std::move(s));
                    }
            if (!g_vehicles.empty())
                std::fprintf(stderr, "[crossover] %zu VCS vehicles in %s; %d new model ids (%d-%d) + %zu pool slots\n",
                             g_vehicles.size(), dir.string().c_str(), g_hooked ? kNewCount : 0, kNewFirst,
                             kNewFirst + kNewCount - 1, g_slots.size() - (g_hooked ? kNewCount : 0));
        }

        std::string poolOf(const std::string &cls) { return cls == "bike" || cls == "boat" ? cls : "car"; }

        bool fresh(const Slot &s)
        {
            return s.other && !s.seenLoaded && std::chrono::steady_clock::now() - s.at < std::chrono::seconds(30);
        }

        // ---- a new id ---------------------------------------------------------------------------------------------------

        // a free texture list slot for it: a pool slot marked free whose streaming entry is empty; taken (flag 0x80
        // cleared, the name written) for good
        int takeTxdSlot(int id)
        {
            const uint32_t pool = rd32(kTxdPool) & kRamMask;
            if (pool < 0x100000u)
                return -1;
            const uint32_t entries = rd32(pool) & kRamMask, flags = rd32(pool + 4) & kRamMask, size = rd32(pool + 8);
            for (uint32_t n = size > 1200 ? 1200 : size; n-- > 0;) // from the top: away from the game's own
            {
                const uint32_t e = vcs::mods::streamEntry(vcs::mods::textureListBase() + n);
                if (!e || !(g_ram[flags + n] & 0x80) || rd32(e + 0x14) || state(e))
                    continue;
                bool usedByUs = false;
                for (const Slot &s : g_slots)
                    usedByUs = usedByUs || s.txd == static_cast<int>(n);
                if (usedByUs)
                    continue;
                g_ram[flags + n] &= 0x7F;
                std::memset(g_ram + entries + n * kTxdEntry, 0, kTxdEntry);
                char name[20] = {};
                std::snprintf(name, sizeof name, "vcsveh%d", id);
                std::memcpy(g_ram + entries + n * kTxdEntry + 8, name, sizeof name);
                return static_cast<int>(n);
            }
            return -1;
        }

        // the donor's model info / handling record as they were before anything of ours touched them (a pool slot that
        // holds a VCS vehicle right now gives its stock values)
        const Snapshot *donorStock(int donor)
        {
            for (const Slot &s : g_slots)
                if (!s.fresh && s.id == donor && s.haveStock)
                    return &s.stock;
            auto it = g_donorStock.find(donor);
            if (it != g_donorStock.end())
                return &it->second;
            for (const Slot &s : g_slots)
                if (!s.fresh && s.id == donor && s.other)
                    return nullptr; // bound, and its stock not kept: not a donor now
            Snapshot snap;
            if (!take(donor, snap))
                return nullptr;
            return &(g_donorStock[donor] = std::move(snap));
        }

        bool setUpNew(Slot &s, const Vehicle &v, std::string &why)
        {
            const int k = s.id - kNewFirst;
            const uint32_t miAt = kMiArea + static_cast<uint32_t>(k) * kModelInfo;
            const uint32_t handAt = kHandArea + static_cast<uint32_t>(k) * kHandling;
            int donor = v.donor;
            const uint32_t dmi = donor > 0 ? vcs::mods::modelInfo(donor) : 0;
            if (!dmi || g_ram[dmi + 0x10] != 6 || g_ram[dmi + 0x38] != classByte(v.cls))
                donor = defaultDonor(v.cls); // none, or the pack's nearest is of another class (a bike for a car)
            const Snapshot *d = donorStock(donor);
            const uint32_t de = vcs::mods::streamEntry(static_cast<uint32_t>(donor));
            if (!d || !de)
                return why = "Donor model " + std::to_string(donor) + " not usable", false;
            if (s.txd < 0 && (s.txd = takeTxdSlot(s.id)) < 0)
                return why = "No free texture list slot", false;
            const uint32_t e = vcs::mods::streamEntry(static_cast<uint32_t>(s.id));
            const uint32_t te = vcs::mods::streamEntry(vcs::mods::textureListBase() + static_cast<uint32_t>(s.txd));
            if (!e || !te || state(e) || state(te))
                return why = "Model " + std::to_string(s.id) + " still loaded", false;

            // the model info: the donor's, its run-time state cleared (+0 / +0xC / +0x24 its loaded objects, +0x1C the
            // reference count), its own texture list (+0x1E), out of the traffic's class lists (+0x45 = -1: 0x1910D0 /
            // 0x2153D0 add a loaded model of class != -1), its own text key (+0x2E, 8 bytes: the HUD name, below)
            std::memcpy(g_ram + miAt, d->mi.data(), kModelInfo);
            wr32(miAt + 0x00, 0);
            wr32(miAt + 0x0C, 0);
            wr16(miAt + 0x1C, 0);
            wr32(miAt + 0x24, 0);
            wr16(miAt + 0x1E, static_cast<uint16_t>(s.txd));
            g_ram[miAt + 0x45] = 0xFF;
            char key[8] = {};
            std::snprintf(key, sizeof key, "VCS%d", s.id);
            std::memcpy(g_ram + miAt + 0x2E, key, 8);
            // the HUD name: VCS's own (CText::Get answers the key, textHook)
            const uint32_t nameAt = kNameArea + static_cast<uint32_t>(k) * kNameBytes;
            std::memset(g_ram + nameAt, 0, kNameBytes);
            for (size_t i = 0; i < v.label.size() && i < kNameBytes / 2 - 1; ++i)
                wr16(nameAt + static_cast<uint32_t>(i) * 2, static_cast<uint8_t>(v.label[i]));
            // its handling record: the donor's, then the VCS vehicle's fields (bindVehicle); vehicles of it point there
            std::memcpy(g_ram + handAt, d->hand.data(), kHandling);
            vcs::mods::setHandlingOverride(miAt, handAt);
            // the streaming entries: empty, not linked, the donor's vehicle flag (+0xD 0x04), no next entry
            for (uint32_t x : {e, te})
            {
                wr32(x + 4, 0), wr32(x + 8, 0);
                g_ram[(x + 0xD) & kRamMask] = g_ram[(de + 0xD) & kRamMask] & 0x04;
                wr16(x + 0xE, 0xFFFF);
                wr32(x + 0x10, 0), wr32(x + 0x14, 0);
            }
            if (static_cast<uint32_t>(s.id) < rd32(kModelInfoCount))
                wr32((rd32(kModelInfoPtrs) & kRamMask) + static_cast<uint32_t>(s.id) * 4u, miAt);
            std::string err;
            if (!vcs::mods::bindVehicle(g_ram, s.id, kMod, v.dff, v.txd, "vcs" + std::to_string(v.id), v.handling, err, donor))
            {
                wr32((rd32(kModelInfoPtrs) & kRamMask) + static_cast<uint32_t>(s.id) * 4u, 0);
                vcs::mods::setHandlingOverride(miAt, 0);
                return why = v.label + ": " + err, false;
            }
            // VCS's paint (the model info's colour lists +0x170 / +0x178 / +0x180: the nearest LCS palette colours)
            vcs::mods::setVehicleColours(g_ram, s.id, kMod, v.colours);
            s.donor = donor;
            g_newHandling[static_cast<size_t>(k)].store(handAt);
            g_newDonor[static_cast<size_t>(k)].store(donor);
            return true;
        }

        // ---- hooks ----------------------------------------------------------------------------------------------------

        int donorOf(int model)
        {
            const int k = model - kNewFirst;
            return k >= 0 && k < kNewCount ? g_newDonor[static_cast<size_t>(k)].load(std::memory_order_relaxed) : 0;
        }

        // cHandlingDataMgr::GetFlyingPointer (0x144268, (mgr, id) -> mgr + 0x2D20 + id x 0x70 for ids 75-80) is called
        // by the vehicle constructors right after `this +0x150 = mgr + 0x20 + id x 0xF0` (id: the model info's +0x42 byte),
        // before anything reads +0x150: CAutomobile 0x1221D0 (this in s1; return address 0x1224D4), CBike 0x154A88 (s0;
        // 0x154C8C), CBoat 0x17CED0 (s1; 0x17CFCC). A vehicle of a new id gets its own record there (the id stays the
        // donor's: the bike / boat / flying records are the donor's).
        constexpr uint32_t kGetFlying = 0x00144268u;
        PS2Runtime::RecompiledFunction g_getFlying = nullptr;
        PS2_GUEST_CC void getFlyingHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const int reg = ra == 0x001224D4u ? 17 : ra == 0x00154C8Cu ? 16 : ra == 0x0017CFCCu ? 17 : 0;
            if (reg)
            {
                const uint32_t veh = GPR_U32(ctx, reg) & kRamMask;
                int16_t model;
                std::memcpy(&model, rdram + veh + 0x58, 2); // CEntity model index (lcs_trainer.cpp kEntityModel)
                const int k = model - kNewFirst;
                if (k >= 0 && k < kNewCount && g_newDonor[static_cast<size_t>(k)].load(std::memory_order_relaxed))
                {
                    const uint32_t rec = g_newHandling[static_cast<size_t>(k)].load(std::memory_order_relaxed);
                    std::memcpy(rdram + veh + 0x150, &rec, 4);
                    // CBike's constructor sets its bike type (+0x3F4) only for its own model ids (a switch on 202-210
                    // before this call: 202 / 207 / 208 -> 5, 203 / 204 / 206 -> 4, 205 -> 3, 209 / 210 -> 6); a new id
                    // kept what the pool slot held before (a car's bytes: hidden run n4 stalled the game on such a bike).
                    // The donor's type is set.
                    if (reg == 16)
                    {
                        const int d = g_newDonor[static_cast<size_t>(k)].load(std::memory_order_relaxed);
                        const int32_t type = d == 205 ? 3 : d == 203 || d == 204 || d == 206 ? 4 : d == 209 || d == 210 ? 6 : 5;
                        std::memcpy(rdram + veh + 0x3F4, &type, 4);
                    }
                }
            }
            g_getFlying(rdram, ctx, runtime);
        }

        // The vehicle sounds: cAudioManager's table at 0x3B2BE0, 0x1C bytes per vehicle model - 130 (130..216; the bytes
        // after 216 are the next table), read by ProcessVehicle 0x19ED80's helpers from its parameter block (a1, the
        // index at +0x14 = model - 130): 0x1A1368 (engine), 0x1A1ED8, 0x1A3120, 0x1A3398, 0x1A3EA0. A new id's index
        // becomes its donor's (the donor's engine sound) before each of them runs; the block is rebuilt every call.
        constexpr uint32_t kAudioUsers[] = {0x001A1368u, 0x001A1ED8u, 0x001A3120u, 0x001A3398u, 0x001A3EA0u};
        std::array<PS2Runtime::RecompiledFunction, 5> g_audioOrig{};
        template <int N>
        PS2_GUEST_CC void audioHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t block = GPR_U32(ctx, 5) & kRamMask;
            int32_t index;
            std::memcpy(&index, rdram + block + 0x14, 4);
            if (const int donor = donorOf(index + 130))
            {
                const int32_t to = donor - 130;
                std::memcpy(rdram + block + 0x14, &to, 4);
            }
            g_audioOrig[N](rdram, ctx, runtime);
        }

        // The player's vehicle sound bank (0x19E808: table byte +4 of (model - 130), asked of / loaded by 0x114960 /
        // 0x1148E8 with (0x3DF580, bank)): for a new id, the donor's bank. The vehicle: Players[cur] (0x408E40 +
        // *0x3D9BC8 x 400) +4 if set, else its ped's (+0) vehicle (+0x534).
        constexpr uint32_t kBankQuery = 0x00114960u, kBankLoad = 0x001148E8u;
        PS2Runtime::RecompiledFunction g_bankQuery = nullptr, g_bankLoad = nullptr;
        void bankFix(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra < 0x0019E808u || ra >= 0x0019ED80u)
                return;
            uint32_t cur, player, veh;
            std::memcpy(&cur, rdram + 0x003D9BC8u, 4);
            if (cur > 1)
                return;
            std::memcpy(&veh, rdram + 0x00408E44u + cur * 400u, 4);
            if (!veh)
            {
                std::memcpy(&player, rdram + 0x00408E40u + cur * 400u, 4);
                if (!player)
                    return;
                std::memcpy(&veh, rdram + (player & kRamMask) + 0x534, 4);
            }
            if (!veh)
                return;
            int16_t model;
            std::memcpy(&model, rdram + (veh & kRamMask) + 0x58, 2);
            const int donor = donorOf(model);
            if (donor && GPR_U32(ctx, 5) == rdram[0x003B2BE4u + static_cast<uint32_t>(model - 130) * 0x1Cu])
                SET_GPR_U32(ctx, 5, rdram[0x003B2BE4u + static_cast<uint32_t>(donor - 130) * 0x1Cu]);
        }
        PS2_GUEST_CC void bankQueryHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            bankFix(rdram, ctx);
            g_bankQuery(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void bankLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            bankFix(rdram, ctx);
            g_bankLoad(rdram, ctx, runtime);
        }

        // CText::Get (0x332E68: (TheText, key) -> UTF-16 text; lcs_prompts.cpp): a new id's key "VCS217".. answers the
        // VCS vehicle's name (the HUD's vehicle name reads the model info's +0x2E key)
        constexpr uint32_t kTextGet = 0x00332E68u;
        PS2Runtime::RecompiledFunction g_textGet = nullptr;
        PS2_GUEST_CC void textHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t key = GPR_U32(ctx, 5) & kRamMask;
            if (key < 0x01FFFFF0u && rdram[key] == 'V' && rdram[key + 1] == 'C' && rdram[key + 2] == 'S')
            {
                const int id = std::atoi(reinterpret_cast<const char *>(rdram + key + 3));
                if (donorOf(id) && rdram[key + 6] == 0)
                {
                    SET_GPR_U32(ctx, 2, kNameArea + static_cast<uint32_t>(id - kNewFirst) * kNameBytes);
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
            }
            g_textGet(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("STORIES_NEWIDS"); e && *e == '0')
            return void(std::fprintf(stderr, "[crossover] new model ids off (STORIES_NEWIDS=0): pool slots only\n"));
        g_getFlying = runtime.lookupFunction(kGetFlying);
        g_bankQuery = runtime.lookupFunction(kBankQuery);
        g_bankLoad = runtime.lookupFunction(kBankLoad);
        g_textGet = runtime.lookupFunction(kTextGet);
        g_removeModel = runtime.lookupFunction(kRemoveModel);
        bool ok = g_getFlying && g_bankQuery && g_bankLoad && g_textGet;
        for (size_t i = 0; i < g_audioOrig.size(); ++i)
            ok = (g_audioOrig[i] = runtime.lookupFunction(kAudioUsers[i])) && ok;
        if (!ok)
            return game::logUnavailable("VCS vehicles on new model ids (lcs_crossover: hooks not found; pool slots only)");
        runtime.replaceFunction(kGetFlying, &getFlyingHook);
        runtime.replaceFunction(kBankQuery, &bankQueryHook);
        runtime.replaceFunction(kBankLoad, &bankLoadHook);
        runtime.replaceFunction(kTextGet, &textHook);
        runtime.replaceFunction(kAudioUsers[0], &audioHook<0>);
        runtime.replaceFunction(kAudioUsers[1], &audioHook<1>);
        runtime.replaceFunction(kAudioUsers[2], &audioHook<2>);
        runtime.replaceFunction(kAudioUsers[3], &audioHook<3>);
        runtime.replaceFunction(kAudioUsers[4], &audioHook<4>);
        vcs::mods::setScratch(kScratchLo, kScratchHi);
        g_hooked = true;
    }

    const std::vector<Vehicle> &vehicles()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
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

    int bind(uint8_t *ram, int otherId, std::string &why, std::vector<int32_t> *evict, bool *retry)
    {
        if (retry)
            *retry = false;
        const Vehicle *v = find(otherId);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ram = ram;
        if (!v)
            return why = "Not in the Vice City pack (tools/stories_pack.py)", 0;
        const std::string pool = poolOf(v->cls);
        // already there
        for (Slot &s : g_slots)
            if (s.other == otherId)
            {
                s.at = std::chrono::steady_clock::now();
                return s.id;
            }
        // a new model id: never used first, then one whose model is unloaded again (no vehicle of it left)
        std::string freshWhy;
        if (g_hooked)
        {
            vcs::mods::setScratch(kScratchLo, kScratchHi);
            Slot *pick = nullptr;
            for (Slot &s : g_slots)
            {
                if (!s.fresh)
                    continue;
                if (!s.other && s.txd < 0)
                {
                    pick = &s;
                    break;
                }
                const uint32_t e = vcs::mods::streamEntry(static_cast<uint32_t>(s.id));
                const uint32_t mi = vcs::mods::modelInfo(s.id);
                if (state(e) != 0 || (s.txd >= 0 && state(vcs::mods::streamEntry(vcs::mods::textureListBase() + s.txd)) != 0) ||
                    (mi && rd16(mi + 0x1C) != 0) || fresh(s))
                    continue;
                if (!pick || (!s.other && pick->other))
                    pick = &s;
            }
            if (pick)
            {
                Slot &s = *pick;
                const int was = s.other;
                s.other = 0;
                if (const int k = s.id - kNewFirst; k >= 0 && k < kNewCount)
                    g_newDonor[static_cast<size_t>(k)].store(0);
                if (setUpNew(s, *v, freshWhy))
                {
                    s.other = otherId;
                    s.cls = pool;
                    s.seenLoaded = false;
                    s.at = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[crossover] VCS %s (%d) bound to new LCS model %d (donor %d, texture list %d%s)\n",
                                 v->label.c_str(), otherId, s.id, s.donor, s.txd,
                                 was ? (", was VCS " + std::to_string(was)).c_str() : "");
                    return s.id;
                }
                std::fprintf(stderr, "[crossover] new model %d for VCS %d not set up: %s; trying a pool slot\n", s.id, otherId,
                             freshWhy.c_str());
            }
        }
        // a free pool slot of its class: nothing bound, or bound but its model unloaded (no vehicle of it left)
        Slot *pick = nullptr;
        for (Slot &s : g_slots)
        {
            if (s.fresh || s.cls != pool)
                continue;
            const uint32_t e = vcs::mods::streamEntry(static_cast<uint32_t>(s.id)), t = texEntry(s.id);
            if (state(e) != 0 || state(t) != 0)
                continue; // loaded (in use: a spawned one, a mission's, or the last one still around)
            if (fresh(s))
                continue; // bound a moment ago, still being streamed in for its spawn
            pick = &s;
            if (!s.other)
                break;
        }
        if (!pick)
        {
            // every id holds a vehicle: free one. A model with no vehicle left is unloaded (onFrame); else the oldest
            // VCS model none of whose vehicles is the player's: its vehicles are deleted (the trainer's script,
            // DELETE_CAR: `evict`), far ones first, and the id is taken on the next try (`retry`)
            if (g_hooked && retry)
            {
                Slot *idle = nullptr, *old = nullptr;
                std::vector<int32_t> oldCars;
                const uint32_t me = rd32(kPlayerPed) & kRamMask;
                const uint32_t myCar = me ? rd32(me + 0x534) & kRamMask : 0;
                float mp[3] = {0, 0, 0};
                if (me)
                    std::memcpy(mp, g_ram + me + 0x30, 12);
                const uint32_t vp = rd32(kVehiclePool) & kRamMask;
                const uint32_t entries = vp ? rd32(vp) & kRamMask : 0, flags = vp ? rd32(vp + 4) & kRamMask : 0;
                const uint32_t n = vp ? std::min<uint32_t>(rd32(vp + 8), 256) : 0;
                bool oldNear = true;
                for (Slot &s : g_slots)
                {
                    if (!s.fresh || !s.other || fresh(s))
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
                        if (static_cast<int16_t>(rd16(e + 0x58)) != s.id)
                            continue;
                        mine = mine || e == myCar;
                        float p[3];
                        std::memcpy(p, g_ram + e + 0x30, 12);
                        near = near || std::hypot(p[0] - mp[0], p[1] - mp[1]) < 40.0f;
                        cars.push_back(static_cast<int32_t>(i << 8 | g_ram[flags + i])); // CPool::GetIndex (0x1120E8)
                    }
                    if (mine || cars.empty())
                        continue;
                    if (!old || (oldNear && !near) || (oldNear == near && s.at < old->at)) // the oldest, far before near
                        old = &s, oldCars = std::move(cars), oldNear = near;
                }
                if (idle)
                {
                    idle->release = true;
                    *retry = true;
                    return why = "Freeing a model", 0;
                }
                if (old && evict)
                {
                    old->release = true;
                    *evict = std::move(oldCars);
                    *retry = true;
                    std::fprintf(stderr, "[crossover] all ids in use: VCS %d's %zu vehicle(s) on LCS model %d removed for VCS %d\n",
                                 old->other, evict->size(), old->id, otherId);
                    return why = "Removing an older Vice City vehicle", 0;
                }
            }
            return why = "Every model id holds a Vice City vehicle you are using", 0;
        }
        Slot &s = *pick;
        if (!s.haveStock)
        {
            if (!take(s.id, s.stock))
                return why = "The game's tables are not ready yet", 0;
            s.haveStock = true;
        }
        if (s.other)
            put(s.id, s.stock); // the one it held before: stock values back first
        s.other = 0;
        if (auto it = s.bound.find(otherId); it != s.bound.end())
            put(s.id, it->second);
        else
        {
            std::string err;
            if (!vcs::mods::bindVehicle(ram, s.id, kMod, v->dff, v->txd, "vcs" + std::to_string(otherId), v->handling, err))
            {
                put(s.id, s.stock);
                return why = v->label + ": " + err, 0;
            }
            Snapshot b;
            if (take(s.id, b))
                s.bound[otherId] = std::move(b);
        }
        s.other = otherId;
        s.seenLoaded = false;
        s.at = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[crossover] VCS %s (%d) bound into LCS pool slot %d\n", v->label.c_str(), otherId, s.id);
        return s.id;
    }

    int boundOther(int lcsModel)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Slot &s : g_slots)
            if (s.id == lcsModel && s.other)
                return s.other;
        return 0;
    }

    void onFrame(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_slots.empty())
            return;
        g_ram = ram;
        // CStreaming's limit of vehicle models loaded at once (gp - 0x6224 = 0x3D0CCC: 12, read by 0x1910D0, which
        // evicts an unused one past it; its ring at +0x1E280 holds 20): raised to the ring's 20 while VCS vehicles are
        // about, so 12 models - traffic's included - don't cap them (agent run ev1: 9 VCS vehicles at most)
        if (g_hooked && rd32(0x003D0CCCu) == 12u)
            wr32(0x003D0CCCu, 20u);
        const auto now = std::chrono::steady_clock::now();
        for (Slot &s : g_slots)
        {
            if (!s.other)
                continue;
            const uint32_t entry = vcs::mods::streamEntry(static_cast<uint32_t>(s.id));
            const uint8_t st = state(entry);
            if (s.fresh)
            {
                // a new id with no vehicle of it left (destroyed and cleared, or culled) for 3 s, or evicted: its model
                // and texture list are unloaded now (CStreaming::RemoveModel), so the id is free at once. A wreck still
                // in the world holds a reference and keeps it. Not while it is requested or kept by a script (+0xD 1 / 2).
                const uint32_t mi = vcs::mods::modelInfo(s.id);
                const bool unused = entry && mi && rd16(mi + 0x1C) == 0 && !(g_ram[(entry + 0xD) & kRamMask] & 0x03) &&
                                    st != 2 && st != 3;
                if (!unused || fresh(s))
                    s.idle = false;
                else if (!s.idle)
                    s.idle = true, s.idleSince = now;
                if (s.idle && g_removeModel && ctx && (s.release || now - s.idleSince > std::chrono::seconds(3)))
                {
                    const uint32_t tex = vcs::mods::textureListBase() + static_cast<uint32_t>(s.txd);
                    bool any = false;
                    for (uint32_t index : {static_cast<uint32_t>(s.id), tex})
                    {
                        if (state(vcs::mods::streamEntry(index)) != 1)
                            continue;
                        if (index == tex) // the texture list only once nothing uses it (CTxdStore refs +4)
                        {
                            const uint32_t pool = rd32(kTxdPool) & kRamMask;
                            if (!pool || rd16((rd32(pool) & kRamMask) + static_cast<uint32_t>(s.txd) * kTxdEntry + 4) != 0)
                                continue;
                        }
                        const R5900Context saved = *ctx; // the game's own function, called at the frame's start
                        SET_GPR_U32(ctx, 4, index);
                        SET_GPR_U32(ctx, 31, kRemoveModel);
                        ctx->pc = kRemoveModel;
                        g_removeModel(ram, ctx, runtime);
                        *ctx = saved;
                        any = true;
                    }
                    if (any)
                        std::fprintf(stderr, "[crossover] LCS model %d (VCS %d): no vehicle of it left, unloaded (id free)\n", s.id,
                                     s.other);
                    s.idle = false, s.release = false, s.seenLoaded = false;
                }
                continue; // the id keeps its VCS vehicle until another one needs it (bind)
            }
            if (st != 0)
            {
                s.seenLoaded = s.seenLoaded || st == 1;
                continue;
            }
            const bool stale = !s.seenLoaded && std::chrono::steady_clock::now() - s.at > std::chrono::seconds(30);
            if ((s.seenLoaded || stale) && state(texEntry(s.id)) == 0)
            {
                std::fprintf(stderr, "[crossover] LCS model %d unloaded: VCS %d unbound, the stock model back\n", s.id, s.other);
                put(s.id, s.stock);
                s.other = 0;
                s.seenLoaded = false;
            }
        }
    }
}
