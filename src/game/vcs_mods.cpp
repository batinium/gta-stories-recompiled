// [android] Mod loader, first step: see vcs_mods.h for the design and the active.txt format, docs/ANDROID.md "Mods"
// for the field map and its evidence.
//
// GTAG layout used here (offsets in GAME.DTZ's inflated image, which the game relocates in place: a pointer in it is
// image-relative until FUN_00324A50 adds the image's address):
//   +0x38 number of model infos (7941), +0x3C CModelInfo::ms_modelInfoPtrs, +0x7C the streaming object (+200: the
//   streaming info array, 0x18 bytes per index, +0x10 first sector in GTA3PS2.IMG, +0x14 sector count), +0xC0 a pair
//   of pointers to the two weapon tables (40 x 0x70 each).
//   Vehicle model info: type byte +0x10 == 6, +0x38 its handling record.
//
// [lcs] Liberty City Stories (SLUS_214.23) has the same loader with its own addresses (Profile below, docs/LCS.md
// "Mods"). LCS's GAME.DTZ header is not VCS's layout, so its tables are taken where the game's own loader hands them
// out (lcsTablesHook), and disc files are replaced by name through the game's disc index (lcsOpenHook).
#include "game/vcs_mods.h"

#include "game/game_functions.h"
#include "game/game_table.h"
#include "game/lcs_trainer.h"
#include "game/vcs_modconv.h"
#include "game/vcs_modfiles.h"
#include "game/vcs_trainer.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace vcs::mods
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr uint32_t kRelocate = 0x00324A50u;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kNumModelInfos = 7941u;
        constexpr uint32_t kImageSize = 0x6495D0u; // GTAG fileEnd: the image's extent, for pointer checks
        constexpr int kWeaponTypes = 40;
        constexpr uint32_t kWeaponRecord = 0x70u;

        // What differs between the games. LCS's evidence (SLUS_214.23, disassembly of the extracted ELF):
        //  - streaming: CStreaming's info array pointer 0x3D0CC8 (gp - 0x6228, gp = 0x3D6EF0), set by 0x18A3D0 from the
        //    GAME.DTZ loader (0x1045A8, at 0x104774). Entry i at ptr + i x 0x14 (the loops at 0x108D1C / 0x18F1BC
        //    multiply by 0x14): +0xC load state (1 = loaded), +0xD flags; GetCdPosnAndSize 0x18A390 gets (entry + 4) and
        //    reads +0xC / +0x10, so the entry's +0x10 is the first sector in GTA3PS2.IMG and +0x14 the sector count -
        //    VCS's offsets with a 0x14 stride (VCS 0x18). Bounds: models < 0x1324, texture lists 0x1324..0x17D3
        //    (RequestModelStream 0x18F1D8: index - 0x1324 < 0x4B0 -> the texture-list loader 0x18DBC8), then more up to
        //    6197 (the IMG's LSN / size follow the array: 0x18A448 passes ptr + 0x1E270 / + 0x1E274 to the disc lookup
        //    0x146F80 for "\MODELS\GTA3PS2.IMG;1", and 0x1E270 = 6198 x 0x14 is entry 6197's +0x14). Read in a run
        //    (VCS_MODS_DUMP=1): entry 1 = 0 0 0 135E0100 45 1A (state 0, flags 1, sector 0x45, 0x1A sectors).
        //  - a model's texture list: 0x18F31C lh +0x1E (model info) + 0x1324 - VCS's +0x1E with base 4900 (VCS 7534).
        //  - the model's game name: +0x2E (lcs_trainer.cpp kModelGameName; VCS +0x158).
        //  - the IMG is read by sector through CDSTREAM (0x18F540: CdStreamRead(channel, buffer, IMG LSN + sector, n)),
        //    never through the file layer, so nothing clamps a read past the disc's end: no extendImg as on VCS.
        //  - weapons: *0x3D9B48 -> two CWeaponInfo tables (40 x 0x70; GetWeaponInfo 0x34F2F0 uses the second), the
        //    same record as VCS's 4 bytes lower: +0 fire type, +4 range, +8 rate, +0xC reload, +0x10 clip, +0x14 damage,
        //    +0x18 speed, +0x1C radius, +0x20 lifespan, +0x24 spread, +0x60 model (the pistol, type 17: 1, 30.0, 209,
        //    450, 17, 25, -1, -1, -1, -1, model 0x112; the shotgun's range is 30 / 40 in the two tables, as VCS's
        //    differ). Read in a run.
        //  - handling: the vehicle model info holds a handling id (int16 +0x42), not a pointer (VCS: +0x38). The records
        //    are cHandlingDataMgr's: GAME.DTZ header word 0x22 (file +0xA8; the loader 0x1045A8 stores it in 0x3D0238,
        //    read e.g. at 0x327EEC), the manager = 5 floats {0.1, 0.9, 1.0, 0.8, 0.98} (re3's field_0..field_10), padding,
        //    81 records of 0xF0 from +0x20, record i's +0x10 = i. A spawned Kuruma (model 152, id 20) points at
        //    manager + 0x20 + 20 x 0xF0 from vehicle +0x150 (read in a run; 0x327EE4 loads +0x150). Field map: kLcsHandling.

        // A handling record's layout, offsets from the address the game's pointer holds (kNone: the game has no such
        // field). VCS: the pointer is the gear table (docs/ANDROID.md "Mods" field map). [lcs] LCS: the record starts
        // with the mass; the order is SA-like (drag multiplier, engine inertia, collision damage multiplier stored).
        constexpr uint32_t kNone = UINT32_MAX;
        struct HandlingLayout
        {
            uint32_t size;
            uint32_t gears, accel, maxVel, cruiseVel, reverseVel, drive, engine, numGears, brake, brakeBias, steer,
                tractionLoss, tractionBias, suspForce, suspDamp, suspUpper, suspLower, suspBias, antiDive, seatOffset, value,
                centreOfMass, submerged, buoyancy, maxSpeedCfg, mass, turnMass, tractionMult;
            uint32_t ident, invMass, collision, drag, inertia; // [lcs] LCS only
            uint32_t abs, flags, lights;                       // [lcs] LCS only (VCS: not located, reported)
        };
        constexpr HandlingLayout kVcsHandling{0xE0u, 0x00u, 0x48u, 0x50u, 0x54u, 0x58u, 0x5Cu, 0x5Du, 0x5Eu, 0x60u, 0x64u,
                                              0x68u, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu, 0x80u, 0x84u, 0x88u, 0x8Cu, 0x90u,
                                              0xA0u, 0xB0u, 0xB4u, 0xB8u, 0xBCu, 0xC0u, 0xC4u,
                                              kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone};
        // [lcs] measured over all 81 records of the user's GAME.DTZ (tools: the records listed with these offsets):
        // +0 mass, +4 turn mass (LANDSTAL 5500; ADMIRAL-like 4056.4 / 4351.7 as VCS), +8 traction multiplier,
        // +0xC collision damage multiplier = cfg x 2000 / mass (0.54 for both record 1 (1600 kg, 0.675) and 2 (1400 kg,
        // 0.7714)), +0x10 the id, +0x14 1 / mass, +0x18 drag multiplier (SA's: 2.5, 2.0...), +0x20 centre of mass
        // (x, y, z: LANDSTAL 0, 0, -0.35), +0x30 percent submerged (int), +0x34 buoyancy (= 100 / submerged x 0.008 x
        // mass in every record), +0x38 the gear table (6 x 12, VC's InitGearRatios layout as VCS's), +0x80 / +0x81 /
        // +0x82 drive, engine, gears (letters as VCS), +0x84 engine acceleration (x 2500 x 2 or x 4: round numbers),
        // +0x88 engine inertia (SA's: 10, 20, 5...), +0x8C max velocity, +0x90 cruise (= max / 1.2), +0x94 reverse
        // (-0.2; bikes -0.05), +0x9C brake (x 2500 round: 7.0, 5.4, 8.0), +0xA0 brake bias, +0xA4 ABS, +0xA8 steering
        // lock (degrees: 25..45), +0xAC / +0xB0 traction loss / bias, +0xB8..+0xCC suspension force, damping, upper,
        // lower, bias, anti-dive (VC order and values), +0xD0 flags, +0xD4 seat offset, +0xD8 value ($), +0xDC lights.
        // No cfg max speed is kept (VCS +0xB8).
        constexpr HandlingLayout kLcsHandling{0xF0u, 0x38u, 0x84u, 0x8Cu, 0x90u, 0x94u, 0x80u, 0x81u, 0x82u, 0x9Cu, 0xA0u,
                                              0xA8u, 0xACu, 0xB0u, 0xB8u, 0xBCu, 0xC0u, 0xC4u, 0xC8u, 0xCCu, 0xD4u, 0xD8u,
                                              0x20u, 0x30u, 0x34u, kNone, 0x00u, 0x04u, 0x08u,
                                              0x10u, 0x14u, 0x0Cu, 0x18u, 0x88u, 0xA4u, 0xD0u, 0xDCu};

        struct Profile
        {
            uint32_t streamEntry;      // bytes per streaming info entry
            uint32_t textureListBase;  // texture list streaming index = this + model info +0x1E
            uint32_t streamIndices;    // indices with an entry (models, texture lists, collision, animations)
            int vehicleFirst, vehicleLast;
            uint32_t modelName;        // model info: the game name (8 characters)
            uint32_t handlingPtr;      // vehicle model info: its handling record (0: an id instead, handlingId)
            uint32_t weaponShift;      // weapon record fields sit this many bytes below VCS's (kWeapon offsets)
            bool clampedImg;           // VCS: the IMG is read through the file layer, which clamps (extendImg)
            uint32_t frameFreeCamera;  // u32 == 1 while no script camera runs (free play; the device tests' wait)
            uint32_t handlingId;       // [lcs] vehicle model info: int16 handling id (records at Gtag::handlingRecords)
            const HandlingLayout *handling;
        };
        // VCS: research ~8616 indices, vehicles 170..280; free play = TheCamera +0x888 (vcs_save_anywhere.cpp)
        constexpr Profile kVcsProfile{0x18u, 7534u, 8700u, 170, 280, 0x158u, 0x38u, 0u, true, 0x006F44D0u + 0x888u, kNone,
                                      &kVcsHandling};
        // LCS: vehicles anywhere in the model infos (the trainer lists type 6 over the whole count); free play =
        // TheCamera + 0x9C (lcs_trainer.cpp cameraLooksAtPlayer). Handling id +0x42: the vehicle model infos in GAME.DTZ
        // hold 0..80 there in VC's handling order (LANDSTK 0, IDAHO 1, STINGER 2, LINERUN 3 ... KURUMA 20; +0x38 is the
        // class: 0 car, 1 boat, 2 train, 3 heli, 4 plane, 5 bike), and a spawned Kuruma's record pointer matches id 20.
        constexpr Profile kLcsProfile{0x14u, 0x1324u, 6197u, 1, 4117, 0x2Eu, 0u, 4u, false, 0x0043BC8Cu, 0x42u, &kLcsHandling};
        const Profile *g_profile = &kVcsProfile;
        const Profile &P() { return *g_profile; }

        uint8_t *g_ram = nullptr;
        int g_testGive = 0;             // "test give=<weapon type>": handed to the player once free play starts
        int g_testSpawn = 0;            // "test spawn=<vehicle model>": spawned (and entered) once free play starts
        uint32_t g_watchIndex = UINT32_MAX; // a served model's streaming index, its load state logged
        std::atomic<int> g_applied{0};
        std::map<uint32_t, std::vector<uint8_t>> g_original; // guest address -> the bytes before the first change
        std::vector<std::string> g_report;                     // applied.txt

        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, g_ram + (a & kRamMask), 4);
            return v;
        }
        // A pointer inside the GTAG image: still image-relative after FUN_00324A50 (the header's +0x3C reads 0x3B64C8
        // on the device), absolute once the game has fixed it up; either way the guest address
        uint32_t g_base = 0, g_streamInfo = 0;
        uint32_t g_relativeBelow = kImageSize; // [lcs] 0: LCS's tables are taken after relocation (absolute pointers)
        uint32_t g_scratchLo = 0, g_scratchHi = 0; // [crossover] guest scratch: its pointers are absolute (VCS too)
        uint32_t ptr(uint32_t a)
        {
            const uint32_t v = rd32(a);
            if (v >= g_scratchLo && v < g_scratchHi)
                return v;
            return v < g_relativeBelow ? g_base + v : v & kRamMask;
        }
        float rdf(uint32_t a)
        {
            float v = 0;
            std::memcpy(&v, g_ram + (a & kRamMask), 4);
            return v;
        }
        // every write goes through here: the first write to an address keeps its original bytes
        void keep(uint32_t a, uint32_t n)
        {
            a &= kRamMask;
            for (uint32_t i = 0; i < n; i += 4)
                if (!g_original.count(a + i))
                    g_original[a + i] = std::vector<uint8_t>(g_ram + a + i, g_ram + a + i + 4);
        }
        void wr32(uint32_t a, uint32_t v)
        {
            keep(a, 4);
            std::memcpy(g_ram + (a & kRamMask), &v, 4);
        }
        void wrf(uint32_t a, float v)
        {
            keep(a, 4);
            std::memcpy(g_ram + (a & kRamMask), &v, 4);
        }
        void wr8(uint32_t a, uint8_t v)
        {
            keep(a & ~3u, 4);
            g_ram[a & kRamMask] = v;
        }

        void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
        void report(const char *fmt, ...)
        {
            char line[512];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(line, sizeof(line), fmt, args);
            va_end(args);
            std::fprintf(stderr, "[mods] %s\n", line);
            g_report.emplace_back(line);
        }

        fs::path dataDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path(); }
        fs::path modsDir() { return dataDir() / "mods"; }

        // ---- the directives -------------------------------------------------------------------------------------------

        struct Directive
        {
            std::string kind, mod;                    // handling / weapon / stream
            std::map<std::string, std::string> keys;  // id=, type=, index=, copy=, file=
            std::vector<std::pair<std::string, std::string>> fields;
        };

        std::vector<Directive> readActive(const fs::path &path)
        {
            std::vector<Directive> out;
            std::ifstream in(path);
            std::string line, mod = "?";
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
                d.kind = kind;
                d.mod = mod;
                std::string tok;
                std::vector<std::string> rest;
                while (s >> tok)
                {
                    const size_t eq = tok.find('=');
                    if (eq != std::string::npos && rest.empty())
                        d.keys[tok.substr(0, eq)] = tok.substr(eq + 1);
                    else
                        rest.push_back(tok);
                }
                for (size_t i = 0; i + 1 < rest.size(); i += 2)
                    d.fields.emplace_back(rest[i], rest[i + 1]);
                out.push_back(std::move(d));
            }
            return out;
        }

        bool number(const std::string &s, double &v)
        {
            char *end = nullptr;
            v = std::strtod(s.c_str(), &end);
            return end && *end == '\0' && std::isfinite(v);
        }

        // ---- GTAG ------------------------------------------------------------------------------------------------------

        struct Gtag
        {
            uint32_t base = 0, modelInfos = 0, streamInfo = 0;
            uint32_t extent = kImageSize; // [lcs] LCS: the game's RAM above its code (0x100000..0x2000000)
            uint32_t modelCount = kNumModelInfos;
            uint32_t weaponTables[2] = {0, 0};
            uint32_t handlingRecords = 0, handlingCount = 0; // [lcs] cHandlingDataMgr's records (Profile::handlingId)
            uint32_t scratchLo = 0, scratchHi = 0; // [crossover] guest scratch holding new model infos / records
            bool inImage(uint32_t p) const
            {
                p &= kRamMask;
                return (p >= base && p < base + extent) || (p >= scratchLo && p < scratchHi);
            }
        };

        bool findGtag(uint32_t header, uint32_t image, Gtag &g)
        {
            static int logged = 0;
            if (logged++ < 3)
                std::fprintf(stderr, "[mods] relocated: header 0x%08x (%08x %08x %08x) image 0x%08x (%08x .. +0x38 %08x +0x3c %08x)\n",
                             header, rd32(header), rd32(header + 4), rd32(header + 8), image, rd32(image), rd32(image + 0x38),
                             rd32(image + 0x3C));
            for (uint32_t cand : {image & kRamMask, header & kRamMask, (image & kRamMask) - 0x20u})
            {
                if (cand == 0 || cand + kImageSize > 0x02000000u)
                    continue;
                if (rd32(cand + 0x38) != kNumModelInfos)
                    continue;
                g.base = g_base = cand;
                g.modelInfos = ptr(cand + 0x3C);
                if (!g.inImage(g.modelInfos))
                    continue;
                const uint32_t streaming = ptr(cand + 0x7C);
                g.streamInfo = g.inImage(streaming) ? ptr(streaming + 200) : 0;
                if (!g.inImage(g.streamInfo))
                    g.streamInfo = 0;
                const uint32_t pair = ptr(cand + 0xC0);
                for (int i = 0; i < 2 && g.inImage(pair); ++i)
                {
                    // a table checks out when its pistol record (type 19) holds the stock values: instant hit, clip 17,
                    // damage 25, model 346
                    const uint32_t t = ptr(pair + i * 4u), pistol = t + 19u * kWeaponRecord;
                    if (g.inImage(t) && rd32(pistol + 4) == 1u && rd32(pistol + 0x14) == 17u && rd32(pistol + 0x18) == 25u &&
                        rd32(pistol + 0x60) == 346u)
                        g.weaponTables[i] = t;
                }
                return true;
            }
            return false;
        }

        uint32_t vehicleModelInfo(const Gtag &g, int id)
        {
            if (static_cast<uint32_t>(id) >= g.modelCount || id < 0)
                return 0;
            // [crossover] a new model id past the game's vehicles (its model info in guest scratch: vcs_crossover.cpp)
            if (const uint32_t mi = rd32(g.modelInfos + static_cast<uint32_t>(id) * 4u) & kRamMask;
                g.scratchHi && mi >= g.scratchLo && mi < g.scratchHi && g_ram[mi + 0x10] == 6)
                return mi;
            if (id < P().vehicleFirst || id > P().vehicleLast)
                return 0;
            const uint32_t mi = ptr(g.modelInfos + static_cast<uint32_t>(id) * 4u);
            if (!g.inImage(mi) || g_ram[mi + 0x10] != 6)
                return 0;
            return mi;
        }

        std::string text(uint32_t a, size_t n)
        {
            std::string s(reinterpret_cast<const char *>(g_ram + (a & kRamMask)), n);
            s.resize(std::strlen(s.c_str()));
            return s;
        }

        // ---- handling --------------------------------------------------------------------------------------------------
        //
        // Record offsets (docs/ANDROID.md "Mods": the field map and how each was confirmed). Values come in PC
        // handling.cfg units (any of the GTA3 / VC / SA dialects, parsed by the launcher) and are converted here.

        const HandlingLayout &H() { return *P().handling; }

        struct Range
        {
            const char *name;
            double lo, hi;
        };
        // accepted range per PC field; a value outside is clamped (and said so)
        constexpr Range kHandling[] = {
            {"mass", 50, 50000},        {"turnmass", 10, 500000},   {"dimx", 0.2, 30},         {"dimy", 0.2, 30},
            {"dimz", 0.2, 30},          {"comx", -10, 10},          {"comy", -10, 10},         {"comz", -10, 10},
            {"submerged", 10, 120},     {"tractionmult", 0.1, 5},   {"tractionloss", 0, 2},    {"tractionbias", 0, 1},
            {"gears", 1, 5},            {"maxspeed", 10, 400},      {"accel", 0.1, 100},       {"brake", 0.01, 100},
            {"brakebias", 0, 1},        {"steer", 5, 80},           {"suspforce", 0.1, 10},    {"suspdamp", 0, 5},
            {"suspupper", -2, 2},       {"susplower", -2, 2},       {"suspbias", 0, 1},        {"antidive", 0, 2},
            {"seatoffset", -2, 2},      {"value", 0, 10000000},
            // [lcs] SA's columns that LCS's record keeps (VCS: reported as not mapped)
            {"drag", 0.1, 30},          {"inertia", 1, 200},        {"collision", 0, 5},
            // [lcs] ABS (a byte, 0 in every stock record), the light types (bytes +0xDC front / +0xDD rear: 0..3 as
            // GTA III / VC / SA's columns; LCS's own records hold 0..3)
            {"abs", 0, 1},              {"frontlights", 0, 3},      {"rearlights", 0, 3},
        };

        const Range *handlingRange(const std::string &f)
        {
            for (const Range &r : kHandling)
                if (f == r.name)
                    return &r;
            return nullptr;
        }

        // VC's cTransmission::InitGearRatios, which reproduces the gear table (+0x00..+0x47: 6 x {max velocity, shift
        // up, shift down}) of all 111 stock records from +0x50 / +0x58 / +0x5E to within 2e-7
        void writeGears(uint32_t rec, float maxVel, int gears, float reverse)
        {
            float g[6][3] = {};
            const float base = 0.5f * maxVel / static_cast<float>(gears);
            const float step = (maxVel - base) / static_cast<float>(gears);
            for (int i = 1; i <= gears; ++i)
            {
                g[i][0] = base + static_cast<float>(i) * step;
                const float prev = i == 1 ? 0.0f : g[i - 1][0];
                const float diff = g[i][0] - prev;
                if (i >= gears)
                    g[i][1] = maxVel;
                else
                {
                    g[i + 1][2] = prev + diff * 0.42f;
                    g[i][1] = prev + diff * 0.6667f;
                }
            }
            g[0][0] = reverse;
            g[0][1] = -0.01f;
            g[0][2] = reverse;
            g[1][2] = -0.01f;
            for (int i = 0; i < 6; ++i)
                for (int k = 0; k < 3; ++k)
                    wrf(rec + H().gears + static_cast<uint32_t>(i * 12 + k * 4), g[i][k]);
        }

        // the vehicle model info's handling record: VCS a pointer in the model info; [lcs] LCS an id into the manager's
        // records (checked against the record's own id)
        std::map<uint32_t, uint32_t> g_handlingOverride; // [crossover] model info -> its own record (lcs_crossover.cpp)

        uint32_t handlingRecord(const Gtag &g, uint32_t mi)
        {
            if (auto it = g_handlingOverride.find(mi & kRamMask); it != g_handlingOverride.end())
                return it->second;
            if (P().handlingPtr)
                return ptr(mi + P().handlingPtr);
            int16_t id = 0;
            std::memcpy(&id, g_ram + ((mi + P().handlingId) & kRamMask), 2);
            if (!g.handlingRecords || id < 0 || static_cast<uint32_t>(id) >= g.handlingCount)
                return 0;
            const uint32_t rec = g.handlingRecords + static_cast<uint32_t>(id) * H().size;
            return rd32(rec + H().ident) == static_cast<uint32_t>(id) ? rec : 0;
        }

        void applyHandling(const Gtag &g, const Directive &d)
        {
            const HandlingLayout &h = H();
            const int id = std::atoi(d.keys.count("id") ? d.keys.at("id").c_str() : "0");
            const uint32_t mi = vehicleModelInfo(g, id);
            const uint32_t rec = mi ? handlingRecord(g, mi) : 0;
            if (mi && !P().handlingPtr && !g.handlingRecords)
            {
                report("%s: handling for model %d skipped: the game's handling records were not found", d.mod.c_str(), id);
                return;
            }
            if (!mi || !rec || !g.inImage(rec))
            {
                report("%s: handling for model %d skipped: not a vehicle slot", d.mod.c_str(), id);
                return;
            }
            const uint8_t drive0 = g_ram[rec + h.drive], gears0 = g_ram[rec + h.numGears];
            if ((drive0 != 'F' && drive0 != 'R' && drive0 != '4') || gears0 < 1 || gears0 > 5 || rdf(rec + h.mass) <= 0.0f)
            {
                report("%s: handling for model %d skipped: its record does not look like a handling record", d.mod.c_str(), id);
                return;
            }
            const std::string slot = text(mi + P().modelName, 8);
            std::map<std::string, double> v;
            char drive = 0, engine = 0;
            bool hasFlags = false;
            uint32_t flags = 0, flagMask = 0x07FFFFFFu; // VC's 27 bits, the manager's mask narrows it (III: 16)
            for (const auto &[name, value] : d.fields)
            {
                if (name == "flags" || name == "flagmask") // hex words in VC's bit layout (tools/mods.py VC_FLAGS)
                {
                    char *end = nullptr;
                    const unsigned long x = std::strtoul(value.c_str(), &end, 16);
                    if (!end || *end || h.flags == kNone)
                    {
                        report("%s -> %s: field %s=%s not mapped, skipped", d.mod.c_str(), slot.c_str(), name.c_str(), value.c_str());
                        continue;
                    }
                    (name == "flags" ? (hasFlags = true, flags) : flagMask) = static_cast<uint32_t>(x);
                    continue;
                }
                if (name == "drive" || name == "engine")
                {
                    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(value.empty() ? 0 : value[0])));
                    if (name == "drive" && (c == 'F' || c == 'R' || c == '4'))
                        drive = c;
                    else if (name == "engine" && (c == 'P' || c == 'D' || c == 'E'))
                        engine = c;
                    else
                        report("%s -> %s: %s '%s' not understood, kept", d.mod.c_str(), slot.c_str(), name.c_str(), value.c_str());
                    continue;
                }
                double x;
                const Range *r = handlingRange(name);
                const bool absent = (name == "drag" && h.drag == kNone) || (name == "inertia" && h.inertia == kNone) ||
                                    (name == "collision" && h.collision == kNone) || (name == "abs" && h.abs == kNone) ||
                                    ((name == "frontlights" || name == "rearlights") && h.lights == kNone);
                if (!r || absent || !number(value, x))
                {
                    report("%s -> %s: field %s=%s not mapped, skipped", d.mod.c_str(), slot.c_str(), name.c_str(), value.c_str());
                    continue;
                }
                const double c = std::clamp(x, r->lo, r->hi);
                if (c != x)
                    report("%s -> %s: %s %g outside %g..%g, clamped to %g", d.mod.c_str(), slot.c_str(), name.c_str(), x, r->lo,
                           r->hi, c);
                v[name] = c;
            }
            auto has = [&](const char *n) { return v.count(n) != 0; };
            int changed = 0;
            auto setf = [&](uint32_t off, float value, const char *what) {
                report("%s -> %s (%d): %s %g (was %g)", d.mod.c_str(), slot.c_str(), id, what, value, rdf(rec + off));
                wrf(rec + off, value);
                ++changed;
            };

            const float mass0 = rdf(rec + h.mass);
            const float mass = has("mass") ? static_cast<float>(v["mass"]) : mass0;
            if (has("mass"))
            {
                setf(h.mass, mass, "mass");
                if (h.invMass != kNone) // [lcs] the record keeps 1 / mass too
                    setf(h.invMass, 1.0f / mass, "1 / mass (derived)");
            }
            // turn mass: SA gives it; GTA3 / VC give the dimensions (VC: (x^2 + y^2) * mass / 12, which reproduces the
            // stock ADMIRAL 4056.4 and POLICEM 4351.7 from round dimensions); a new mass alone scales it
            if (has("turnmass"))
                setf(h.turnMass, static_cast<float>(v["turnmass"]), "turn mass");
            else if (has("dimx") && has("dimy"))
                setf(h.turnMass, std::max(10.0f, static_cast<float>((v["dimx"] * v["dimx"] + v["dimy"] * v["dimy"]) * mass / 12.0)),
                     "turn mass (from the dimensions)");
            else if (has("mass"))
                setf(h.turnMass, rdf(rec + h.turnMass) * mass / mass0, "turn mass (scaled with the mass)");
            if (has("submerged"))
            {
                const uint32_t sub = static_cast<uint32_t>(std::lround(v["submerged"]));
                report("%s -> %s (%d): percent submerged %u (was %u)", d.mod.c_str(), slot.c_str(), id, sub, rd32(rec + h.submerged));
                wr32(rec + h.submerged, sub);
                ++changed;
            }
            if (has("mass") || has("submerged")) // buoyancy = 100 / percent submerged * 0.008 * mass (all 111 / 81 records)
                setf(h.buoyancy, 100.0f / static_cast<float>(rd32(rec + h.submerged)) * 0.008f * mass, "buoyancy (derived)");
            // [lcs] collision damage multiplier, stored as cfg x 2000 / mass: a new value, or the old one for a new mass
            if (h.collision != kNone && has("collision"))
                setf(h.collision, static_cast<float>(v["collision"] * 2000.0 / mass), "collision damage multiplier");
            else if (h.collision != kNone && has("mass"))
                setf(h.collision, rdf(rec + h.collision) * mass0 / mass, "collision damage multiplier (kept for the new mass)");
            const char *com[] = {"comx", "comy", "comz"};
            for (int i = 0; i < 3; ++i)
                if (has(com[i]))
                    setf(h.centreOfMass + static_cast<uint32_t>(i) * 4u, static_cast<float>(v[com[i]]), com[i]);
            if (has("tractionmult"))
                setf(h.tractionMult, static_cast<float>(v["tractionmult"]), "traction multiplier");
            if (has("tractionloss"))
                setf(h.tractionLoss, static_cast<float>(v["tractionloss"]), "traction loss");
            if (has("tractionbias"))
                setf(h.tractionBias, static_cast<float>(v["tractionbias"]), "traction bias");
            if (has("drag")) // [lcs] SA's drag multiplier, as is (stock 0.2..12, SA-like values)
                setf(h.drag, static_cast<float>(v["drag"]), "drag multiplier");

            // the transmission
            if (drive && drive != static_cast<char>(drive0))
            {
                report("%s -> %s (%d): drive type %c (was %c)", d.mod.c_str(), slot.c_str(), id, drive, drive0);
                wr8(rec + h.drive, static_cast<uint8_t>(drive));
                ++changed;
            }
            if (engine)
            {
                report("%s -> %s (%d): engine type %c (was %c)", d.mod.c_str(), slot.c_str(), id, engine, g_ram[rec + h.engine]);
                wr8(rec + h.engine, static_cast<uint8_t>(engine));
                ++changed;
            }
            const int gears = has("gears") ? static_cast<int>(std::lround(v["gears"])) : gears0;
            if (gears != gears0)
            {
                report("%s -> %s (%d): gears %d (was %d)", d.mod.c_str(), slot.c_str(), id, gears, gears0);
                wr8(rec + h.numGears, static_cast<uint8_t>(gears));
                ++changed;
            }
            const char driveNow = static_cast<char>(g_ram[rec + h.drive]);
            if (has("accel")) // cfg / 2500 (per frame^2), then / 4 for four-wheel drive, / 2 for the others (re3)
                setf(h.accel, static_cast<float>(v["accel"] / 2500.0 / (driveNow == '4' ? 4.0 : 2.0)), "engine acceleration");
            if (has("inertia")) // [lcs] SA's engine inertia, as is (stock 4.5..150)
                setf(h.inertia, static_cast<float>(v["inertia"]), "engine inertia");
            bool gearsChanged = gears != gears0;
            if (has("maxspeed"))
            {
                // VCS: +0xB8 keeps the cfg number. The working velocities (max, cruise = max / 1.2) come from a drag loop
                // whose inputs are not all in the record: they are scaled with the cfg speed (approximate). [lcs] LCS
                // keeps no cfg speed: the slot's own top speed in km/h (max velocity x 180) stands in for it, so the
                // value is the new top speed (max velocity = cfg / 180).
                const float cfg = static_cast<float>(v["maxspeed"]);
                const float cfg0 = h.maxSpeedCfg != kNone ? rdf(rec + h.maxSpeedCfg) : rdf(rec + h.maxVel) * 180.0f;
                if (cfg0 > 0.0f)
                {
                    const float k = cfg / cfg0;
                    if (h.maxSpeedCfg != kNone)
                        setf(h.maxSpeedCfg, cfg, "max speed (cfg)");
                    setf(h.maxVel, rdf(rec + h.maxVel) * k, "max velocity (scaled)");
                    setf(h.cruiseVel, rdf(rec + h.cruiseVel) * k, "cruise velocity (scaled)");
                    gearsChanged = true;
                }
            }
            if (gearsChanged)
            {
                writeGears(rec, rdf(rec + h.maxVel), gears, rdf(rec + h.reverseVel));
                report("%s -> %s (%d): gear table rebuilt (%d gears, top %g)", d.mod.c_str(), slot.c_str(), id, gears, rdf(rec + h.maxVel));
            }
            if (has("brake")) // cfg / 2500: every stock record x 2500 is a round cfg number (4.0, 9.1, 13.1...)
                setf(h.brake, static_cast<float>(v["brake"] / 2500.0), "brake deceleration");
            if (has("brakebias"))
                setf(h.brakeBias, static_cast<float>(v["brakebias"]), "brake bias");
            if (has("steer"))
                setf(h.steer, static_cast<float>(v["steer"]), "steering lock");
            const std::pair<const char *, uint32_t> plain[] = {
                {"suspforce", h.suspForce}, {"suspdamp", h.suspDamp}, {"suspupper", h.suspUpper}, {"susplower", h.suspLower},
                {"suspbias", h.suspBias},   {"antidive", h.antiDive}, {"seatoffset", h.seatOffset},
            };
            for (const auto &[name, off] : plain)
                if (has(name))
                    setf(off, static_cast<float>(v[name]), name);
            if (has("value"))
            {
                const uint32_t money = static_cast<uint32_t>(std::lround(v["value"]));
                report("%s -> %s (%d): value %u (was %u)", d.mod.c_str(), slot.c_str(), id, money, rd32(rec + h.value));
                wr32(rec + h.value, money);
                ++changed;
            }
            // [lcs] bytes: ABS, the front / rear light types
            const std::pair<const char *, uint32_t> bytes[] = {
                {"abs", h.abs}, {"frontlights", h.lights}, {"rearlights", h.lights == kNone ? kNone : h.lights + 1}};
            for (const auto &[name, off] : bytes)
                if (has(name) && off != kNone)
                {
                    const uint8_t b = static_cast<uint8_t>(std::lround(v[name]));
                    report("%s -> %s (%d): %s %u (was %u)", d.mod.c_str(), slot.c_str(), id, name, b, g_ram[(rec + off) & kRamMask]);
                    wr8(rec + off, b);
                    ++changed;
                }
            // [lcs] the handling flags: LCS's record keeps VC's bit layout (bikes 0x10000, helis 0x20000, boats 0x80000,
            // vans 0x20, the BF Injection 0x7000000 ... read from all 81 records in a run). The mod decides the bits of
            // its mask; the vehicle-kind bits (bike / heli / plane / boat / sit in boat: 0x8F0000) stay the slot's, as do
            // the bits above VC's (0x8000000, 0x10000000: LCS's own)
            if (hasFlags && h.flags != kNone)
            {
                const uint32_t keepMask = ~(flagMask & 0x07FFFFFFu) | 0x008F0000u;
                const uint32_t old = rd32(rec + h.flags), now = (old & keepMask) | (flags & ~keepMask);
                report("%s -> %s (%d): handling flags 0x%08x (was 0x%08x; the mod's 0x%x, its mask 0x%x)", d.mod.c_str(), slot.c_str(),
                       id, now, old, flags, flagMask);
                wr32(rec + h.flags, now);
                ++changed;
            }
            report("%s -> %s (%d): %d handling values written to record 0x%08x", d.mod.c_str(), slot.c_str(), id, changed, rec);
        }

        // ---- weapons ---------------------------------------------------------------------------------------------------
        // Record (from its first word): +4 fire type, +8 range, +0xC firing rate, +0x10 reload, +0x14 clip, +0x18 damage,
        // +0x1C speed, +0x20 radius, +0x24 lifespan, +0x28 spread - weapon.dat's columns C..K. Fire type, offsets,
        // animation timings, models and slot are left alone.

        struct WeaponField
        {
            const char *name;
            uint32_t off;
            bool integer;
            double lo, hi;
        };
        constexpr WeaponField kWeapon[] = {
            {"range", 0x08, false, 0.5, 500},  {"rate", 0x0C, true, 1, 10000},     {"reload", 0x10, true, 0, 10000},
            {"clip", 0x14, true, 1, 1000},     {"damage", 0x18, true, 0, 10000},   {"speed", 0x1C, false, 0.01, 10},
            {"radius", 0x20, false, 0.01, 50}, {"lifespan", 0x24, false, 1, 20000}, {"spread", 0x28, false, 0, 50},
            // the fire offset (weapon.dat columns L-N; SA: the muzzle flash x / y / z): where the shot and the muzzle
            // flash leave the gun. [lcs] LCS +0x30 / +0x34 / +0x38 (the pistol 0.3 0 0.09, read in a run; VCS by the same
            // 4-byte shift, not checked there). Any sign (no -1 "unused" rule)
            {"firex", 0x34, false, -3, 3},     {"firey", 0x38, false, -3, 3},     {"firez", 0x3C, false, -3, 3},
        };

        void applyWeapon(const Gtag &g, const Directive &d)
        {
            const int type = std::atoi(d.keys.count("type") ? d.keys.at("type").c_str() : "-1");
            // [lcs] LCS: types up to the camera (36, lcs_trainer.cpp kWeapons); the table's length is not confirmed
            const int last = game::isLcs() ? 36 : kWeaponTypes - 1;
            if (type < 1 || type > last)
            {
                report("%s: weapon type %d skipped: not a weapon slot (1..%d)", d.mod.c_str(), type, last);
                return;
            }
            if (!g.weaponTables[0] && !g.weaponTables[1])
            {
                report("%s: weapon type %d skipped: the weapon tables were not found", d.mod.c_str(), type);
                return;
            }
            int changed = 0;
            for (const auto &[name, value] : d.fields)
            {
                const WeaponField *f = nullptr;
                for (const WeaponField &w : kWeapon)
                    if (name == w.name)
                        f = &w;
                double x;
                if (!f || !number(value, x))
                {
                    report("%s -> weapon %d: field %s=%s not mapped, skipped", d.mod.c_str(), type, name.c_str(), value.c_str());
                    continue;
                }
                const double c = std::clamp(x, f->lo, f->hi);
                for (int t = 0; t < 2; ++t)
                {
                    if (!g.weaponTables[t])
                        continue;
                    const uint32_t a = g.weaponTables[t] + static_cast<uint32_t>(type) * kWeaponRecord + f->off - P().weaponShift;
                    const bool offset = f->off >= 0x34 && f->off <= 0x3C;
                    if (!f->integer && !offset && rdf(a) < 0.0f) // -1: the field is not used by this weapon's fire type
                    {
                        if (t == 0)
                            report("%s -> weapon %d: %s unused by this weapon (stock -1), skipped", d.mod.c_str(), type, name.c_str());
                        continue;
                    }
                    if (t == 0)
                    {
                        if (f->integer)
                            report("%s -> weapon %d: %s %d (was %d)%s", d.mod.c_str(), type, name.c_str(), static_cast<int>(std::lround(c)),
                                   static_cast<int>(rd32(a)), c != x ? " (clamped)" : "");
                        else
                            report("%s -> weapon %d: %s %g (was %g)%s", d.mod.c_str(), type, name.c_str(), c, rdf(a), c != x ? " (clamped)" : "");
                        ++changed;
                    }
                    if (f->integer)
                        wr32(a, static_cast<uint32_t>(std::lround(c)));
                    else
                        wrf(a, static_cast<float>(c));
                }
            }
            report("%s -> weapon %d: %d values written (%s)", d.mod.c_str(), type, changed,
                   g.weaponTables[0] && g.weaponTables[1] ? "both tables" : "one table");
        }

        // ---- streaming redirection -------------------------------------------------------------------------------------
        // A redirected entry's sectors point past the end of the disc, into a range this disc wrapper serves from host
        // memory; every other read goes to the player's disc as before.

        constexpr uint32_t kFileBlob = UINT32_MAX; // a blob holding a whole replaced disc file, not a streaming entry

        class ModDisc final : public IPS2DiscImage
        {
        public:
            explicit ModDisc(IPS2DiscImage *inner) : inner_(inner), first_(inner->totalSectors() + 0x100u) {}
            bool findFile(std::string_view p, uint32_t &lsn, uint32_t &size, bool &dir) const override
            {
                return inner_->findFile(p, lsn, size, dir);
            }
            uint32_t totalSectors() const override { return inner_->totalSectors(); }
            bool listDirectory(std::string_view p, std::vector<DirEntry> &out) const override { return inner_->listDirectory(p, out); }
            bool readBytes(uint64_t offset, size_t count, void *destination) const override
            {
                const uint64_t start = static_cast<uint64_t>(first_) * 2048u;
                if (offset + count <= start)
                    return inner_->readBytes(offset, count, destination);
                uint8_t *out = static_cast<uint8_t *>(destination);
                if (offset < start) // (a read across the disc's end: not expected)
                {
                    const size_t head = static_cast<size_t>(start - offset);
                    if (!inner_->readBytes(offset, head, out))
                        return false;
                    offset += head, out += head, count -= head;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                const uint64_t rel = offset - start;
                std::memset(out, 0, count);
                for (const Blob &b : blobs_)
                {
                    const uint64_t bs = static_cast<uint64_t>(b.sector) * 2048u, be = bs + b.bytes.size();
                    const uint64_t s = std::max(rel, bs), e = std::min<uint64_t>(rel + count, be);
                    if (s < e)
                    {
                        std::memcpy(out + (s - rel), b.bytes.data() + (s - bs), static_cast<size_t>(e - s));
                        if (served_++ < 64)
                        {
                            if (b.index == kFileBlob)
                                std::fprintf(stderr, "[mods] served %llu bytes of a replaced disc file from the mod (host memory)\n",
                                             static_cast<unsigned long long>(e - s));
                            else
                                std::fprintf(stderr, "[mods] served %llu bytes of streaming index %u from the mod (host memory)\n",
                                             static_cast<unsigned long long>(e - s), b.index);
                        }
                    }
                }
                return true;
            }
            // the sector (absolute LSN) where the data now lives; the bytes are padded to whole sectors
            uint32_t add(uint32_t index, std::vector<uint8_t> bytes)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                bytes.resize((bytes.size() + 2047u) / 2048u * 2048u, 0);
                Blob b{index, next_, std::move(bytes)};
                next_ += static_cast<uint32_t>(b.bytes.size() / 2048u);
                const uint32_t lsn = first_ + b.sector;
                blobs_.push_back(std::move(b));
                return lsn;
            }
            void clear()
            {
                std::lock_guard<std::mutex> lock(mutex_);
                blobs_.clear();
                next_ = 0;
            }
            IPS2DiscImage *inner() const { return inner_; }
            uint32_t endSector() const
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return first_ + next_;
            }

        private:
            struct Blob
            {
                uint32_t index, sector;
                std::vector<uint8_t> bytes;
            };
            IPS2DiscImage *inner_;
            uint32_t first_;
            uint32_t next_ = 0;
            std::vector<Blob> blobs_;
            mutable std::mutex mutex_;
            mutable std::atomic<int> served_{0};
        };

        std::unique_ptr<ModDisc> g_disc;

        bool imgLsn(IPS2DiscImage *disc, uint32_t &lsn)
        {
            uint32_t size = 0;
            bool dir = false;
            // VCS: GTA3PS2.IMG at the disc's root; [lcs] LCS: MODELS\GTA3PS2.IMG (its string 0x3AD710)
            for (const char *p : {"GTA3PS2.IMG", "\\GTA3PS2.IMG;1", "cdrom0:\\GTA3PS2.IMG;1", "MODELS/GTA3PS2.IMG",
                                  "\\MODELS\\GTA3PS2.IMG;1"})
                if (disc->findFile(p, lsn, size, dir) && !dir)
                    return true;
            return false;
        }

        void serve(const Gtag &g, IPS2DiscImage *disc, uint32_t img, uint32_t index, std::vector<uint8_t> bytes);

        void applyStream(const Gtag &g, const Directive &d)
        {
            const uint32_t index = static_cast<uint32_t>(std::atoi(d.keys.count("index") ? d.keys.at("index").c_str() : "-1"));
            if (!g.streamInfo || index >= P().streamIndices)
            {
                report("%s: stream entry %d skipped: no streaming directory or index out of range", d.mod.c_str(), static_cast<int>(index));
                return;
            }
            IPS2DiscImage *disc = g_disc ? g_disc->inner() : ps2_disc::image();
            uint32_t img = 0;
            if (!disc || !imgLsn(disc, img))
            {
                report("%s: stream entry %u skipped: GTA3PS2.IMG not found on the disc", d.mod.c_str(), index);
                return;
            }
            const uint32_t entry = g.streamInfo + index * P().streamEntry;
            std::vector<uint8_t> bytes;
            std::string from;
            if (d.keys.count("copy"))
            {
                const uint32_t src = static_cast<uint32_t>(std::atoi(d.keys.at("copy").c_str()));
                const uint32_t se = g.streamInfo + src * P().streamEntry;
                const uint32_t sector = rd32(se + 0x10), count = rd32(se + 0x14);
                if (src >= P().streamIndices || count == 0 || count > 1024)
                {
                    report("%s: stream entry %u skipped: source %u has no data", d.mod.c_str(), index, src);
                    return;
                }
                bytes.resize(static_cast<size_t>(count) * 2048u);
                if (!disc->readBytes((static_cast<uint64_t>(img) + sector) * 2048u, bytes.size(), bytes.data()))
                {
                    report("%s: stream entry %u skipped: reading source %u failed", d.mod.c_str(), index, src);
                    return;
                }
                from = "the disc's entry " + std::to_string(src);
            }
            else if (d.keys.count("file"))
            {
                const std::string rel = d.keys.at("file");
                if (rel.find("..") != std::string::npos || rel.empty() || rel[0] == '/')
                {
                    report("%s: stream entry %u skipped: bad file name", d.mod.c_str(), index);
                    return;
                }
                std::ifstream in(modsDir() / rel, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (bytes.empty() || bytes.size() > (2u << 20))
                {
                    report("%s: stream entry %u skipped: %s missing, empty or over 2 MB", d.mod.c_str(), index, rel.c_str());
                    return;
                }
                from = rel;
            }
            else
            {
                report("%s: stream entry %u skipped: no copy= or file=", d.mod.c_str(), index);
                return;
            }
            const uint32_t oldSector = rd32(entry + 0x10), oldCount = rd32(entry + 0x14);
            serve(g, disc, img, index, std::move(bytes));
            if (index < P().textureListBase)
                g_watchIndex = index;
            const uint32_t count = rd32(entry + 0x14), lsn = rd32(entry + 0x10) + img;
            report("%s: streaming index %u now served from %s: sector 0x%x x %u (was 0x%x x %u)", d.mod.c_str(), index, from.c_str(),
                   lsn - img, count, oldSector, oldCount);
        }

        // the stock bytes of a streaming entry, from the player's disc
        // index -> the stock {sector, count} of an entry served from a mod: a second mod on the same slot (two packs
        // with an M4) converts from the stock entry too, not from the first mod's served range (which read nothing:
        // "its stock model / texture list could not be read")
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_stockEntry;

        bool readEntry(const Gtag &g, IPS2DiscImage *disc, uint32_t img, uint32_t index, std::vector<uint8_t> &bytes)
        {
            const uint32_t se = g.streamInfo + index * P().streamEntry;
            uint32_t sector = rd32(se + 0x10), count = rd32(se + 0x14);
            if (auto it = g_stockEntry.find(index); it != g_stockEntry.end())
                sector = it->second.first, count = it->second.second;
            if (index >= P().streamIndices || count == 0 || count > 1024)
                return false;
            bytes.resize(static_cast<size_t>(count) * 2048u);
            return disc->readBytes((static_cast<uint64_t>(img) + sector) * 2048u, bytes.size(), bytes.data());
        }

        // The game reads GTA3PS2.IMG through its own file layer (FUN_00220EC8: handle table 0x5058A8, 32 x 0x14 bytes:
        // +4 first sector, +8 size in bytes, +0xC position), and a seek is clamped to the file's size. A redirected
        // entry lies past the end of the disc, so the IMG's size is raised to reach it: in the disc index the handles are
        // opened from (0x5038A8, 16-byte entries, count at 0x486E8C) and in a handle already open.
        void extendImg(uint32_t img, uint32_t endSector)
        {
            const uint32_t bytes = (endSector - img) * 2048u;
            const uint32_t count = std::min<uint32_t>(rd32(0x00486E8Cu), 4096u);
            int patched = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = 0x005038A8u + i * 16u;
                for (uint32_t k = 4; k <= 8; k += 4)
                    if (rd32(e + k) == img && rd32(e + k + 4) < bytes)
                        wr32(e + k + 4, bytes), ++patched;
            }
            for (uint32_t i = 0; i < 32; ++i)
            {
                const uint32_t h = 0x005058A8u + i * 0x14u;
                if (rd32(h) && rd32(h + 4) == img && rd32(h + 8) < bytes)
                    wr32(h + 8, bytes), ++patched;
            }
            static bool logged = false;
            if (!logged)
                std::fprintf(stderr, "[mods] GTA3PS2.IMG (sector 0x%x) made %u bytes long for the served entries (%d places)\n", img,
                             bytes, patched),
                    logged = true;
        }

        void serve(const Gtag &g, IPS2DiscImage *disc, uint32_t img, uint32_t index, std::vector<uint8_t> bytes)
        {
            if (!g_disc)
            {
                g_disc = std::make_unique<ModDisc>(disc);
                ps2_disc::setImage(g_disc.get());
            }
            const uint32_t entry = g.streamInfo + index * P().streamEntry;
            g_stockEntry.emplace(index, std::make_pair(rd32(entry + 0x10), rd32(entry + 0x14))); // the first time only
            const uint32_t count = static_cast<uint32_t>((bytes.size() + 2047u) / 2048u);
            const uint32_t lsn = g_disc->add(index, std::move(bytes));
            wr32(entry + 0x10, lsn - img);
            wr32(entry + 0x14, count);
            if (P().clampedImg) // [lcs] LCS reads the IMG by sector through CDSTREAM: nothing to extend
                extendImg(img, g_disc->endSector());
        }

        // [mods] a model / text file of a mod: the manager's copy under mods/, the player's own file ("keep where it
        // is": an absolute path, or place:<handle> on Android), or one entry of a .zip / .vcm (vcs_modfiles.h)
        bool readModFile(const std::string &value, std::vector<uint8_t> &bytes, std::string &why)
        {
            const files::Location l = files::resolve(value, modsDir());
            if (!files::read(l, bytes, 32u << 20, why))
                return false;
            if (bytes.empty())
                return why = l.shown + " is empty", false;
            return true;
        }

        // The converted-model cache: a model is converted once and its chunks kept next to the mod, in
        // <data>/mods/<mod folder>/converted/<game>-<model id>.{mdl,xet,key,txt} (<mod folder>: the first component of
        // the dff= path; the data folder is the one the runtime uses). The .key file holds what the output depends on:
        // the game, the model id, the converter version (modconv::kVersion), the budgets, and FNV-1a 64 hashes of the
        // .dff, the .txd and the slot's stock model / texture list read from the disc. Any difference -> converted
        // again and the files rewritten; deleting the folder only costs one conversion. .txt: the converter's report.
        uint64_t fnv64(const std::vector<uint8_t> &b)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t c : b)
                h = (h ^ c) * 1099511628211ull;
            return h;
        }

        // [mods] .src (next to .key): "<stamp of the dff>\n<stamp of the txd>\n" + the key, the stamps being the
        // source files' size, modification time and entry name (files::stamp). When both stamps and the key's other
        // lines (game, converter, budgets, stock model) still match, the cache is taken without reading the .dff /
        // .txd at all: a mod kept in its zip, where it is, is not opened at a start that changes nothing.
        class ModelCache
        {
        public:
            ModelCache(const std::string &folder, int id, const modconv::Options &o, const std::vector<uint8_t> &donorMdl,
                       const std::vector<uint8_t> &donorXet, const std::string &tag = std::string())
            {
                const std::string gameName = game::isLcs() ? "lcs" : "vcs";
                if (!folder.empty())
                    dir_ = modsDir() / files::pathOf(folder) / "converted";
                stem_ = gameName + "-" + std::to_string(id);
                if (!tag.empty()) // [crossover] several models for one slot (a stories pack's vehicles, bound at run time)
                    stem_ += "-" + tag;
                char k[256];
                std::snprintf(k, sizeof k, "game %s\nmodel %d\nconverter %d\ntris %d\ntex %d\nscale %g\n", gameName.c_str(), id,
                              modconv::kVersion, o.maxTriangles, o.maxTexture, o.scale);
                head_ = k;
                if (!o.part.empty()) // (only when set: the keys of caches made before part= stay the same)
                    head_ += "part " + o.part + "\n";
                std::snprintf(k, sizeof k, "stock model %016llx %zu\nstock textures %016llx %zu\n",
                              static_cast<unsigned long long>(fnv64(donorMdl)), donorMdl.size(),
                              static_cast<unsigned long long>(fnv64(donorXet)), donorXet.size());
                tail_ = k;
            }
            std::string where() const { return (dir_ / stem_).string() + ".*"; }
            // the key for these sources (the same text as before .src existed: old caches stay valid)
            void sources(const std::vector<uint8_t> &dff, const std::vector<uint8_t> &txd)
            {
                char k[128];
                std::snprintf(k, sizeof k, "dff %016llx %zu\ntxd %016llx %zu\n", static_cast<unsigned long long>(fnv64(dff)),
                              dff.size(), static_cast<unsigned long long>(fnv64(txd)), txd.size());
                key_ = head_ + k + tail_;
            }
            static bool enabled()
            {
                const char *env = std::getenv("VCS_MODS_CACHE"); // 0: convert again (and rewrite the cache)
                return !(env && *env == '0');
            }
            // taken by the sources' stamps alone (no source read)
            bool loadByStamp(const std::string &stamps, modconv::Result &r) const
            {
                if (dir_.empty() || stamps.empty() || !enabled())
                    return false;
                const std::string stored = text(".key");
                if (stored.empty() || text(".src") != stamps + stored)
                    return false;
                // the stored key's source lines with today's game / converter / stock lines: still the same key?
                const size_t a = stored.find("\ndff "), b = stored.find("\nstock model ");
                if (a == std::string::npos || b == std::string::npos || b < a ||
                    head_ + stored.substr(a + 1, b - a) + tail_ != stored)
                    return false;
                r.mdl = read(".mdl");
                r.xet = read(".xet");
                r.report = text(".txt"); // a vehicle's "set" lines (model info values) come back with it
                return !r.mdl.empty() && !r.xet.empty();
            }
            bool load(modconv::Result &r) const
            {
                if (dir_.empty() || !enabled() || text(".key") != key_)
                    return false;
                r.mdl = read(".mdl");
                r.xet = read(".xet");
                r.report = text(".txt"); // a vehicle's "set" lines (model info values) come back with it
                return !r.mdl.empty() && !r.xet.empty();
            }
            // after a full-key hit: the stamps written, so the next start needs no source read
            void stampAgain(const std::string &stamps) const
            {
                std::error_code ec;
                if (dir_.empty())
                    return;
                if (stamps.empty())
                    fs::remove(dir_ / (stem_ + ".src"), ec);
                else if (text(".src") != stamps + key_)
                    write(".src", std::vector<uint8_t>(stamps.begin(), stamps.end()), key_);
            }
            bool save(const modconv::Result &r, const std::string &stamps) const
            {
                if (dir_.empty())
                    return false;
                std::error_code ec;
                fs::create_directories(dir_, ec);
                fs::remove(dir_ / (stem_ + ".src"), ec);
                fs::remove(dir_ / (stem_ + ".key"), ec); // the key last: a half-written cache is never taken
                const bool ok = write(".mdl", r.mdl) && write(".xet", r.xet) &&
                                write(".txt", std::vector<uint8_t>(r.report.begin(), r.report.end())) &&
                                write(".key", std::vector<uint8_t>(key_.begin(), key_.end()));
                if (ok && !stamps.empty())
                    write(".src", std::vector<uint8_t>(stamps.begin(), stamps.end()), key_);
                return ok;
            }

        private:
            std::string text(const char *ext) const
            {
                const std::vector<uint8_t> b = read(ext);
                return std::string(b.begin(), b.end());
            }
            bool write(const char *ext, std::vector<uint8_t> b, const std::string &more) const
            {
                b.insert(b.end(), more.begin(), more.end());
                return write(ext, b);
            }
            std::vector<uint8_t> read(const char *ext) const
            {
                std::ifstream in(dir_ / (stem_ + ext), std::ios::binary);
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            bool write(const char *ext, const std::vector<uint8_t> &b) const
            {
                std::ofstream out(dir_ / (stem_ + ext), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
                return static_cast<bool>(out);
            }
            fs::path dir_;
            std::string stem_, head_, tail_, key_;
        };

        // the converted-model cache's folder: mods/<the first part of a relative dff= path> (the manager's copy, as
        // before), else mods/<mod id> (a file kept where it is)
        std::string cacheFolder(const std::string &dffValue, const std::string &mod)
        {
            const files::Location l = files::resolve(dffValue, fs::path());
            if (l.error.empty() && !l.inPlace)
            {
                const std::string rel = dffValue.substr(0, dffValue.find('!'));
                const size_t slash = rel.find_first_of("/\\");
                if (slash != std::string::npos && slash > 0)
                    return rel.substr(0, slash);
            }
            const bool sane = !mod.empty() && mod != "?" && mod.find_first_of("/\\.:") == std::string::npos;
            return sane ? mod : "";
        }

        // model id=<model id> dff=<file> txd=<file> [tris=<n>] [tex=<n>]: a PC model converted (vcs_modconv.h) and
        // streamed in place of the slot's model and texture list ([lcs] LCS too, in its dialect: Options::lcs; base 0x1324
        // there). The texture list's streaming index is 7534 + the
        // model info's texture slot (+0x1E; checked on the disc: ADMIRAL 0xA7 -> 7701, the pistol 0x125 -> 7827).

        // [lcs] A converted vehicle's model info values ("set" lines of the converter's report, vcs_modconv.h Result):
        //   set position <k> x y z   k = 0 headlights, 1 tail lights, 2 front seat, 3 back seat, 4 exhaust, at model
        //                            info +0x50 + k x 0x10 (read in a run: the Kuruma's 0.645 2.408 -0.186 / 0.821
        //                            -2.438 -0.080 / 0.401 0.149 -0.276 / 0.401 -0.824 -0.321 / 0.552 -2.637 -0.557, the
        //                            bus's back seat 0 0 0, VC's order of CVehicleModelInfo positions)
        //   set wheelscale <s>       model info +0x3C (Kuruma 0.7, Bus 0.9, PCJ-600 0.67: VC's IDE wheel scales), next to
        //                            the wheel model id (+0x40, int16: Kuruma 251)
        // Cars only (class +0x38 == 0: bikes / boats / aircraft keep theirs); a position the slot doesn't use (all zero,
        // e.g. a bus's back seat) stays unused. VCS: its model info layout for these is not located: reported.
        void applyCarSets(uint32_t mi, int id, const Directive &d, const std::string &report_)
        {
            std::istringstream lines(report_);
            int done = 0, seen = 0;
            for (std::string l; std::getline(lines, l);)
            {
                if (l.rfind("set ", 0) != 0)
                    continue;
                ++seen;
                // the class byte: LCS +0x38, VCS +0x54 (0 car, 6 bike, 8 BMX: read in a run, all 111 vehicle model infos)
                const bool lcs = game::isLcs();
                if (g_ram[(mi + (lcs ? 0x38u : 0x54u)) & kRamMask] != 0)
                    continue;
                int k = -1;
                float x = 0, y = 0, z = 0, s = 0;
                if (std::sscanf(l.c_str(), "set position %d %f %f %f", &k, &x, &y, &z) == 4 && k >= 0 && k < 5)
                {
                    // [vcs] VCS keeps SA's dummy order from +0x60, 0x10 apart: headlights, tail lights, two secondary
                    // lights (+0x80 / +0x90, 0 in the cars read), front seat +0xA0, back seat +0xB0, exhaust +0xC0 (read
                    // in a run: the Admiral 0.706 2.474 0.024 / 0.684 -2.864 -0.015 / 0.478 -0.033 -0.156 / 0.484 -1.141
                    // -0.157 / 0.600 -2.644 -0.477, the Cabbie 0.914 2.743 0.046 ...)
                    static const uint32_t kVcsPositions[5] = {0x60u, 0x70u, 0xA0u, 0xB0u, 0xC0u};
                    const uint32_t a = mi + (lcs ? 0x50u + static_cast<uint32_t>(k) * 0x10u : kVcsPositions[k]);
                    if (rdf(a) == 0.0f && rdf(a + 4) == 0.0f && rdf(a + 8) == 0.0f)
                        continue;
                    report("%s -> model %d: position %d %.3f %.3f %.3f (was %.3f %.3f %.3f)", d.mod.c_str(), id, k, x, y, z, rdf(a),
                           rdf(a + 4), rdf(a + 8));
                    wrf(a, x), wrf(a + 4, y), wrf(a + 8, z);
                    ++done;
                }
                else if (int wm = 0; std::sscanf(l.c_str(), "set wheelmodel %d", &wm) == 1 && wm == -1 && lcs)
                {
                    // the car carries the mod's own wheels (elements on its wheel nodes, vcs_modconv.cpp): no shared
                    // wheel model added (+0x40, int16; -1 as on LCS's bikes, which carry theirs)
                    int16_t was;
                    std::memcpy(&was, g_ram + ((mi + 0x40) & kRamMask), 2);
                    const int16_t none = -1;
                    for (int b = 0; b < 2; ++b)
                        wr8(mi + 0x40u + static_cast<uint32_t>(b), reinterpret_cast<const uint8_t *>(&none)[b]);
                    report("%s -> model %d: own wheels, no shared wheel model (was %d)", d.mod.c_str(), id, was);
                    ++done;
                }
                else if (std::sscanf(l.c_str(), "set wheelscale %f", &s) == 1 && s > 0.2f && s < 3.0f)
                {
                    // [vcs] VCS: front +0x58 and rear +0x5C wheel scale (SA's two; equal in every stock vehicle read:
                    // Admiral 0.7, Faggio 0.46, PCJ-600 0.67, Linerunner 1.1, Rhino 1.5)
                    const uint32_t a = lcs ? 0x3Cu : 0x58u;
                    report("%s -> model %d: wheel scale %.3f (was %.3f)", d.mod.c_str(), id, s, rdf(mi + a));
                    wrf(mi + a, s);
                    if (!lcs)
                        wrf(mi + 0x5C, s);
                    ++done;
                }
            }
            if (seen && !done)
                report("%s -> model %d: the mod's light / seat / exhaust positions and wheel size not applied (%s)", d.mod.c_str(), id,
                       "not a car slot");
        }

        // [lcs] colours id=<vehicle model> pair1 r,g,b,r,g,b ... pair8: a carcols.dat line, its PC palette numbers turned
        // into RGB by the manager (from the mod's own colour list). Each colour becomes the nearest entry of the game's
        // car palette (*0x3D7388 + 4, RGBA bytes: 0 = 15,15,15, 1 = 202,202,202, 79 = 14,49,109; docs/LCS.md "Police
        // cars"), written into the model info's colour lists: +0x170 8 primary indices, +0x178 8 secondary, +0x180 the
        // count (read in a run: the Kuruma 2d 3c 40 43 4b 51 11 10 / 24 24 24 24 4b 51 11 10, 8; the bus 7). The game
        // picks one pair per spawned car. VCS: its model info layout for these is not located: reported.
        void applyColours(const Gtag &g, const Directive &d)
        {
            constexpr uint32_t kLcsCarPalette = 0x003D7388u;
            constexpr uint32_t kPaletteColours = 101u;
            const int id = std::atoi(d.keys.count("id") ? d.keys.at("id").c_str() : "0");
            const uint32_t mi = vehicleModelInfo(g, id);
            if (!game::isLcs() || !mi)
            {
                report("%s: colours for model %d skipped: %s", d.mod.c_str(), id, !mi ? "not a vehicle slot" : "LCS only for now");
                return;
            }
            const uint32_t pal = rd32(kLcsCarPalette) & kRamMask;
            if (pal < 0x100000u || pal >= 0x1FFF000u)
            {
                report("%s: colours for model %d skipped: the car palette is not set up yet (0x%08x)", d.mod.c_str(), id, pal);
                return;
            }
            auto nearest = [&](int r, int gr, int b) {
                uint32_t best = 0, bestD = UINT32_MAX;
                for (uint32_t i = 0; i < kPaletteColours; ++i)
                {
                    const uint8_t *c = g_ram + ((pal + 4u + i * 4u) & kRamMask);
                    const uint32_t dd = static_cast<uint32_t>((c[0] - r) * (c[0] - r) + (c[1] - gr) * (c[1] - gr) + (c[2] - b) * (c[2] - b));
                    if (dd < bestD)
                        best = i, bestD = dd;
                }
                return best;
            };
            uint8_t prim[8], sec[8];
            int n = 0;
            for (const auto &[name, value] : d.fields)
            {
                int c[6];
                if (n >= 8 || name.rfind("pair", 0) != 0 ||
                    std::sscanf(value.c_str(), "%d,%d,%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) != 6)
                    continue;
                prim[n] = static_cast<uint8_t>(nearest(c[0], c[1], c[2]));
                sec[n] = static_cast<uint8_t>(nearest(c[3], c[4], c[5]));
                const uint8_t *p = g_ram + ((pal + 4u + prim[n] * 4u) & kRamMask), *s = g_ram + ((pal + 4u + sec[n] * 4u) & kRamMask);
                report("%s -> colours for model %d: pair %d: %d,%d,%d -> %u (%u,%u,%u), %d,%d,%d -> %u (%u,%u,%u)", d.mod.c_str(), id, n + 1,
                       c[0], c[1], c[2], prim[n], p[0], p[1], p[2], c[3], c[4], c[5], sec[n], s[0], s[1], s[2]);
                ++n;
            }
            if (!n)
            {
                report("%s: colours for model %d skipped: no colour pair", d.mod.c_str(), id);
                return;
            }
            const uint8_t was = g_ram[(mi + 0x180u) & kRamMask];
            for (int i = 0; i < 8; ++i)
            {
                wr8(mi + 0x170u + static_cast<uint32_t>(i), prim[i % n]);
                wr8(mi + 0x178u + static_cast<uint32_t>(i), sec[i % n]);
            }
            wr8(mi + 0x180u, static_cast<uint8_t>(n));
            report("%s -> colours for model %d: %d pairs written (was %u)", d.mod.c_str(), id, n, was);
        }

        void applyModel(const Gtag &g, const Directive &d)
        {
            const int id = std::atoi(d.keys.count("id") ? d.keys.at("id").c_str() : "-1");
            if (!g.streamInfo || id < 1 || id >= static_cast<int>(P().textureListBase))
            {
                report("%s: model %d skipped: no streaming directory or not a model id", d.mod.c_str(), id);
                return;
            }
            const uint32_t mi = ptr(g.modelInfos + static_cast<uint32_t>(id) * 4u);
            if (!g.inImage(mi))
            {
                report("%s: model %d skipped: no model info", d.mod.c_str(), id);
                return;
            }
            uint16_t slot;
            std::memcpy(&slot, g_ram + mi + 0x1E, 2);
            const uint32_t texIndex = P().textureListBase + slot;
            // [crossover] donor=<model id>: a new model id (lcs_crossover.cpp) has no stock data of its own; the
            // conversion is made on the donor's stock model / texture list (and cached under the donor's id)
            int donor = id;
            uint32_t donorTex = texIndex;
            if (d.keys.count("donor"))
            {
                donor = std::atoi(d.keys.at("donor").c_str());
                const uint32_t dmi = donor > 0 && donor < static_cast<int>(P().textureListBase)
                                         ? ptr(g.modelInfos + static_cast<uint32_t>(donor) * 4u) : 0;
                if (!g.inImage(dmi))
                {
                    report("%s: model %d skipped: donor %d has no model info", d.mod.c_str(), id, donor);
                    return;
                }
                uint16_t dslot;
                std::memcpy(&dslot, g_ram + dmi + 0x1E, 2);
                donorTex = P().textureListBase + dslot;
            }
            IPS2DiscImage *disc = g_disc ? g_disc->inner() : ps2_disc::image();
            uint32_t img = 0;
            std::vector<uint8_t> donorMdl, donorXet, dff, txd;
            if (!disc || !imgLsn(disc, img) || !readEntry(g, disc, img, static_cast<uint32_t>(donor), donorMdl) ||
                !readEntry(g, disc, img, donorTex, donorXet))
            {
                report("%s: model %d skipped: its stock model / texture list could not be read", d.mod.c_str(), id);
                return;
            }
            // VCS_MODS_DONORS=<folder>: the slot's stock entries written there as <game>-<id>.ldm / .xet, for the PC
            // harness (tools/modconv_check.cpp). The player's own disc data: never share or commit them.
            if (const char *dump = std::getenv("VCS_MODS_DONORS"); dump && *dump)
            {
                std::error_code ec;
                fs::create_directories(dump, ec);
                const std::string stem = std::string(game::isLcs() ? "lcs-" : "vcs-") + std::to_string(id);
                std::ofstream(fs::path(dump) / (stem + ".ldm"), std::ios::binary)
                    .write(reinterpret_cast<const char *>(donorMdl.data()), static_cast<std::streamsize>(donorMdl.size()));
                std::ofstream(fs::path(dump) / (stem + ".xet"), std::ios::binary)
                    .write(reinterpret_cast<const char *>(donorXet.data()), static_cast<std::streamsize>(donorXet.size()));
            }
            const std::string dffName = d.keys.count("dff") ? d.keys.at("dff") : "", txdName = d.keys.count("txd") ? d.keys.at("txd") : "";
            const files::Location dffAt = files::resolve(dffName, modsDir()), txdAt = files::resolve(txdName, modsDir());
            modconv::Options o;
            o.lcs = game::isLcs(); // [lcs] LCS's chunk dialect (vcs_modconv.h Options::lcs)
            const bool vehicle = g_ram[mi + 0x10] == 6;
            if (vehicle)
                // a stock car is 1-2.5k triangles; LCS's has one 128 x 128 texture (a mod's all together: ten of those at
                // most, vcs_modconv buildXet); VCS 64 px as before (its stock car textures are 4-bit; converted ones
                // look mottled on some VCS cars at 64 and at 128 alike, docs/ANDROID.md "Not done")
                o.maxTriangles = 3000, o.maxTexture = game::isLcs() ? 128 : 64;
            if (d.keys.count("tris"))
                o.maxTriangles = std::clamp(std::atoi(d.keys.at("tris").c_str()), 100, 20000);
            if (d.keys.count("tex"))
                o.maxTexture = std::clamp(std::atoi(d.keys.at("tex").c_str()), 16, 256);
            if (d.keys.count("part")) // one weapon of a clump holding several (vcs_modconv.h Options::part)
                o.part = d.keys.at("part"); // a frame name (the manager writes it %XX-escaped only if it holds a space / %; none seen)
            modconv::Result r;
            ModelCache cache(cacheFolder(dffName, d.mod), donor, o, donorMdl, donorXet, d.keys.count("cache") ? d.keys.at("cache") : "");
            // [mods] the sources' stamps (size, time, entry): unchanged since the cache was made -> taken without
            // opening the .dff / .txd (or their zip). A source that isn't there is never served from the cache.
            const std::string dffStamp = files::stamp(dffAt), txdStamp = txdName.empty() ? "-" : files::stamp(txdAt);
            const std::string stamps = dffStamp.empty() || txdStamp.empty() ? "" : dffStamp + "\n" + txdStamp + "\n";
            const bool byStamp = cache.loadByStamp(stamps, r);
            if (byStamp)
                report("%s -> model %d: cached (%s; sources unchanged, not read)", d.mod.c_str(), id, cache.where().c_str());
            else
            {
                std::string why;
                if (!readModFile(dffName, dff, why))
                {
                    report("%s: model %d skipped: %s", d.mod.c_str(), id, why.c_str());
                    return;
                }
                if (!txdName.empty() && !readModFile(txdName, txd, why))
                    report("%s: model %d: no textures (%s)", d.mod.c_str(), id, why.c_str());
                cache.sources(dff, txd);
            }
            if (byStamp)
            {
            }
            else if (cache.load(r))
            {
                cache.stampAgain(stamps);
                report("%s -> model %d: cached (%s)", d.mod.c_str(), id, cache.where().c_str());
            }
            else
            {
                std::string error;
                if (!(vehicle ? modconv::convertVehicle : modconv::convertSimple)(donorMdl, donorXet, dff, txd, id, o, r, error))
                {
                    report("%s: model %d not converted: %s", d.mod.c_str(), id, error.c_str());
                    return;
                }
                std::istringstream lines(r.report);
                for (std::string l; std::getline(lines, l);)
                    report("%s -> model %d: %s", d.mod.c_str(), id, l.c_str());
                report("%s -> model %d: converted%s", d.mod.c_str(), id,
                       cache.save(r, stamps) ? (" (saved to " + cache.where() + ")").c_str() : " (not cached: no mod folder, or it is not writable)");
            }
            if (vehicle)
                applyCarSets(mi, id, d, r.report);
            const size_t mdlBytes = r.mdl.size(), xetBytes = r.xet.size();
            serve(g, disc, img, static_cast<uint32_t>(id), std::move(r.mdl));
            g_watchIndex = static_cast<uint32_t>(id);
            serve(g, disc, img, texIndex, std::move(r.xet));
            report("%s -> model %d: streaming index %d (%zu bytes, was %zu) and its texture list %u (%zu bytes, was %zu) "
                   "served from the converted model",
                   d.mod.c_str(), id, id, mdlBytes, donorMdl.size(), texIndex, xetBytes, donorXet.size());
        }

        Gtag g_tables; // [crossover] LCS's tables as lcsTablesHook found them (bindVehicle)
        void (*g_onTables)(uint8_t *rdram) = nullptr; // [hero] onTables

        // ---- the hook --------------------------------------------------------------------------------------------------

        void writeReport()
        {
            std::error_code ec;
            fs::create_directories(modsDir(), ec);
            std::ofstream out(modsDir() / "applied.txt", std::ios::trunc);
            out << "# written by the game at its last start: what the mod loader changed\n";
            for (const std::string &l : g_report)
                out << l << "\n";
        }

        // ---- whole disc files (replace disc=<path on the disc> file=<path under mods/>) ----------------------------------
        // The game opens files by name through its own disc index, built at boot from the disc's directories: an
        // entry's LSN and size are repointed into the range ModDisc serves from host memory, so the next open of that
        // name reads the mod's bytes (any size). Files the game opened before the patch keep the disc's.

        struct DiscIndex
        {
            uint32_t entries, count, stride, lsnOff, sizeOff;
        };
        // VCS: built by FUN_002209F0, {name, LSN, size, ?} (vcs_overrides.cpp traceOpen prints +4 / +8 as LSN / size)
        constexpr DiscIndex kVcsIndex{0x005038A8u, 0x00486E8Cu, 16u, 4u, 8u};
        // [lcs] LCS: the open 0x2E6208 strcmps each entry's name (+0) over gp+0xD14 (0x3D7C04) entries of 16 bytes
        // at 0x62AA70 and copies +8 to the handle's LSN (+4) and +0xC to its size (+8); the IMG lookup 0x2E60F8 hands
        // out the same +8 / +0xC. The index is built by 0x2E5EB8 (lui 0x63 / addiu -0x5590 at 0x2E5EFC).
        constexpr DiscIndex kLcsIndex{0x0062AA70u, 0x003D7C04u, 16u, 8u, 0xCu};

        // "cdrom0:\TEXT\ENGLISH.GXT;1", "/text/english.gxt", "TEXT\ENGLISH.GXT" -> "TEXT\ENGLISH.GXT"
        std::string discName(std::string s)
        {
            for (char &c : s)
                c = c == '/' ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (s.rfind("CDROM0:", 0) == 0)
                s.erase(0, 7);
            while (!s.empty() && s[0] == '\\')
                s.erase(0, 1);
            if (s.size() > 2 && s.compare(s.size() - 2, 2, ";1") == 0)
                s.resize(s.size() - 2);
            return s;
        }

        void applyReplace(const Directive &d)
        {
            const std::string want = discName(d.keys.count("disc") ? d.keys.at("disc") : "");
            const std::string rel = d.keys.count("file") ? d.keys.at("file") : "";
            std::vector<uint8_t> bytes;
            std::string why = "no disc=";
            if (want.empty() || !readModFile(rel, bytes, why))
            {
                report("%s: replace %s skipped: %s", d.mod.c_str(), want.c_str(), why.c_str());
                return;
            }
            const DiscIndex &x = game::pick(kVcsIndex, kLcsIndex);
            const uint32_t count = std::min<uint32_t>(rd32(x.count), 4096u);
            uint32_t entry = 0;
            for (uint32_t i = 0; i < count && !entry; ++i)
            {
                const uint32_t e = x.entries + i * x.stride, name = rd32(e) & kRamMask;
                if (name > 0x100000u && name < 0x02000000u && discName(text(name, 128)) == want)
                    entry = e;
            }
            if (!entry)
            {
                report("%s: replace %s skipped: not in the game's disc index (%u entries)", d.mod.c_str(), want.c_str(), count);
                return;
            }
            if (!g_disc)
            {
                g_disc = std::make_unique<ModDisc>(ps2_disc::image());
                ps2_disc::setImage(g_disc.get());
            }
            const uint32_t size = static_cast<uint32_t>(bytes.size()), oldLsn = rd32(entry + x.lsnOff), oldSize = rd32(entry + x.sizeOff);
            const uint32_t lsn = g_disc->add(kFileBlob, std::move(bytes));
            wr32(entry + x.lsnOff, lsn);
            wr32(entry + x.sizeOff, size);
            report("%s: disc file %s now served from %s: LSN 0x%x, %u bytes (was 0x%x, %u)", d.mod.c_str(), want.c_str(),
                   files::resolve(rel, modsDir()).shown.c_str(), lsn,
                   size, oldLsn, oldSize);
        }

        // active.txt, read once per start (VCS: at GAME.DTZ; LCS: at the first file open after the disc index)
        bool g_loaded = false, g_none = true;
        std::vector<Directive> g_directives;
        std::vector<std::string> g_mods;

        bool loadActive()
        {
            if (g_loaded)
                return !g_none;
            g_loaded = true;
            const char *env = std::getenv("VCS_MODS");
            const fs::path active = modsDir() / "active.txt";
            std::error_code ec;
            if ((env && *env == '0') || !fs::exists(active, ec))
            {
                std::fprintf(stderr, "[mods] no mods applied (%s)\n", env && *env == '0' ? "VCS_MODS=0" : "no mods/active.txt");
                fs::remove(modsDir() / "applied.txt", ec);
                return false;
            }
            g_none = false;
            g_directives = readActive(active);
            // [mods] "needs file=<path>": a mod kept where it is names its originals; one missing (renamed, deleted,
            // its Android permission gone) -> the whole mod is skipped this start, reported, and the others apply
            std::vector<std::string> missing;
            for (const Directive &d : g_directives)
                if (d.kind == "needs" && std::find(missing.begin(), missing.end(), d.mod) == missing.end())
                {
                    const files::Location l = files::resolve(d.keys.count("file") ? d.keys.at("file") : "", modsDir());
                    if (!files::available(l))
                    {
                        missing.push_back(d.mod);
                        report("%s: skipped: original missing (%s%s%s)", d.mod.c_str(), l.shown.c_str(), l.error.empty() ? "" : ": ",
                               l.error.c_str());
                    }
                }
            g_directives.erase(std::remove_if(g_directives.begin(), g_directives.end(),
                                              [&](const Directive &d) {
                                                  return d.kind == "needs" ||
                                                         std::find(missing.begin(), missing.end(), d.mod) != missing.end();
                                              }),
                               g_directives.end());
            for (const Directive &d : g_directives)
                if (std::find(g_mods.begin(), g_mods.end(), d.mod) == g_mods.end())
                    g_mods.push_back(d.mod);
            std::fprintf(stderr, "[mods] %s: %zu directive(s) from %zu mod(s)\n", active.string().c_str(), g_directives.size(), g_mods.size());
            return true;
        }

        void applyFiles()
        {
            for (const Directive &d : g_directives)
                if (d.kind == "replace")
                    applyReplace(d);
        }

        void applyTables(const Gtag &g)
        {
            std::fprintf(stderr, "[mods] GTAG at 0x%08x: model infos 0x%08x, streaming info 0x%08x, weapon tables 0x%08x 0x%08x\n",
                         g.base, g.modelInfos, g.streamInfo, g.weaponTables[0], g.weaponTables[1]);
            for (const Directive &d : g_directives)
            {
                if (d.kind == "replace")
                    continue; // applyFiles
                if (d.kind == "handling")
                    applyHandling(g, d);
                else if (d.kind == "weapon")
                    applyWeapon(g, d);
                else if (d.kind == "stream")
                    applyStream(g, d);
                else if (d.kind == "model")
                    applyModel(g, d);
                else if (d.kind == "colours")
                    applyColours(g, d);
                else if (d.kind == "sound" && !game::isLcs())
                    report("%s: sound: LCS only, skipped", d.mod.c_str());
                else if (d.kind == "sound") // [lcs] applied by the sound driver when its banks load (game/lcs_sfxmod.cpp)
                    report("%s: sound %s: now served by the sound driver (its log: [sfxmod])", d.mod.c_str(),
                           d.keys.count("file") ? files::resolve(d.keys.at("file"), modsDir()).shown.c_str() : "?");
                else if (d.kind == "anims" && !game::isLcs())
                    report("%s: anims: LCS only, skipped", d.mod.c_str());
                else if (d.kind == "anims") // [lcs] an animation set, loaded by the swimming (game/lcs_swim.cpp)
                    report("%s: anims %s: now served by the swimming (its log: [lcs-swim])", d.mod.c_str(),
                           d.keys.count("file") ? files::resolve(d.keys.at("file"), modsDir()).shown.c_str() : "?");
                else if (d.kind == "hero") // [hero] the other game's player model (game/stories_hero.cpp, STORIES_HERO)
                    report("%s: hero %s: used when the Stories hero option asks for him (its log: [hero])", d.mod.c_str(),
                           d.keys.count("name") ? d.keys.at("name").c_str() : "?");
                else if (d.kind == "test") // a device test: give the player a weapon once free play starts
                {
                    if (d.keys.count("give"))
                        g_testGive = std::atoi(d.keys.at("give").c_str());
                    if (d.keys.count("spawn"))
                        g_testSpawn = std::atoi(d.keys.at("spawn").c_str());
                }
                else
                    report("%s: unknown directive '%s' skipped", d.mod.c_str(), d.kind.c_str());
            }
            g_applied = static_cast<int>(g_mods.size());
            report("%d mod(s) applied, %zu words changed (originals kept)", g_applied.load(), g_original.size());
            writeReport();
        }

        void onRelocated(uint8_t *rdram, uint32_t header, uint32_t image)
        {
            static bool done = false; // GAME.DTZ is relocated once per start; the level images (WRLD) don't match
            if (done)
                return;
            g_ram = rdram;
            Gtag g;
            if (!findGtag(header, image, g))
                return;
            done = true;
            g_streamInfo = g.streamInfo;
            g_tables = g; // [crossover] VCS: for bindVehicle at run time (game/vcs_crossover.cpp)
            if (g_onTables) // [hero] game/stories_hero.cpp
                g_onTables(rdram);
            if (!loadActive())
                return;
            applyFiles(); // VCS: files opened after GAME.DTZ (not checked in a run on VCS)
            applyTables(g);
        }

        PS2_GUEST_CC void relocateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t header = GPR_U32(ctx, 4), image = GPR_U32(ctx, 5);
            VCS_GAME_FN(FUN_00324a50_0x324a50, 0x00324A50u)(rdram, ctx, runtime);
            onRelocated(rdram, header, image);
        }

        // ---- [lcs] Liberty City Stories ----------------------------------------------------------------------------
        //
        // The file open (0x2E6208, name in a0; every CFileMgr-style open of a disc file goes through it, GAME.DTZ's
        // and the GXT's included): once the disc index has entries, the replace directives patch it, before the
        // original looks the name up.
        constexpr uint32_t kLcsOpen = 0x002E6208u;
        // CModelInfo setup from GAME.DTZ (0x31FAF0(count, CModelInfo *infos[]): stores them in 0x3D9BA0 / 0x3D8720,
        // gp + 0x2CB0 / + 0x1830), called at 0x104850 by the GAME.DTZ loader 0x1045A8 after the relocation 0x22C4E8
        // (0x104660), the streaming info pointer (0x18A3D0 at 0x104774) and the weapon infos (0x34F2E0 at 0x104834).
        // Its arguments are already relocated pointers, so no GTAG header layout is needed. Once per start.
        constexpr uint32_t kLcsModelInfoSetup = 0x0031FAF0u;
        constexpr uint32_t kLcsStreamInfoPtr = 0x003D0CC8u; // -> streaming info array (gp - 0x6228, 0x18A3DC)
        constexpr uint32_t kLcsWeaponInfoPtr = 0x003D9B48u; // -> {?, CWeaponInfo[] (0x70 each)} (0x34F2E4; GetWeaponInfo 0x34F2F0 reads +4)
        // -> cHandlingDataMgr: the GAME.DTZ loader stores header word 0x22 there (0x1047E0, before the model-info setup
        // 0x104850); 0x327EEC reads the manager's +4 through it. Records from +0x20 (5 floats, padding to 16).
        constexpr uint32_t kLcsHandlingMgrPtr = 0x003D0238u;
        constexpr uint32_t kLcsHandlingFirst = 0x20u;

        // a weapon table checks out when its pistol record (LCS type 17, lcs_trainer.cpp kWeapons) holds the stock
        // values read in a run (VCS_MODS_DUMP=1): +0 fire type 1 (instant hit), +0x10 clip 17, +0x14 damage 25, +0x60
        // model 0x112 - VCS's pistol values with the fields 4 bytes lower (Profile::weaponShift)
        bool lcsWeaponTableOk(uint32_t t)
        {
            const uint32_t pistol = t + 17u * kWeaponRecord;
            return rd32(pistol) == 1u && rd32(pistol + 0x10) == 17u && rd32(pistol + 0x14) == 25u && rd32(pistol + 0x60) == 0x112u;
        }

        // VCS_MODS_DUMP=1: the raw records the LCS field maps come from (the log only)
        void lcsDiagnostics(const Gtag &g, uint32_t weapons)
        {
            const char *env = std::getenv("VCS_MODS_DUMP");
            if (!env || *env != '1')
                return;
            std::fprintf(stderr, "[mods] dump: model count %u infos 0x%08x streaming 0x%08x weapon infos 0x%08x -> %08x %08x\n",
                         g.modelCount, g.modelInfos, g.streamInfo, weapons, rd32(weapons), rd32(weapons + 4));
            for (int i = 0; i < 2; ++i)
            {
                const uint32_t t = rd32(weapons + static_cast<uint32_t>(i) * 4u) & kRamMask;
                if (!g.inImage(t))
                    continue;
                for (uint32_t type : {0u, 1u, 17u, 19u, 26u})
                {
                    const uint32_t r = t + type * kWeaponRecord;
                    std::string line;
                    char w[16];
                    for (uint32_t k = 0; k < kWeaponRecord; k += 4)
                        std::snprintf(w, sizeof w, " %08x", rd32(r + k)), line += w;
                    std::fprintf(stderr, "[mods] dump: weapons[%d] type %u @0x%08x:%s\n", i, type, r, line.c_str());
                }
            }
            int shown = 0;
            for (uint32_t id = 0; id < g.modelCount && shown < 3; ++id)
            {
                const uint32_t mi = rd32(g.modelInfos + id * 4u) & kRamMask;
                if (!g.inImage(mi) || g_ram[mi + 0x10] != 6)
                    continue;
                ++shown;
                std::string line;
                char w[16];
                for (uint32_t k = 0; k < 0x80; k += 4)
                    std::snprintf(w, sizeof w, " %08x", rd32(mi + k)), line += w;
                std::fprintf(stderr, "[mods] dump: vehicle %u %s @0x%08x:%s\n", id, text(mi + 0x2E, 8).c_str(), mi, line.c_str());
            }
            for (uint32_t i : {1u, 2u, 0x1324u, 0x1325u})
            {
                const uint32_t e = g.streamInfo + i * 0x14u;
                std::fprintf(stderr, "[mods] dump: stream %u: %08x %08x %08x %08x %08x %08x\n", i, rd32(e), rd32(e + 4), rd32(e + 8),
                             rd32(e + 0xC), rd32(e + 0x10), rd32(e + 0x14));
            }
            const uint32_t n = std::min<uint32_t>(rd32(kLcsIndex.count), 4096u);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t e = kLcsIndex.entries + i * 16u;
                std::fprintf(stderr, "[mods] dump: disc index %u \"%s\" %08x lsn 0x%x size %u\n", i, text(rd32(e) & kRamMask, 64).c_str(),
                             rd32(e + 4), rd32(e + 8), rd32(e + 12));
            }
        }

        PS2_GUEST_CC void lcsOpenHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static bool done = false;
            if (!done)
            {
                g_ram = rdram;
                if (rd32(kLcsIndex.count) != 0)
                {
                    done = true;
                    if (loadActive())
                    {
                        applyFiles();
                        writeReport();
                    }
                }
            }
            ps2GameFunction<kLcsOpen>(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void lcsTablesHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static bool done = false;
            if (!done)
            {
                done = true;
                g_ram = rdram;
                Gtag g;
                g.base = 0x00100000u;            // pointer checks: anywhere in the game's RAM above its code
                g.extent = 0x02000000u - g.base;
                g.modelCount = GPR_U32(ctx, 4);
                g.modelInfos = GPR_U32(ctx, 5) & kRamMask;
                g.streamInfo = rd32(kLcsStreamInfoPtr) & kRamMask;
                if (!g.inImage(g.streamInfo))
                    g.streamInfo = 0;
                const uint32_t weapons = rd32(kLcsWeaponInfoPtr) & kRamMask;
                for (int i = 0; i < 2 && g.inImage(weapons); ++i)
                {
                    const uint32_t t = rd32(weapons + static_cast<uint32_t>(i) * 4u) & kRamMask;
                    if (g.inImage(t) && lcsWeaponTableOk(t))
                        g.weaponTables[i] = t;
                }
                // the handling records: counted while each record's id (+0x10) is its index and its mass positive (81)
                const uint32_t mgr = rd32(kLcsHandlingMgrPtr) & kRamMask;
                if (g.inImage(mgr))
                {
                    const uint32_t first = mgr + kLcsHandlingFirst;
                    uint32_t n = 0;
                    while (n < 256u && rd32(first + n * kLcsHandling.size + kLcsHandling.ident) == n &&
                           rdf(first + n * kLcsHandling.size + kLcsHandling.mass) > 0.0f)
                        ++n;
                    if (n)
                        g.handlingRecords = first, g.handlingCount = n;
                }
                std::fprintf(stderr, "[mods] handling manager 0x%08x: %u records at 0x%08x\n", mgr, g.handlingCount, g.handlingRecords);
                g_streamInfo = g.streamInfo;
                g_tables = g; // [crossover] for bindVehicle at run time
                lcsDiagnostics(g, weapons);
                if (loadActive())
                    applyTables(g);
                if (g_onTables) // [hero] game/stories_hero.cpp
                    g_onTables(rdram);
            }
            ps2GameFunction<kLcsModelInfoSetup>(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (game::isLcs())
        {
            g_profile = &kLcsProfile; // [lcs]
            g_relativeBelow = 0;
            runtime.replaceFunction(kLcsOpen, &lcsOpenHook);
            runtime.replaceFunction(kLcsModelInfoSetup, &lcsTablesHook);
            return;
        }
        runtime.replaceFunction(kRelocate, &relocateHook);
    }

    void revert(uint8_t *rdram)
    {
        for (const auto &[a, bytes] : g_original)
            std::memcpy(rdram + a, bytes.data(), bytes.size());
        std::fprintf(stderr, "[mods] reverted %zu words\n", g_original.size());
        g_original.clear();
        if (g_disc)
            g_disc->clear();
        g_applied = 0;
    }

    int appliedMods() { return g_applied.load(); }

    void onGameFrame(uint8_t *rdram)
    {
        // once, ~20 s in: what the game itself now reaches through its own pointers (the model-info array global
        // 0x486518, a vehicle's handling pointer), so the log shows the values in use, not only what was written
        static int frames = 0, free = 0;
        if (g_original.empty())
            return;
        g_ram = rdram;
        ++frames;
        // free play: no script camera (vcs_save_anywhere.cpp's kCameraLooksAtPlayer) for 3 s
        // ([lcs] LCS: TheCamera + 0x9C, lcs_trainer.cpp's cameraLooksAtPlayer; the trainer is lcs::trainer)
        const bool lcs = game::isLcs();
        free = rd32(P().frameFreeCamera) == 1 ? free + 1 : 0;
        if (g_testGive > 0 && free > 180)
        {
            std::fprintf(stderr, "[mods] free play: testing (giving weapon type %d)\n", g_testGive);
            lcs ? lcs::trainer::giveWeapon(g_testGive) : vcs::trainer::giveWeapon(g_testGive);
            g_testGive = 0;
        }
        if (g_testSpawn > 0 && free > 240 && (lcs ? lcs::trainer::ready() : vcs::trainer::ready()))
        {
            std::fprintf(stderr, "[mods] free play: testing (spawning vehicle %d)\n", g_testSpawn);
            lcs ? lcs::trainer::spawnVehicle(g_testSpawn) : vcs::trainer::spawnVehicle(g_testSpawn);
            g_watchIndex = static_cast<uint32_t>(g_testSpawn);
            g_testSpawn = 0;
        }
        static uint32_t lastState = UINT32_MAX;
        if (g_watchIndex != UINT32_MAX && g_streamInfo)
        {
            const uint32_t state = g_ram[(g_streamInfo + g_watchIndex * P().streamEntry + 0xC) & kRamMask];
            if (state != lastState)
                std::fprintf(stderr, "[mods] streaming index %u load state %u (frame %d)\n", g_watchIndex, state, frames), lastState = state;
        }
        if (frames != 600 || lcs) // (the check below reads VCS's addresses)
            return;
        const uint32_t infos = rd32(0x00486518u) & kRamMask;
        for (int id : {254, 207, 272})
        {
            const uint32_t mi = rd32(infos + static_cast<uint32_t>(id) * 4u) & kRamMask;
            const uint32_t rec = rd32(mi + 0x38);
            const uint32_t r = rec < kImageSize ? g_base + rec : rec & kRamMask;
            std::fprintf(stderr, "[mods] check: model %d info 0x%08x handling ptr 0x%08x -> mass %g accel %g traction %g gears %u drive %c\n",
                         id, mi, rec, rdf(r + 0xBC), rdf(r + 0x48), rdf(r + 0xC4), g_ram[r + 0x5E], g_ram[r + 0x5C]);
        }
    }

    // [crossover] a stories pack's vehicle bound into a slot at run time (game/lcs_crossover.cpp; LCS). The same steps as
    // a start's `model` and `handling` directives (applyModel / applyHandling), on the tables lcsTablesHook found.
    bool bindVehicle(uint8_t *rdram, int id, const std::string &mod, const std::string &dff, const std::string &txd,
                     const std::string &cacheTag, const std::vector<std::pair<std::string, std::string>> &handling,
                     std::string &why, int donor)
    {
        if (!g_tables.streamInfo || !g_tables.modelInfos)
            return why = "the game's tables are not known yet", false;
        g_ram = rdram;
        Directive m;
        m.kind = "model";
        m.mod = mod;
        m.keys = {{"id", std::to_string(id)}, {"dff", dff}, {"txd", txd}, {"cache", cacheTag}};
        if (donor > 0)
            m.keys["donor"] = std::to_string(donor);
        const size_t before = g_disc ? g_disc->endSector() : 0;
        applyModel(g_tables, m);
        if (!g_disc || g_disc->endSector() == before)
            return why = "not converted (see the log's [mods] lines)", false;
        if (!handling.empty())
        {
            Directive h;
            h.kind = "handling";
            h.mod = mod;
            h.keys = {{"id", std::to_string(id)}};
            h.fields = handling;
            applyHandling(g_tables, h);
        }
        return true;
    }

    void setScratch(uint32_t lo, uint32_t hi) { g_tables.scratchLo = g_scratchLo = lo, g_tables.scratchHi = g_scratchHi = hi; }

    void setVehicleColours(uint8_t *rdram, int id, const std::string &mod, const std::vector<std::string> &pairs)
    {
        if (!game::isLcs() || !g_tables.modelInfos || pairs.empty())
            return;
        g_ram = rdram;
        Directive d;
        d.kind = "colours";
        d.mod = mod;
        d.keys = {{"id", std::to_string(id)}};
        for (size_t i = 0; i < pairs.size() && i < 8; ++i)
            d.fields.emplace_back("pair" + std::to_string(i + 1), pairs[i]);
        applyColours(g_tables, d);
    }

    void setHandlingOverride(uint32_t modelInfo, uint32_t record)
    {
        if (record)
            g_handlingOverride[modelInfo & kRamMask] = record & kRamMask;
        else
            g_handlingOverride.erase(modelInfo & kRamMask);
    }

    uint32_t streamEntry(uint32_t index)
    {
        const uint32_t si = g_tables.streamInfo ? g_tables.streamInfo : g_streamInfo; // [hero] VCS: g_streamInfo
        return si && index < P().streamIndices ? si + index * P().streamEntry : 0;
    }

    uint32_t modelInfo(int id)
    {
        if (!g_tables.modelInfos || id < 0 || static_cast<uint32_t>(id) >= g_tables.modelCount || !g_ram)
            return 0;
        return rd32(g_tables.modelInfos + static_cast<uint32_t>(id) * 4u) & kRamMask;
    }

    uint32_t textureListBase() { return P().textureListBase; }

    void onTables(void (*fn)(uint8_t *rdram)) { g_onTables = fn; }

    uint32_t serveBlob(std::vector<uint8_t> bytes, uint32_t &sectors)
    {
        IPS2DiscImage *disc = g_disc ? g_disc->inner() : ps2_disc::image();
        uint32_t img = 0;
        sectors = 0;
        if (!disc || !imgLsn(disc, img) || bytes.empty())
            return 0;
        if (!g_disc)
        {
            g_disc = std::make_unique<ModDisc>(disc);
            ps2_disc::setImage(g_disc.get());
        }
        sectors = static_cast<uint32_t>((bytes.size() + 2047u) / 2048u);
        const uint32_t lsn = g_disc->add(0xFFFF0000u, std::move(bytes)); // (no streaming index: the log's "65535 << 16")
        if (P().clampedImg)
            extendImg(img, g_disc->endSector());
        return lsn - img;
    }

    uint32_t handlingRecord(int id)
    {
        const uint32_t mi = modelInfo(id);
        return mi && g_tables.handlingRecords ? handlingRecord(g_tables, mi) : 0;
    }
}
