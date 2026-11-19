// [lcs] See lcs_trainer.h and docs/TRAINER.md. The logic follows vcs_trainer.cpp (queued actions run as short scripts,
// held toggles every 10 frames, models streamed before use, scripts taken back out while the game is paused); the
// addresses, script commands and lists are Liberty City Stories' (SLUS-21423).
//
// Evidence for the addresses: LCS's script interpreter was read from the ELF. CRunningScript::ProcessOneCommand
// 0x10A7A8 dispatches by blocks of ~100 commands to 17 switch functions (0x10A9A0 for 0..0x63, 0x10DA48, 0x10FF30, ...);
// each command below was decoded from its switch's jump table and its case checked: parameter count passed to
// CollectParameters 0x1094C8, the pool it looks the handle up in (peds *0x3D5AE8, vehicles *0x3D5AEC, players
// 0x408E40 + n * 0x190) and what it does. The command numbers agree with the Sanny Builder library's LCS list.
#include "game/lcs_trainer.h"

#include "debug/rig.h"
#include "game/bridge_points.h"
#include "game/game_addrs.h"
#include "game/game_table.h"
#include "game/lcs_crossover.h"
#include "game/lcs_freecam.h"
#include "game/lcs_swim.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>

namespace lcs::trainer
{
    using namespace vcs::trainer; // the Toggle values

    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        // ---- Addresses (LCS) ----------------------------------------------------------------------------------------
        struct Addrs
        {
            // CTheScripts::Process (iterates the active list 0x3D9C00 calling each script; called from the game's
            // process 0x2FAE40 / 0x2FBAF0): the trainer's frame hook, before the scripts run.
            uint32_t scriptsProcess = 0x0010A288u;
            // the script lists: TERMINATE_THIS_SCRIPT (0x004E, case 0x10CD94) unlinks from 0x3D9C00 (0x10A1B8) and
            // links into 0x3D9C04 (0x10A1F0); StartNewScript 0x10A210 does the reverse
            uint32_t activeScripts = 0x003D9C00u;
            uint32_t idleScripts = 0x003D9C04u;
            uint32_t nextScriptId = 0x003CF100u; // StartNewScript: id = this++ into +8
            // CPlayerInfo[0] = 0x408E40 (stride 0x190): +0 the ped (shared table), +0xBC money (ADD_SCORE 0x010E,
            // case 0x110C58: lw/addu/sw 0xBC)
            uint32_t pedPool = 0x003D5AE8u;      // -> CPool<CPed>: +0 objects, +4 flags (GetAt 0x112068), 0xCD0 each
            uint32_t modelCount = 0x003D9BA0u;   // CModelInfo count (0x31F948: slt id, count)
            uint32_t modelInfos = 0x003D8720u;   // -> CModelInfo* [count] (0x31F948); info +0x10 type (6 = vehicle)
            uint32_t streamingInfo = 0x003D0CC8u; // -> per-model streaming info, 0x14 each, +0xC state 1 = loaded
                                                  // (HAS_MODEL_LOADED 0x024D, case 0x25CF70)
            uint32_t textTable = 0x003D89B8u;    // -> CText: +0 entries {u32 UTF-16 text, char key[8]}, +4 count
                                                 // (LOAD_MISSION_TEXT 0x0551 creates it; read in a run)
            uint32_t weaponInfos = 0x003D9B48u;  // -> {?, CWeaponInfo[] (0x70 each)}: GetWeaponInfo 0x34F2F0;
                                                 // +0x60 model, +0x68 slot (0x1E1380)
            uint32_t clockHours = 0x003D9B74u;   // u8: SetGameClock 0x1F87E0 (SET_TIME_OF_DAY 0x00C0's callee)
            uint32_t clockMinutes = 0x003D9B80u; // u8: the same
            uint32_t currArea = 0x003D8430u;     // SET_AREA_VISIBLE (0x04C0, case 0x185A70) stores its argument here
            uint32_t cameraLooksAtPlayer = 0x0043BC8Cu; // TheCamera + 0x9C (internal LCS address report)
            // CWanted::MaximumWantedLevel (gp + 0x13AC): SetWantedLevel 0x2F4C48 clamps the level to it (slt / movz),
            // SetMaximumWantedLevel 0x2F4DA8 writes it with the chaos cap 0x3D82A0. The opening mission keeps it at 0,
            // so a wanted level asked for there stayed 0 (the trainer's first test)
            uint32_t maxWantedLevel = 0x003D829Cu;
            // CPed::SetDead (health 0, state 0x37; 0x1E9368): CVehicle::KillPedsInVehicle (0x32A1E8: the driver at
            // +0x1F8, the passengers from +0x1FC, count +0x220 - as GTA III's) calls it for whoever sits in a vehicle
            // that blows up, past the ped's proofs
            uint32_t pedSetDead = 0x001E9368u;
        };
        constexpr Addrs A{};

        // CRunningScript (Init 0x109A88, StartNewScript 0x10A210, ProcessOneCommand 0x10A7A8)
        constexpr uint32_t kScriptName = 0x10u;    // char[8] ("id%02i" by Init)
        constexpr uint32_t kScriptIp = 0x18u;      // offset from ScriptSpace
        constexpr uint32_t kScriptLocals = 0x60u;  // 0x6A words (CollectParameters' variable 0x1098D8)
        constexpr uint32_t kScriptActive = 0x20Cu; // u8: set by StartNewScript, cleared by TERMINATE_THIS_SCRIPT
        constexpr uint32_t kScriptInit = 0x21Au;   // the bytes Init sets (its last write is +0x219)
        constexpr uint32_t kScriptUnknown217 = 0x217u; // Init sets it to 1 (as VCS's +0x20D)
        // Our code lives in the script's own locals 0x30.. (+0x120..+0x1D0, 176 bytes): locals 0..0x2F stay free for
        // outputs, the timers (locals 0x68 / 0x69 at +0x200) are not touched.
        constexpr uint32_t kCodeOffset = kScriptLocals + 0x30u * 4u;
        constexpr uint32_t kCodeMax = kScriptLocals + 0x5Cu * 4u - kCodeOffset;

        // ped fields
        constexpr uint32_t kPedSize = 0xCD0u;   // ped pool stride (GetAt 0x112068)
        constexpr uint32_t kPosition = 0x30u;   // CPlaceable matrix position (SET_CHAR_COORDINATES writes it)
        constexpr uint32_t kPedHealth = 0x4B8u; // float: SET_CHAR_HEALTH (0x0228) swc1 0x4B8
        constexpr uint32_t kPedState = 0x34Cu;  // u32: 0x32 driving, 0x36 / 0x37 dying / dead (lcs_input.cpp)
        constexpr uint32_t kPedWeapons = 0x594u; // 10 slots of 0x1C (REMOVE_ALL_CHAR_WEAPONS 0x1E16C8 loops 10 x 0x1C):
        constexpr uint32_t kWeaponSlotSize = 0x1Cu; // +0 type, +8 ammo in clip, +0xC ammo total (GiveWeapon 0x1E1180)
        constexpr int kWeaponSlots = 10;
        constexpr uint32_t kCarHealth = 0x268u; // float: SET_CAR_HEALTH (0x0229) swc1 0x268
        constexpr uint32_t kEntityModel = 0x58u; // u16 CEntity model index (read in a run)
        // CPlayerPed's CWanted, inside the ped: CLEAR_WANTED_LEVEL (0x0115, case 0x110F88) calls 0x34D770 = ped + 0x7F0
        // -> CWanted::SetWantedLevel 0x2F4C48; ALTER_WANTED_LEVEL_NO_DROP (0x0113) 0x34D790 -> ped + 0x7F0 ->
        // SetWantedLevelNoDrop 0x2F4D48. CWanted +0 chaos, +0x20 the level (stars; UpdateWantedLevel 0x2F49E8 writes it)
        constexpr uint32_t kPedWanted = 0x7F0u;
        constexpr uint32_t kWantedLevel = 0x20u;

        // ---- Script commands (LCS numbers; see the file comment) ---------------------------------------------------
        enum : uint16_t
        {
            TERMINATE = 0x004E,
            SET_CHAR_COORDINATES = 0x00A1, // char, x, y, z (z <= -100: the ground)
            CREATE_CAR = 0x00A5,           // model, x, y, z -> car
            DELETE_CAR = 0x00A6,           // [crossover] car (VC's number, as CREATE_CAR / GET_CAR_COORDINATES 0x00AA)
            WAIT = 0x0001,                 // [crossover] ms (VC's number, as TERMINATE 0x004E)
            GET_CAR_COORDINATES = 0x00AA,
            SET_CAR_COORDINATES = 0x00AB,
            SET_TIME_OF_DAY = 0x00C0,      // -> SetGameClock 0x1F87E0
            ADD_SCORE = 0x010E,            // player, money
            ALTER_WANTED_LEVEL_NO_DROP = 0x0113,
            CLEAR_WANTED_LEVEL = 0x0115,
            SET_MAX_WANTED_LEVEL = 0x01F5, // case 0x20C424: one parameter -> SetMaximumWantedLevel 0x2F4DA8
            SET_TIME_SCALE = 0x0162,
            GET_CHAR_HEADING = 0x0177,
            GET_CAR_HEADING = 0x0179,
            SET_CAR_HEADING = 0x017A,      // degrees (* 0.01745)
            GIVE_WEAPON_TO_CHAR = 0x01B7,  // char, type, ammo -> GiveWeapon 0x1E1180
            FORCE_WEATHER_NOW = 0x01BB,    // -> 0x325728 (old / new / forced weather)
            RELEASE_WEATHER = 0x01BC,      // forced weather -1
            SET_CURRENT_CHAR_WEAPON = 0x01BE,
            MARK_CAR_AS_NO_LONGER_NEEDED = 0x01C8,
            SET_POLICE_IGNORE_PLAYER = 0x01FC,
            SET_CHAR_HEALTH = 0x0228,
            SET_CAR_HEALTH = 0x0229,
            CHANGE_CAR_COLOUR = 0x022E,
            REQUEST_MODEL = 0x024C,
            MARK_MODEL_AS_NO_LONGER_NEEDED = 0x024E,
            SET_CHAR_PROOFS = 0x02B0,      // char + 5 bools
            SET_CAR_PROOFS = 0x02B1,
            RESTORE_CAMERA_JUMPCUT = 0x02F0, // -> 0x292CD8(TheCamera, 0)
            SET_PLAYER_NEVER_GETS_TIRED = 0x0335,
            SET_PLAYER_FAST_RELOAD = 0x0336,
            ADD_ARMOUR_TO_CHAR = 0x0364,   // armour +0x4BC, capped at 100
            WARP_CHAR_FROM_CAR_TO_COORD = 0x0367,
            WARP_CHAR_INTO_CAR = 0x036F,
            SET_EVERYONE_IGNORE_PLAYER = 0x03C4,
            STORE_CAR_CHAR_IS_IN_NO_SAVE = 0x03C5, // char -> car handle (GetIndex 0x1120E8 of ped +0x534)
            LOAD_SCENE = 0x03D0,
            SET_UPSIDEDOWN_CAR_NOT_DAMAGED = 0x03F2,
            SET_CAR_CAN_BE_DAMAGED = 0x03FA,
            GET_OFFSET_FROM_CAR_IN_WORLD_COORDS = 0x040C,
            LOAD_AND_LAUNCH_MISSION_INTERNAL = 0x041C, // mission table 0x65E170, missionRunning 0x3D9C29
            REMOVE_ALL_CHAR_WEAPONS = 0x0494,
            SET_CAR_FORWARD_SPEED = 0x04BF,
            SET_AREA_VISIBLE = 0x04C0,
            GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS = 0x04C9,
            SET_CHAR_DROWNS_IN_WATER = 0x04DD,
        };

        // Bytecode for one script. Arguments as VCS's (CollectParameters 0x1094C8: 1 zero, 6 int32, 7 int8, 8 int16,
        // 9 float), but a local variable is 0x0C + n (0x1098D8: < 0x0C the timers, 0x0C..0x6B locals), and LCS keeps
        // a player (index into CPlayerInfo, 0) apart from the player's char (a ped pool handle).
        class Scm
        {
        public:
            explicit Scm(int32_t me = 0) : m_me(me) {}
            Scm &op(uint16_t code) { return put(code & 0xFF).put(code >> 8); }
            Scm &i(int32_t v)
            {
                if (v == 0)
                    return put(0x01);
                if (v >= -128 && v < 128)
                    return put(0x07).put(static_cast<uint8_t>(v));
                if (v >= -32768 && v < 32768)
                    return put(0x08).put(v & 0xFF).put((v >> 8) & 0xFF);
                put(0x06);
                for (int k = 0; k < 4; ++k)
                    put((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF);
                return *this;
            }
            Scm &f(float v)
            {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                put(0x09);
                for (int k = 0; k < 4; ++k)
                    put((bits >> (8 * k)) & 0xFF);
                return *this;
            }
            Scm &b(bool v) { return i(v ? 1 : 0); }
            Scm &player() { return i(0); }   // Player parameters: CWorld::Players[0]
            Scm &me() { return i(m_me); }    // Char parameters: the player's ped handle
            Scm &local(int n) { return put(static_cast<uint8_t>(0x0C + n)); }
            Scm &car() { return local(9); }
            Scm &storePlayerCar() { return op(STORE_CAR_CHAR_IS_IN_NO_SAVE).me().car(); }
            bool empty() const { return code.empty(); }
            std::vector<uint8_t> finished() const
            {
                std::vector<uint8_t> out = code;
                out.push_back(TERMINATE & 0xFF);
                out.push_back(TERMINATE >> 8);
                return out;
            }

        private:
            Scm &put(uint32_t byte)
            {
                code.push_back(static_cast<uint8_t>(byte));
                return *this;
            }
            std::vector<uint8_t> code;
            int32_t m_me;
        };

        // ---- Static tables ------------------------------------------------------------------------------------------
        struct WeaponDef
        {
            int type;
            const char *textKey;
            const char *fallback;
            int ammo;
        };
        // LCS weapon types 1..36 (GiveWeapon 0x1E1180 takes 1..0x24). Names: LCS's text has no weapon names (no WEAPnn
        // keys), so ours; types checked against the weapon info read in a run (*0x3D9B48 + 4, 0x70 each: +0x68 the slot,
        // +0x60 the model): slot 0 type 1, slot 1 (melee) 2..11, slot 2 (thrown) 12..16, slot 3 (pistols) 17..18,
        // slot 4 (shotguns) 19..21, slot 5 (SMGs) 22..25, slot 6 (rifles) 26..27, slot 8 (snipers) 28..29, slot 7
        // (heavy) 30..33 and 35, slot 9 34 (its model is remote grenade 13's second model: the detonator) and 36.
        // Left out: 16 (thrown, model 273: unidentified) and 35 (slot 7 with the M60's model: the helicopter cannon).
        constexpr WeaponDef kWeapons[] = {
            {1, nullptr, "Brass Knuckles", 1},    {2, nullptr, "Screwdriver", 1},      {3, nullptr, "Golf Club", 1},
            {4, nullptr, "Nightstick", 1},        {5, nullptr, "Knife", 1},            {6, nullptr, "Baseball Bat", 1},
            {7, nullptr, "Hammer", 1},            {8, nullptr, "Meat Cleaver", 1},     {9, nullptr, "Machete", 1},
            {10, nullptr, "Katana", 1},           {11, nullptr, "Chainsaw", 1},        {12, nullptr, "Grenades", 25},
            {13, nullptr, "Remote Grenades", 25}, {14, nullptr, "Tear Gas", 25},       {15, nullptr, "Molotovs", 25},
            {17, nullptr, "Pistol", 200},         {18, nullptr, ".357", 200},          {19, nullptr, "Shotgun", 100},
            {20, nullptr, "Combat Shotgun", 100}, {21, nullptr, "Stubby Shotgun", 100}, {22, nullptr, "Tec-9", 500},
            {23, nullptr, "Micro SMG", 500},      {24, nullptr, "Silenced MAC-10", 500}, {25, nullptr, "MP5", 500},
            {26, nullptr, "M4", 500},             {27, nullptr, "AK-47", 500},         {28, nullptr, "Sniper Rifle", 50},
            {29, nullptr, "Laser-Scoped Rifle", 50}, {30, nullptr, "Rocket Launcher", 25}, {31, nullptr, "Flamethrower", 500},
            {32, nullptr, "M60", 500},            {33, nullptr, "Minigun", 1000},      {36, nullptr, "Camera", 50},
        };
        const WeaponDef *weaponDef(int type)
        {
            for (const WeaponDef &w : kWeapons)
                if (w.type == type)
                    return &w;
            return nullptr;
        }

        struct LoadoutDef
        {
            const char *name;
            const char *help;
            std::array<int, 8> types; // 0 ends the list early
        };
        constexpr LoadoutDef kLoadouts[] = {
            {"Street", "bat, molotovs, pistol, shotgun, micro SMG", {6, 15, 17, 19, 23, 0}},
            {"Military", "machete, grenades, .357, combat shotgun, AK-47, sniper rifle, rocket launcher",
             {9, 12, 18, 20, 27, 28, 30, 0}},
            {"Heavy", "chainsaw, grenades, .357, combat shotgun, MP5, M4, laser-scoped rifle, minigun",
             {11, 12, 18, 20, 25, 26, 29, 33}},
        };

        struct LandmarkDef
        {
            const char *zoneKey; // the district's name in the game's text
            const char *fallback;
            float x, y, z;       // a street-level spot in it; z the ground there + 1 (0: unknown, found with LOAD_SCENE)
        };
        // District names are GTA III's zone keys in LCS's text (CHINA "Chinatown", PORT_W "Callahan Point"...). The
        // spots lie in the game's own zone boxes (CTheZones' array, read in a run: 0x38 each, name[8] then min / max
        // x y z; e.g. CHINA 745..1065 x -908..-464) and were checked by teleporting there: the game names the district
        // on arrival, the player stands on a street (docs/ANDROID.md, "The trainer in LCS").
        constexpr LandmarkDef kLandmarks[] = {
            {"LITTLEI", "Saint Mark's", 1297.0f, -200.0f, 38.7f},
            {"REDLIGH", "Red Light District", 903.0f, -373.0f, 15.1f},
            {"CHINA", "Chinatown", 902.0f, -686.0f, 15.8f},
            {"TOWERS", "Hepburn Heights", 902.0f, -180.0f, 6.0f},
            {"HARWOOD", "Harwood", 1036.0f, 122.0f, 4.9f},
            {"PORT_I", "Trenton", 1147.0f, -850.0f, 16.0f},
            {"PORT_E", "Portland Harbor", 1497.0f, -700.0f, 12.8f},
            {"PORT_W", "Callahan Point", 906.0f, -1068.0f, 14.2f},
            {"PORT_S", "Atlantic Quays", 1281.0f, -1161.0f, 12.9f},
            {"EASTBAY", "Portland Beach", 1497.0f, -500.0f, 8.5f},
            {"S_VIEW", "Portland View", 1212.0f, -627.0f, 21.5f},
            {"CONSTRU", "Fort Staunton", 423.0f, -235.0f, 22.4f},
            {"STADIUM", "Aspatria", -58.0f, -44.0f, 17.2f},
            {"YAKUSA", "Torrington", 386.0f, -1366.0f, 27.2f},
            {"SHOPING", "Bedford Point", -54.0f, -1354.0f, 27.2f},
            {"COM_EAS", "Newport", 297.0f, -700.0f, 27.0f},
            {"PARK", "Belleville Park", -11.0f, -705.0f, 27.1f},
            {"UNIVERS", "Liberty Campus", 175.0f, -235.0f, 17.2f},
            {"HOSPI_2", "Rockford", 247.0f, 0.0f, 23.7f},
            {"AIRPORT", "Francis Intl. Airport", -1054.0f, -806.0f, 12.1f},
            {"PROJECT", "Wichita Gardens", -700.0f, -100.0f, 4.3f},
            {"SWANKS", "Cedar Grove", -570.0f, 396.0f, 94.1f},
            {"SUB_IND", "Pike Creek", -1113.0f, -87.0f, 47.9f},
            {"BIG_DAM", "Cochrane Dam", -1180.0f, 227.0f, 4.4f},
        };

        // Missions (setMissionScript): MAIN.SCM's mission table and each mission's SCRIPT_NAME. A story mission's
        // title is in the game's main text under its script name (VIC1 "Home Sweet Home", SAL1 "The Offer": the text
        // ENGLISH.GXT's MAIN table, read in a run); the story's are 3-4 letters and a digit (VIC1, SALS2, DONH6), from
        // VIC1 (index 9) on. The side jobs: the vehicle ones (started in their vehicle, as the game's own R3 does), by
        // script name, with their vehicle's game name (model info +0x2E; read in a run: AMBULAN 147, FIRETRK 138,
        // NOODLBO 204, PIZZABO 203, POLICAR 157, TAXI 151).
        struct JobDef
        {
            const char *script;  // SCRIPT_NAME
            const char *textKey; // its title in the main text, if any
            const char *fallback;
            const char *vehicle; // model game name
        };
        constexpr JobDef kJobs[] = {
            {"TAXIDR", nullptr, "Taxi", "TAXI"},          {"AMBULAN", nullptr, "Paramedic", "AMBULAN"},
            {"COPCAR", nullptr, "Vigilante", "POLICAR"},  {"FIRETRU", nullptr, "Firefighter", "FIRETRK"},
            {"PIZZA", nullptr, "Pizza Boy", "PIZZABO"},   {"NOODLE", nullptr, "Noodle Punk", "NOODLBO"},
        };
        // SCRIPT_NAME (0x03A9) and LOAD_MISSION_TEXT (0x0551): 8 raw bytes each (no type byte), in MAIN.SCM
        constexpr uint16_t kScriptNameOp = 0x03A9;
        constexpr uint32_t kFirstStoryMission = 9; // VIC1; 0..8 are the INIT* scripts the game runs at the start
        // (not the races RAC1..3 / BIK1..3: they start at their own markers)
        bool storyName(const std::string &k)
        {
            return k.compare(0, 3, "RAC") != 0 && k.compare(0, 3, "BIK") != 0 && k.size() >= 4 && k.size() <= 5 && std::isdigit(static_cast<unsigned char>(k.back())) &&
                   std::all_of(k.begin(), k.end() - 1, [](char c) { return c >= 'A' && c <= 'Z'; });
        }

        // FORCE_WEATHER_NOW's types: the game's own weather list (0x39A2E0, 64 entries) uses 0..5; 2 is rain (CWeather's
        // update 0x324C50 tests it). Names checked by looking: 5 is a dark storm, heavier rain than 2.
        constexpr const char *kWeatherNames[] = {"Sunny", "Cloudy", "Rainy", "Foggy", "Extra sunny", "Stormy"};
        constexpr int kCarColours = 64; // CHANGE_CAR_COLOUR refuses a colour >= 0x100 (case 0x25C6C8); carcols has fewer

        // Vehicle model info (0x190 each; +0x10 type 6): +0x2E the game name, also its text key (read in a run:
        // SPIDER, LANDSTK...), +0x38 its class (0x31F948: 1 = boat; in a run 0 car, 1 boat, 2 the train, 3 the script
        // helis, 4 a plane (AEROPL and the second DODO, 201: path planes nobody can fly - Toni stood on its roof), 5 bike,
        // 6 the ferry; the flyable Dodo, 164, is a car as in GTA III).
        constexpr uint32_t kModelGameName = 0x2Eu;
        constexpr uint32_t kModelClass = 0x38u;
        bool vehicleSpawnable(const uint8_t *ram, uint32_t mi)
        {
            uint32_t cls;
            std::memcpy(&cls, ram + ((mi + kModelClass) & kRamMask), 4);
            char key[9] = {};
            std::memcpy(key, ram + ((mi + kModelGameName) & kRamMask), 8);
            // the train, the ferry, the script-only HELI / AEROPL (the airliner) and the RC toys (they need RC mode)
            static const char *const kOut[] = {"TRAIN", "FERRY", "HELI", "AEROPL", "RCBANDT", "RCGOBLI", "RCRAIDE"};
            return cls != 2 && cls != 4 && cls != 6 &&
                   std::none_of(std::begin(kOut), std::end(kOut), [&](const char *o) { return std::strcmp(key, o) == 0; });
        }

        // ---- State ----------------------------------------------------------------------------------------------------
        std::array<std::atomic<bool>, ToggleCount> g_toggles{};
        std::atomic<bool> g_ready{false};
        std::atomic<bool> g_inVehicle{false};
        std::atomic<int> g_speed{100};
        std::atomic<int> g_clockHour{12};
        std::atomic<bool> g_missionRunning{false};
        std::atomic<int> g_wanted{0};
        bool g_wasFlying = false; // game thread: the free camera flew at the last frame
        int32_t g_me = 0; // game thread: the player's ped handle (as of the last frame)

        struct Action
        {
            std::function<bool(uint8_t *ram, Scm &code, std::string &notice)> build;
            ps2control::Reply reply;
            std::vector<int> models; // loaded before build() runs
        };
        std::mutex g_actionsMutex;
        std::deque<Action> g_actions;

        struct Pending
        {
            uint32_t script = 0;
            Action action;
            std::string notice;
        };
        std::vector<Pending> g_pending;
        uint32_t g_tickScript = 0;
        struct Loading
        {
            bool active = false, requested = false;
            Action action;
            uint64_t deadline = 0; // game milliseconds (g_lastGameTime)
            uint32_t script = 0;
            bool heldNoted = false;
        };
        Loading g_loading;
        // 10 s of game time to stream a model in (was 300 frames: 10 s at 30 fps, but 2 s at 144)
        constexpr uint64_t kLoadMs = 10000;
        uint32_t g_lastGameTime = 0;
        uint64_t g_runFrames = 0;
        uint32_t g_protectedCar = 0;
        int g_frozenHour = 12, g_frozenMinute = 0;
        std::array<int, 64> g_clipSize{};

        std::mutex g_namesMutex;
        uint32_t g_namesFrom = 0;
        std::vector<Entry> g_vehicleNames, g_weaponNames, g_landmarkNames, g_storyNames, g_jobNames;
        std::vector<std::string> g_missionScripts; // per mission index: its SCRIPT_NAME ("" if none); set before the game
        std::unordered_map<std::string, int> g_vehicleByKey; // model game name -> model id
        std::array<int, 64> g_weaponModel{}; // per weapon type, from the game's weapon info

        struct Saved
        {
            bool valid = false;
            float x = 0, y = 0, z = 0;
            uint32_t area = 0;
        };
        Saved g_saved;
        std::atomic<bool> g_hasSaved{false};

        std::string fmt(const char *format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        int rd16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        void post(const std::string &text) { vcs::trainer::notify(text); }

        void queue(std::function<bool(uint8_t *, Scm &, std::string &)> build, ps2control::Reply reply = {},
                   std::vector<int> models = {})
        {
            std::lock_guard<std::mutex> lock(g_actionsMutex);
            g_actions.push_back({std::move(build), std::move(reply), std::move(models)});
        }

        // ---- LCS's script VM: our own scripts (as vcs::rig::startCodeScript, debug/rig.cpp, with LCS's layout) ------
        uint32_t scriptSpace(const uint8_t *ram) { return rd32(ram, game::addrs().script.space); }

        // StartNewScript 0x10A210 done from the host: the head of the idle list, unlinked (0x10A1B8), an id, Init
        // 0x109A88 (zeroes, +0x217 = 1), the IP, linked at the head of the active list (0x10A1F0), active = 1.
        uint32_t startCodeScript(uint8_t *ram, const uint8_t *code, size_t bytes, const char *name)
        {
            const uint32_t space = scriptSpace(ram);
            const uint32_t s = rd32(ram, A.idleScripts);
            if (!space || !s || bytes > kCodeMax)
                return 0;
            const uint32_t next = rd32(ram, s), prev = rd32(ram, s + 4);
            if (prev)
                wr32(ram, prev, next);
            else
                wr32(ram, A.idleScripts, next);
            if (next)
                wr32(ram, next + 4, prev);
            const uint32_t id = rd32(ram, A.nextScriptId);
            wr32(ram, A.nextScriptId, id + 1);
            std::memset(ram + (s & kRamMask), 0, kScriptInit);
            wr32(ram, s + 8, id);
            ram[(s + kScriptUnknown217) & kRamMask] = 1;
            std::strncpy(reinterpret_cast<char *>(ram + ((s + kScriptName) & kRamMask)), name, 7);
            std::memcpy(ram + ((s + kCodeOffset) & kRamMask), code, bytes);
            wr32(ram, s + kScriptIp, s + kCodeOffset - space); // the interpreter adds ScriptSpace (32-bit wrap)
            const uint32_t head = rd32(ram, A.activeScripts);
            wr32(ram, s, head);
            wr32(ram, s + 4, 0);
            if (head)
                wr32(ram, head + 4, s);
            wr32(ram, A.activeScripts, s);
            ram[(s + kScriptActive) & kRamMask] = 1;
            return s;
        }

        bool inActiveList(const uint8_t *ram, uint32_t script)
        {
            for (uint32_t s = rd32(ram, A.activeScripts), n = 0; s && n < 512; s = rd32(ram, s), ++n)
                if (s == script)
                    return true;
            return false;
        }

        // Ours while active and its IP still in the slot's code (the game reuses a finished slot).
        bool scriptActive(const uint8_t *ram, uint32_t script)
        {
            if (!script || !ram[(script + kScriptActive) & kRamMask] || !inActiveList(ram, script))
                return false;
            const uint32_t at = rd32(ram, script + kScriptIp) + scriptSpace(ram);
            return at >= script + kCodeOffset && at < script + kCodeOffset + kCodeMax;
        }

        // Back to the idle list unrun, as TERMINATE_THIS_SCRIPT would (0x10CD94: unlink, link idle, active = 0).
        void cancelCodeScript(uint8_t *ram, uint32_t script)
        {
            if (!scriptActive(ram, script))
                return;
            const uint32_t next = rd32(ram, script), prev = rd32(ram, script + 4);
            if (prev)
                wr32(ram, prev, next);
            else
                wr32(ram, A.activeScripts, next);
            if (next)
                wr32(ram, next + 4, prev);
            const uint32_t head = rd32(ram, A.idleScripts);
            wr32(ram, script, head);
            wr32(ram, script + 4, 0);
            if (head)
                wr32(ram, head + 4, script);
            wr32(ram, A.idleScripts, script);
            ram[(script + kScriptActive) & kRamMask] = 0;
        }

        uint32_t start(uint8_t *ram, const Scm &code)
        {
            const std::vector<uint8_t> bytes = code.finished();
            return startCodeScript(ram, bytes.data(), bytes.size(), "trainer");
        }

        // The player's ped handle: CPool::GetIndex - (index << 8) | the slot's flags byte (GetAt 0x112068 checks it).
        int32_t pedHandle(const uint8_t *ram, uint32_t ped)
        {
            const uint32_t pool = rd32(ram, A.pedPool);
            if (!pool || !ped)
                return 0;
            const uint32_t objects = rd32(ram, pool), flags = rd32(ram, pool + 4);
            if (ped < objects || (ped - objects) % kPedSize)
                return 0;
            const uint32_t index = (ped - objects) / kPedSize;
            return static_cast<int32_t>((index << 8) | ram[(flags + index) & kRamMask]);
        }

        bool modelLoaded(const uint8_t *ram, int model)
        {
            const uint32_t infos = rd32(ram, A.streamingInfo);
            return infos && model >= 0 && ram[(infos + model * 0x14u + 0xC) & kRamMask] == 1;
        }

        uint32_t playerPed(const uint8_t *ram) { return rd32(ram, game::addrs().world.playerPed); }
        uint32_t pedVehicle(const uint8_t *ram, uint32_t ped)
        {
            const game::Addrs &G = game::addrs();
            return rd32(ram, ped + G.off.pedInVehicle) ? rd32(ram, ped + G.off.pedVehicle) : 0;
        }
        bool driving(const uint8_t *ram, uint32_t ped) { return pedVehicle(ram, ped) && rd32(ram, ped + kPedState) == 0x32; }
        bool alive(const uint8_t *ram, uint32_t ped)
        {
            const uint32_t state = rd32(ram, ped + kPedState);
            return state != 0x36 && state != 0x37;
        }

        // [guard] Free slots in the vehicle pool, as VCS's (vcs_trainer.cpp freeVehicleSlots). CREATE_CAR (0x00A5, case
        // in ProcessCommands 0x10DA48) takes its car from the pool *0x3D5AEC ({+0 objects, +4 one flag byte a slot: bit 7
        // free, +8 size (70), +0xC the last slot handed out}; 0x780 a slot) through 0x327978 -> 0x32F150, which returns 0
        // when none is free; the handler does not check and builds the vehicle at address 0 (the constructor 0x1221D0
        // writes its vtable to +0x5C): the same corruption / null call VCS died of. A trainer / border-arrival
        // CREATE_CAR checks first; the game frees slots by itself (its remove-when-full pass, game-internals.md).
        constexpr uint32_t kVehiclePool = 0x003D5AECu;
        int freeVehicleSlots(const uint8_t *ram)
        {
            const uint32_t pool = rd32(ram, kVehiclePool);
            if (!pool || pool >= 0x02000000u)
                return 0;
            const uint32_t flags = rd32(ram, pool + 4), size = rd32(ram, pool + 8);
            if (!flags || flags >= 0x02000000u || size > 1000)
                return 0;
            int n = 0;
            for (uint32_t i = 0; i < size; ++i)
                n += (ram[(flags + i) & kRamMask] & 0x80) != 0;
            return n;
        }
        constexpr int kSpawnFreeSlots = 2; // the script's car and one for the game's own spawners in the same frame
        // tests (`trainer poolreserve N`): N more slots asked for, so a "full" pool is simulated without touching it
        std::atomic<int> g_poolReserve{0};
        bool vehiclePoolFull(const uint8_t *ram) { return freeVehicleSlots(ram) < kSpawnFreeSlots + g_poolReserve.load(); }
        constexpr const char *kPoolFull = "Too many vehicles about: try again in a moment";

        // ---- Names from the game's text ---------------------------------------------------------------------------
        std::string keyAt(const uint8_t *ram, uint32_t a, uint32_t n = 8)
        {
            std::string key;
            for (uint32_t k = 0; k < n; ++k)
            {
                const char c = static_cast<char>(ram[(a + k) & kRamMask]);
                if (!c)
                    break;
                key += c;
            }
            return key;
        }

        std::string textAt(const uint8_t *ram, uint32_t a)
        {
            std::string out;
            bool token = false;
            for (int n = 0; n < 64; ++n, a += 2)
            {
                const uint16_t c = static_cast<uint16_t>(ram[a & kRamMask] | (ram[(a + 1) & kRamMask] << 8));
                if (!c)
                    break;
                if (c == '~')
                {
                    token = !token;
                    continue;
                }
                if (token)
                    continue;
                if (c < 0x80)
                    out += static_cast<char>(c);
                else if (c < 0x800)
                {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xE0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            return out;
        }

        std::string titleCase(std::string s)
        {
            bool first = true;
            for (char &c : s)
            {
                c = first ? static_cast<char>(std::toupper(c)) : static_cast<char>(std::tolower(c));
                first = c == ' ' || c == '-';
            }
            return s;
        }

        // Game thread: the name lists, once the game's text and models are in memory (again if the text is reloaded).
        void refreshNames(const uint8_t *ram)
        {
            const uint32_t table = rd32(ram, A.textTable);
            if (!table)
                return;
            const uint32_t entries = rd32(ram, table), count = rd32(ram, table + 4);
            static uint64_t lastTry = 0;
            if (!entries || !count || count > 20000 || entries == g_namesFrom || g_runFrames - lastTry < 30)
                return;
            lastTry = g_runFrames;
            const uint32_t modelInfos = rd32(ram, A.modelInfos), modelCount = rd32(ram, A.modelCount);
            const uint32_t weaponInfo = rd32(ram, A.weaponInfos);
            if (!modelInfos || !modelCount || modelCount > 10000 || !weaponInfo)
                return;
            std::unordered_map<std::string, std::string> text;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = entries + i * 12;
                text.emplace(keyAt(ram, e + 4), textAt(ram, rd32(ram, e)));
            }
            auto lookup = [&](const char *key, const std::string &fallback) {
                const auto it = key ? text.find(key) : text.end();
                return it != text.end() && !it->second.empty() ? it->second : fallback;
            };

            std::vector<Entry> vehicles;
            std::unordered_map<std::string, int> byKey;
            for (uint32_t id = 0; id < modelCount; ++id)
            {
                const uint32_t mi = rd32(ram, modelInfos + id * 4u);
                if (!mi || ram[(mi + 0x10) & kRamMask] != 6)
                    continue;
                const std::string key = keyAt(ram, mi + kModelGameName);
                if (key.empty())
                    continue;
                byKey.emplace(key, static_cast<int>(id));
                if (!vehicleSpawnable(ram, mi))
                    continue;
                // [crossover] the class: model info +0x38 (0 car, 1 boat, 2 train, 3 heli, 4 plane, 5 bike)
                // (the helicopters HUNTER .. VCNMAV 213-216 and the DODO 164 are class 0 automobiles with a helicopter /
                // plane handling record: listed as aircraft)
                const uint8_t c = ram[(mi + 0x38) & kRamMask];
                vehicles.push_back({lookup(key.c_str(), titleCase(key)), static_cast<int>(id),
                                    c == 1 ? "boat" : c == 3 || (id >= 213 && id <= 216) ? "heli" : c == 4 || id == 164 ? "plane"
                                    : c == 5 ? "bike" : "car"});
            }
            std::sort(vehicles.begin(), vehicles.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
            for (size_t k = 0; k < vehicles.size(); ++k)
            {
                size_t end = k + 1;
                while (end < vehicles.size() && vehicles[end].name == vehicles[k].name)
                    ++end;
                if (end - k > 1)
                    for (size_t n = k; n < end; ++n)
                        vehicles[n].name += fmt(" (%zu)", n - k + 1);
                k = end - 1;
            }

            std::array<int, 64> models{};
            const uint32_t infos = rd32(ram, weaponInfo + 4);
            std::vector<Entry> weapons;
            for (const WeaponDef &w : kWeapons)
            {
                models[w.type] = infos ? static_cast<int32_t>(rd32(ram, infos + w.type * 0x70u + 0x60)) : 0;
                weapons.push_back({lookup(w.textKey, w.fallback), w.type});
            }
            std::vector<Entry> landmarks;
            for (size_t k = 0; k < std::size(kLandmarks); ++k)
                landmarks.push_back({titleCase(lookup(kLandmarks[k].zoneKey, kLandmarks[k].fallback)), static_cast<int>(k)});
            std::sort(landmarks.begin(), landmarks.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });

            std::vector<Entry> story, jobs;
            for (size_t i = 0; i < g_missionScripts.size(); ++i)
            {
                const std::string &key = g_missionScripts[i];
                if (i >= kFirstStoryMission && storyName(key) && text.count(key))
                    story.push_back({lookup(key.c_str(), key), static_cast<int>(i)});
                for (const JobDef &j : kJobs)
                    if (key == j.script && byKey.count(j.vehicle))
                    {
                        std::string t = lookup(j.textKey, j.fallback);
                        if (t.size() > 2 && t.front() == '\'' && t.back() == '\'')
                            t = t.substr(1, t.size() - 2);
                        jobs.push_back({t, static_cast<int>(i)});
                    }
            }

            std::lock_guard<std::mutex> lock(g_namesMutex);
            g_storyNames = std::move(story);
            g_jobNames = std::move(jobs);
            g_vehicleByKey = std::move(byKey);
            g_vehicleNames = std::move(vehicles);
            g_weaponNames = std::move(weapons);
            g_landmarkNames = std::move(landmarks);
            g_weaponModel = models;
            if (!g_vehicleNames.empty())
                g_namesFrom = entries;
        }

        std::string nameOf(const std::vector<Entry> &list, int id, const std::string &fallback)
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            for (const Entry &e : list)
                if (e.id == id)
                    return e.name;
            return fallback;
        }

        // ---- The held toggles ------------------------------------------------------------------------------------
        void tick(uint8_t *ram, uint32_t ped)
        {
            const bool refresh = g_runFrames % 60 == 0;
            Scm code(g_me);
            if (lcs::trainer::toggle(GodMode))
            {
                if (rdf(ram, ped + kPedHealth) < 100.0f)
                    wrf(ram, ped + kPedHealth, 100.0f);
                if (refresh)
                {
                    code.op(SET_CHAR_PROOFS).me().b(true).b(true).b(true).b(true).b(true);
                    code.op(SET_CHAR_DROWNS_IN_WATER).me().b(false);
                }
            }
            if (lcs::trainer::toggle(NeverWanted))
                code.op(CLEAR_WANTED_LEVEL).player();
            if (lcs::trainer::toggle(UnlimitedAmmo) || lcs::trainer::toggle(NoReload))
                for (int s = 0; s < kWeaponSlots; ++s)
                {
                    const uint32_t slot = ped + kPedWeapons + s * kWeaponSlotSize;
                    const uint32_t type = rd32(ram, slot);
                    if (!weaponDef(static_cast<int>(type)) || weaponDef(static_cast<int>(type))->ammo <= 1)
                        continue; // unarmed, melee: nothing to count
                    const uint32_t clip = rd32(ram, slot + 8), total = rd32(ram, slot + 12);
                    g_clipSize[type] = std::max<int>(g_clipSize[type], static_cast<int>(clip));
                    if (lcs::trainer::toggle(UnlimitedAmmo) && total < 9000)
                        wr32(ram, slot + 12, 9999);
                    const uint32_t left = rd32(ram, slot + 12);
                    if (lcs::trainer::toggle(NoReload) && static_cast<int>(clip) < g_clipSize[type] && left > clip)
                        wr32(ram, slot + 8, std::min<uint32_t>(g_clipSize[type], left));
                }
            if (refresh)
            {
                if (lcs::trainer::toggle(InfiniteSprint))
                    code.op(SET_PLAYER_NEVER_GETS_TIRED).player().b(true);
                if (lcs::trainer::toggle(FastReload))
                    code.op(SET_PLAYER_FAST_RELOAD).player().b(true);
                if (lcs::trainer::toggle(PoliceIgnore))
                    code.op(SET_POLICE_IGNORE_PLAYER).player().b(true);
                if (lcs::trainer::toggle(EveryoneIgnores))
                    code.op(SET_EVERYONE_IGNORE_PLAYER).player().b(true);
            }
            if (lcs::trainer::toggle(VehicleInvincible) && driving(ram, ped))
            {
                const uint32_t car = pedVehicle(ram, ped);
                if (rdf(ram, car + kCarHealth) < 1000.0f)
                    wrf(ram, car + kCarHealth, 1000.0f);
                if (car != g_protectedCar || refresh)
                {
                    code.storePlayerCar();
                    code.op(SET_CAR_PROOFS).car().b(true).b(true).b(true).b(true).b(true);
                    code.op(SET_CAR_CAN_BE_DAMAGED).car().b(false);
                    code.op(SET_UPSIDEDOWN_CAR_NOT_DAMAGED).car().b(true);
                    g_protectedCar = car;
                }
            }
            if (lcs::trainer::toggle(FreezeTime))
                code.op(SET_TIME_OF_DAY).i(g_frozenHour).i(g_frozenMinute);
            if (!code.empty())
                g_tickScript = start(ram, code);
        }

        void applyToggle(Toggle t, bool on)
        {
            queue([t, on](uint8_t *ram, Scm &code, std::string &notice) {
                static const char *const names[ToggleCount] = {"God mode", "Never wanted", "Unlimited ammo", "No reload",
                                                               "Infinite sprint", "Fast reload", "Police ignore you",
                                                               "Everyone ignores you", "Vehicle invincibility",
                                                               "Get into spawned vehicles", "Freeze time", "Free camera",
                                                               "Hide HUD"};
                notice = std::string(names[t]) + (on ? " on" : " off");
                const uint32_t ped = playerPed(ram);
                switch (t)
                {
                case GodMode:
                    code.op(SET_CHAR_PROOFS).me().b(on).b(on).b(on).b(on).b(on);
                    code.op(SET_CHAR_DROWNS_IN_WATER).me().b(!on);
                    if (on)
                        code.op(SET_CHAR_HEALTH).me().i(100);
                    break;
                case NeverWanted:
                    if (on)
                        code.op(CLEAR_WANTED_LEVEL).player();
                    break;
                case InfiniteSprint: code.op(SET_PLAYER_NEVER_GETS_TIRED).player().b(on); break;
                case FastReload: code.op(SET_PLAYER_FAST_RELOAD).player().b(on); break;
                case PoliceIgnore: code.op(SET_POLICE_IGNORE_PLAYER).player().b(on); break;
                case EveryoneIgnores: code.op(SET_EVERYONE_IGNORE_PLAYER).player().b(on); break;
                case VehicleInvincible:
                    g_protectedCar = 0;
                    if (driving(ram, ped))
                    {
                        code.storePlayerCar();
                        code.op(SET_CAR_PROOFS).car().b(on).b(on).b(on).b(on).b(on);
                        code.op(SET_CAR_CAN_BE_DAMAGED).car().b(!on);
                        code.op(SET_UPSIDEDOWN_CAR_NOT_DAMAGED).car().b(on);
                    }
                    break;
                case FreezeTime:
                    g_frozenHour = ram[A.clockHours & kRamMask];
                    g_frozenMinute = ram[A.clockMinutes & kRamMask];
                    if (on)
                        notice += fmt(" at %02d:%02d", g_frozenHour, g_frozenMinute);
                    break;
                default: break;
                }
                return true;
            });
        }

        // ---- The game thread's queue (as vcs_trainer.cpp) -------------------------------------------------------
        void finish(Pending &p, bool ok, const std::string &why = {})
        {
            const std::string text = ok ? p.notice : why;
            if (!text.empty())
                post(text);
            if (p.action.reply)
                p.action.reply(ok ? "ok " + p.notice : "error " + why);
        }

        void runActions(uint8_t *ram, uint32_t ped)
        {
            for (;;)
            {
                Action a;
                if (g_loading.active)
                {
                    // A cutscene streams its own data and the game loads nothing else meanwhile: a model asked for in
                    // the mission-1 restart cutscene stayed requested (state 2) for all of it, and the action failed
                    // with "the game's memory for models is full" (QA 2026-11-18). Held until the camera is the
                    // player's again, then loaded as usual.
                    if (rd32(ram, A.cameraLooksAtPlayer) != 1)
                    {
                        g_loading.deadline = g_lastGameTime + kLoadMs;
                        if (!g_loading.heldNoted)
                            post("After the cutscene");
                        g_loading.heldNoted = true;
                        return;
                    }
                    if (!g_loading.requested)
                    {
                        Scm request(g_me);
                        for (int m : g_loading.action.models)
                            request.op(REQUEST_MODEL).i(m);
                        g_loading.script = start(ram, request);
                        g_loading.requested = g_loading.script != 0;
                        return;
                    }
                    const bool loaded = std::all_of(g_loading.action.models.begin(), g_loading.action.models.end(),
                                                    [ram](int m) { return modelLoaded(ram, m); });
                    if (!loaded && g_lastGameTime < g_loading.deadline)
                        return;
                    a = std::move(g_loading.action);
                    g_loading.active = false;
                    if (!loaded)
                    {
                        Scm release(g_me);
                        for (int m : a.models)
                            release.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(m);
                        start(ram, release);
                        Pending p{0, std::move(a), {}};
                        finish(p, false, "Could not load it in time: the game is busy streaming here (try again, leave "
                                         "the area or lose some spawned vehicles)");
                        return;
                    }
                    a.models.clear();
                }
                else
                {
                    std::lock_guard<std::mutex> lock(g_actionsMutex);
                    if (g_actions.empty())
                        return;
                    a = std::move(g_actions.front());
                    g_actions.pop_front();
                }
                if (!a.models.empty())
                {
                    g_loading = {true, false, std::move(a), g_lastGameTime + kLoadMs, 0, false};
                    continue;
                }
                Pending p{0, std::move(a), {}};
                Scm code(g_me);
                std::string notice;
                if (!alive(ram, ped))
                {
                    finish(p, false, "Not now: the player is down");
                    continue;
                }
                if (!p.action.build(ram, code, notice))
                {
                    finish(p, false, notice);
                    continue;
                }
                p.notice = notice;
                if (code.empty())
                {
                    finish(p, true);
                    continue;
                }
                p.script = start(ram, code);
                if (!p.script)
                {
                    finish(p, false, code.finished().size() > kCodeMax ? "Internal error: script too long"
                                                                         : "The game's script slots are all in use");
                    continue;
                }
                g_pending.push_back(std::move(p));
                return;
            }
        }

        std::vector<std::string> words(const std::string &args)
        {
            std::istringstream in(args);
            std::vector<std::string> out;
            for (std::string w; in >> w;)
                out.push_back(w);
            return out;
        }

        std::string listReply(const std::vector<Entry> &list)
        {
            std::string out = "ok";
            for (const Entry &e : list)
                out += fmt(" %d:", e.id) + e.name + ";";
            return out;
        }

        void moveTo(Scm &code, uint32_t area, float x, float y, float z);
        const char *spawnCar(uint8_t *ram, Scm &code, int model, bool warp);
        int weaponModel(int type);

        // Control channel: trainer <command> [arg] (the VCS command's verbs that LCS has, and goto).
        void command(const std::string &args, ps2control::Reply reply)
        {
            static const std::pair<const char *, Toggle> kToggleNames[] = {
                {"god", GodMode},         {"wanted", NeverWanted},     {"ammo", UnlimitedAmmo},
                {"reload", NoReload},     {"sprint", InfiniteSprint},  {"fastreload", FastReload},
                {"police", PoliceIgnore}, {"ignore", EveryoneIgnores}, {"vehicle", VehicleInvincible},
                {"warp", WarpIntoSpawned}, {"freeze", FreezeTime},   {"freecam", FreeCamera},
                {"hidehud", HideHud},
            };
            const auto w = words(args);
            const std::string verb = w.empty() ? "" : w[0];
            const int arg = w.size() > 1 ? std::atoi(w[1].c_str()) : 0;
            auto answerWhenDone = [&reply] {
                queue([](uint8_t *, Scm &, std::string &) { return true; }, [reply](const std::string &) {
                    const std::vector<Notice> n = notices(5.0);
                    reply("ok " + (n.empty() ? std::string() : n.back().text));
                });
            };
            if (verb == "state")
            {
                std::string out = fmt("ok ready=%d vehicle=%d speed=%d mission=%d me=%d stars=%d", ready() ? 1 : 0,
                                      inVehicle() ? 1 : 0, gameSpeed(), missionRunning() ? 1 : 0, g_me, g_wanted.load());
                for (const auto &[name, t] : kToggleNames)
                    out += fmt(" %s=%d", name, lcs::trainer::toggle(t) ? 1 : 0);
                return reply(out);
            }
            if (verb == "toggle" && w.size() >= 3)
            {
                for (const auto &[name, t] : kToggleNames)
                    if (w[1] == name)
                    {
                        lcs::trainer::setToggle(t, w[2] == "1" || w[2] == "on");
                        return answerWhenDone();
                    }
                return reply("error unknown toggle");
            }
            if (verb == "list" && w.size() >= 2)
            {
                if (w[1] == "vehicles")
                    return reply(listReply(vehicles()));
                if (w[1] == "weapons")
                    return reply(listReply(weapons()));
                if (w[1] == "loadouts")
                    return reply(listReply(loadouts()));
                if (w[1] == "landmarks")
                    return reply(listReply(landmarks()));
                if (w[1] == "weathers")
                    return reply(listReply(weathers()));
                if (w[1] == "missions")
                    return reply(listReply(missions(true)));
                if (w[1] == "jobs")
                    return reply(listReply(missions(false)));
                return reply("error usage: trainer list vehicles|weapons|loadouts|landmarks|weathers|missions|jobs");
            }
            if (verb == "freecam" && w.size() >= 7) // tests: put the flying camera at x y z yaw pitch fov (radians, degrees)
            {
                auto f = [&w](size_t i) { return std::strtof(w[i].c_str(), nullptr); };
                lcs::freecam::setPose(f(1), f(2), f(3), f(4), f(5), f(6));
                return reply("ok");
            }
            if (verb == "freecam") // the free camera as of its last update
            {
                const vcs::freecam::Pose p = lcs::freecam::pose();
                return reply(fmt("ok flying=%d active=%d x=%.2f y=%.2f z=%.2f yaw=%.3f pitch=%.3f fov=%.1f speed=%d",
                                 lcs::freecam::flying() ? 1 : 0, p.active ? 1 : 0, p.x, p.y, p.z, p.yaw, p.pitch, p.fov,
                                 vcs::trainer::freeCameraSpeed()));
            }
            if (verb == "notices")
            {
                std::string out = "ok";
                for (const Notice &n : notices(1e9))
                    out += " " + n.text + ";";
                return reply(out);
            }
            if (!ready())
                return reply("error not in game");
            if (verb == "goto" && w.size() >= 4) // tests: the player to x y z (z <= -100: the ground, with LOAD_SCENE)
            {
                const float x = std::strtof(w[1].c_str(), nullptr), y = std::strtof(w[2].c_str(), nullptr),
                            z = std::strtof(w[3].c_str(), nullptr);
                queue([x, y, z](uint8_t *, Scm &code, std::string &notice) {
                    moveTo(code, 0, x, y, z);
                    notice = fmt("At (%.0f, %.0f, %.0f)", x, y, z);
                    return true;
                });
                return answerWhenDone();
            }
            if (verb == "poolreserve" && w.size() >= 2) // [guard] tests: vehiclePoolFull's extra slots (0 = off)
            {
                g_poolReserve = std::atoi(w[1].c_str());
                return reply(fmt("ok reserve %d", g_poolReserve.load()));
            }
            if (verb == "arrive" && w.size() >= 7) // [guard] tests: the border arrival, as VCS's verb (x y z heading model
            {                                      // speed; model 0 = on foot); answered before a retried arrival ends
                auto f = [&w](size_t i) { return std::strtof(w[i].c_str(), nullptr); };
                lcs::trainer::arrive(f(1), f(2), f(3), f(4), std::atoi(w[5].c_str()), f(6));
                return answerWhenDone();
            }
            if (verb == "mountgun" && w.size() >= 3) // tests: a car of model w[1] spawned ahead, the player on foot
            {                                        // put on it with weapon w[2] (ATTACH_CHAR_TO_CAR, as missions do)
                const int model = std::atoi(w[1].c_str()), weapon = std::atoi(w[2].c_str());
                // The gun's model is streamed in first, as the missions do (REQUEST_MODEL before 0x0469: SALH5, SALS3,
                // RAYS1): without it the attack starts (ped state 0x10) but no shot is ever fired (clip stays full) -
                // the aim2 agent's "no button fires" (game-internals.md, "Guns on vehicles").
                const int gunModel = weaponModel(weapon);
                std::vector<int> models = {model};
                if (gunModel > 0)
                    models.push_back(gunModel);
                queue([model, weapon, gunModel](uint8_t *ram, Scm &code, std::string &notice) {
                    if (const char *why = spawnCar(ram, code, model, false))
                        return (notice = why, false);
                    // 0x0469 (case in 0x269200 -> 0x1F08D0; 0x04F9 in 0x185218 is the object pool's): char, car, x, y, z offset,
                    // position, angle limit (deg), weapon; the player's camera goes to mode 45 (the gun on a vehicle)
                    code.op(0x0469).me().local(0).f(0.0f).f(-1.0f).f(1.2f).i(1).f(70.0f).i(weapon);
                    if (gunModel > 0) // the ped holds its own reference now (as giveWeapon's addWeapon)
                        code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(gunModel);
                    notice = "Mounted";
                    return true;
                }, [reply](const std::string &n) { reply("ok " + n); }, models);
                return;
            }
            using Fn = std::function<void()>;
            const std::pair<const char *, Fn> kActions[] = {
                {"heal", [] { heal(); }},
                {"money", [arg] { addMoney(arg); }},
                {"setwanted", [arg] { setWanted(arg); }},
                {"weapon", [arg] { giveWeapon(arg); }},
                {"loadout", [arg] { giveLoadout(arg); }},
                {"noweapons", [] { removeWeapons(); }},
                {"spawn", [arg] { spawnVehicle(arg); }},
                {"repair", [] { repairVehicle(); }},
                {"flip", [] { flipVehicle(); }},
                {"boost", [] { boostVehicle(); }},
                {"paint", [] { paintVehicle(); }},
                {"teleport", [arg] { teleport(arg); }},
                {"savepos", [] { savePosition(); }},
                {"returnpos", [] { returnToSaved(); }},
                {"time", [arg] { setTime(arg); }},
                {"weather", [arg] { setWeather(arg); }},
                {"speed", [arg] { setGameSpeed(arg); }},
                {"camtp", [] { teleportToCamera(); }},
                {"mission", [arg] { launchMission(arg); }},
                {"camspeed", [arg] { vcs::trainer::setFreeCameraSpeed(arg); }},
            };
            for (const auto &[name, fn] : kActions)
                if (verb == name)
                {
                    fn();
                    return answerWhenDone();
                }
            reply("error usage: trainer state | toggle <name> 0|1 | list <what> | notices | heal | money N | setwanted N | "
                  "weapon T | loadout N | noweapons | spawn MODEL | repair | flip | boost | paint | teleport N | savepos | "
                  "returnpos | camtp | camspeed M | freecam | time H | weather W | speed PERCENT | mission N");
        }

        // Game thread, before each CTheScripts::Process: what vcs::trainer::onGameFrame does at VCS's frame kick.
        void onGameFrame(uint8_t *ram)
        {
            const game::Addrs &G = game::addrs();
            const uint32_t time = rd32(ram, G.timer.timeMs);
            const bool running = time != g_lastGameTime;
            g_lastGameTime = time;
            const uint32_t ped = playerPed(ram);
            g_me = pedHandle(ram, ped);
            const bool inGame = scriptSpace(ram) != 0 && ped != 0 && g_me != 0;
            g_ready = inGame;
            g_inVehicle = inGame && driving(ram, ped);
            g_clockHour = ram[A.clockHours & kRamMask];
            g_missionRunning = inGame && ram[G.script.missionRunning & kRamMask] != 0;
            g_wanted = inGame ? static_cast<int32_t>(rd32(ram, ped + kPedWanted + kWantedLevel)) : 0;
            refreshNames(ram);
            if (!inGame)
                g_toggles[FreeCamera] = false; // (a load, the title screen): the camera is the game's again

            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                if (!scriptActive(ram, it->script))
                {
                    finish(*it, true);
                    it = g_pending.erase(it);
                }
                else if (!running)
                {
                    cancelCodeScript(ram, it->script);
                    std::lock_guard<std::mutex> lock(g_actionsMutex);
                    g_actions.push_front(std::move(it->action));
                    it = g_pending.erase(it);
                }
                else
                    ++it;
            }
            if (scriptActive(ram, g_tickScript) && !running)
                cancelCodeScript(ram, g_tickScript);
            if (g_loading.active && scriptActive(ram, g_loading.script) && !running)
            {
                cancelCodeScript(ram, g_loading.script);
                g_loading.requested = false;
            }
            if (!running || !inGame || !g_pending.empty())
                return;
            ++g_runFrames;
            // the free camera ended: the game's camera back behind the player at once (RESTORE_CAMERA_JUMPCUT), not
            // while a script camera holds it (as vcs_trainer.cpp)
            const bool flying = lcs::freecam::flying();
            if (g_wasFlying && !flying && rd32(ram, A.cameraLooksAtPlayer) == 1)
            {
                Scm code(g_me);
                code.op(RESTORE_CAMERA_JUMPCUT);
                start(ram, code);
            }
            g_wasFlying = flying;
            runActions(ram, ped);
            if (g_runFrames % 10 == 0 && alive(ram, ped) && !scriptActive(ram, g_tickScript))
                tick(ram, ped);
        }

        PS2_GUEST_CC void scriptsProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // (the rig's game-thread jobs - poke, dump, pause: debug/rig.h - run at the frame kick, game/lcs_frame.cpp,
            // once per frame in menus and loading screens too)
            onGameFrame(rdram);
            game::call(A.scriptsProcess, rdram, ctx, runtime);
        }

        // God mode in a vehicle that blows up: KillPedsInVehicle kills the occupants with SetDead whatever their proofs
        // (QA 2026-11-18: a spawned Maverick set to health 0 with god mode on -> Wasted). The player sitting in it
        // (state 0x32) is left alive instead; on foot the proofs already keep explosions, fire and falls off him.
        PS2_GUEST_CC void pedSetDead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = GPR_U32(ctx, 4);
            if (lcs::trainer::toggle(GodMode) && ped && ped == playerPed(rdram) && rd32(rdram, ped + kPedState) == 0x32)
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            game::call(A.pedSetDead, rdram, ctx, runtime);
        }

        // A known height: no LOAD_SCENE. LOAD_SCENE loads the area inside the frame (CStreaming 0x18FC10), and in this
        // port a synchronous stream load can wait for good (the game thread spun in 0x191780 polling 0x150DA8 after the
        // 6th teleport of a run, 2026-11-18: the LCS streaming hang of the cutscene skip). Placed at its height, the
        // player waits for the area's collision as after any long move, and the game streams the rest in.
        void moveTo(Scm &code, uint32_t area, float x, float y, float z)
        {
            code.op(SET_AREA_VISIBLE).i(static_cast<int32_t>(area));
            if (z < -99.0f)
                code.op(LOAD_SCENE).f(x).f(y).f(20.0f);
            code.op(SET_CHAR_COORDINATES).me().f(x).f(y).f(z);
        }

        void addWeapon(Scm &code, const WeaponDef &w, int model, bool select)
        {
            if (model > 0)
                code.op(REQUEST_MODEL).i(model);
            code.op(GIVE_WEAPON_TO_CHAR).me().i(w.type).i(w.ammo);
            if (select)
                code.op(SET_CURRENT_CHAR_WEAPON).me().i(w.type);
            if (model > 0)
                code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(model);
        }

        int weaponModel(int type)
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            return type >= 0 && type < static_cast<int>(g_weaponModel.size()) ? g_weaponModel[type] : 0;
        }

        std::vector<int> modelList(int model) { return model > 0 ? std::vector<int>{model} : std::vector<int>{}; }

        void vehicleAction(const char *done, std::function<void(Scm &)> body)
        {
            queue([done, body](uint8_t *ram, Scm &code, std::string &notice) {
                if (!driving(ram, playerPed(ram)))
                    return (notice = "Not in a vehicle", false);
                code.storePlayerCar();
                body(code);
                notice = done;
                return true;
            });
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_toggles[WarpIntoSpawned] = true;
        runtime.replaceFunction(A.scriptsProcess, &scriptsProcess);
        runtime.replaceFunction(A.pedSetDead, &pedSetDead);
        ps2control::addCommand("trainer", command);
        std::fprintf(stderr, "[lcs] trainer installed (frame hook at CTheScripts::Process 0x%x)\n", A.scriptsProcess);
    }

    bool toggle(Toggle t) { return t >= 0 && t < ToggleCount && g_toggles[t].load(std::memory_order_relaxed); }

    void setToggle(Toggle t, bool on)
    {
        if (t == FreeCamera && on && !lcs::freecam::installed())
            return post("The free camera is off in this run (LCS_FREECAM=0)");
        if (t < 0 || t >= ToggleCount || g_toggles[t].exchange(on) == on)
            return;
        applyToggle(t, on);
    }

    bool ready() { return g_ready.load(std::memory_order_relaxed); }
    bool inVehicle() { return g_inVehicle.load(std::memory_order_relaxed); }

    void heal()
    {
        queue([](uint8_t *, Scm &code, std::string &notice) {
            code.op(SET_CHAR_HEALTH).me().i(100);
            code.op(ADD_ARMOUR_TO_CHAR).me().i(100);
            notice = "Health and armour filled";
            return true;
        });
    }

    void addMoney(int amount)
    {
        queue([amount](uint8_t *, Scm &code, std::string &notice) {
            code.op(ADD_SCORE).player().i(amount);
            notice = fmt("$%d added", amount);
            return true;
        });
    }

    void setWanted(int stars)
    {
        stars = std::clamp(stars, 0, 6);
        queue([stars](uint8_t *ram, Scm &code, std::string &notice) {
            if (stars > 0 && lcs::trainer::toggle(NeverWanted))
                return (notice = "Never wanted is on", false);
            code.op(CLEAR_WANTED_LEVEL).player();
            notice = stars ? fmt("Wanted level %d", stars) : "Wanted level cleared";
            if (stars > 0)
            {
                // the game caps the level at its maximum (a mission may hold it at 0): raised as far as asked
                const int32_t max = static_cast<int32_t>(rd32(ram, A.maxWantedLevel));
                if (max < stars)
                {
                    code.op(SET_MAX_WANTED_LEVEL).i(stars);
                    notice += fmt(" (the limit of %d raised)", max);
                }
                code.op(ALTER_WANTED_LEVEL_NO_DROP).player().i(stars);
            }
            return true;
        });
    }

    void giveWeapon(int type)
    {
        const WeaponDef *w = weaponDef(type);
        if (!w)
            return post("No such weapon");
        const std::string name = nameOf(g_weaponNames, type, w->fallback);
        const int model = weaponModel(type);
        queue([w, name, model](uint8_t *, Scm &code, std::string &notice) {
            g_clipSize[w->type] = 0;
            addWeapon(code, *w, model, true);
            notice = name + " given";
            return true;
        }, {}, modelList(model));
    }

    void giveLoadout(int which)
    {
        if (which < 0 || which >= static_cast<int>(std::size(kLoadouts)))
            return post("No such loadout");
        const LoadoutDef &l = kLoadouts[which];
        for (size_t k = 0; k < l.types.size() && l.types[k]; ++k)
            if (const WeaponDef *w = weaponDef(l.types[k]))
            {
                const int model = weaponModel(w->type);
                queue([w, model](uint8_t *, Scm &code, std::string &) { return (addWeapon(code, *w, model, false), true); },
                      {}, modelList(model));
            }
        queue([&l](uint8_t *, Scm &, std::string &notice) { return (notice = std::string(l.name) + " loadout given", true); });
    }

    void removeWeapons()
    {
        queue([](uint8_t *, Scm &code, std::string &notice) {
            code.op(REMOVE_ALL_CHAR_WEAPONS).me();
            notice = "Weapons removed";
            return true;
        });
    }

    namespace
    {
        // [crossover] a VCS vehicle bound to a model id of its own (game/lcs_crossover.h); every id in use: the oldest
        // VCS vehicles (not the player's) deleted by this action's script and the bind tried again (up to 30 times, a
        // frame or more apart) - a spawn is not refused while one can be freed
        void spawnOther(int other, const std::string &name, int attempt)
        {
            queue([other, name, attempt](uint8_t *ram, Scm &code, std::string &notice) {
                std::string why;
                std::vector<int32_t> evict;
                bool retry = false;
                const int slot = crossover::bind(ram, other, why, &evict, &retry);
                if (!slot && retry && attempt < 30)
                {
                    for (int32_t car : evict)
                        code.op(DELETE_CAR).i(car);
                    if (evict.empty())
                        code.op(WAIT).i(0); // a frame for the model to be unloaded (crossover::onFrame)
                    spawnOther(other, name, attempt + 1);
                    notice.clear();
                    return true;
                }
                if (!slot)
                    return notice = why, false;
                queue([slot, name](uint8_t *ram2, Scm &code2, std::string &notice2) {
                    if (const char *why = spawnCar(ram2, code2, slot, lcs::trainer::toggle(WarpIntoSpawned)))
                        return (notice2 = why, false);
                    notice2 = name + " spawned";
                    return true;
                }, {}, {slot});
                notice.clear();
                return false;
            });
        }
    }

    void spawnVehicle(int model)
    {
        if (model >= crossover::kIdBase) // [crossover] a VCS vehicle: bound to a model id first, then spawned there
        {
            const crossover::Vehicle *v = crossover::find(model - crossover::kIdBase);
            if (!v)
                return post("No such vehicle");
            spawnOther(v->id, v->label, 0);
            return;
        }
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            for (const Entry &e : g_vehicleNames)
                known = known || e.id == model;
        }
        if (!known)
            return post("No such vehicle");
        const std::string name = nameOf(g_vehicleNames, model, fmt("Vehicle %d", model));
        queue([model, name](uint8_t *ram, Scm &code, std::string &notice) {
            if (const char *why = spawnCar(ram, code, model, lcs::trainer::toggle(WarpIntoSpawned)))
                return (notice = why, false);
            notice = name + " spawned";
            return true;
        }, {}, {model});
    }

    namespace
    {
        // A car of `model` (loaded) ahead of the player, facing his way; `warp`: him in it. Nullptr when added, else why
        // nothing was: the vehicle pool (nearly) full (freeVehicleSlots), or the player swimming / in a boat and `model` no boat
        const char *spawnCar(uint8_t *ram, Scm &code, int model, bool warp)
        {
            if (vehiclePoolFull(ram))
                return kPoolFull;
            const bool inCar = driving(ram, playerPed(ram));
            // [guard] a swimming player (lcs_swim) warped into a car / helicopter / plane made in the sea under him goes
            // down with it and drowns inside (the swimming keeps only a ped on foot afloat): health 100 -> 11 in 5.7 s
            // with a Dodo (tools: guard's swimspawn.py); the stress tests' "player is down" after an on-foot arrival in
            // the sea. Only a boat is made there. Model info +0x38 is the class (1 boat; vehicleSpawnable's note).
            // The same from a boat: the new vehicle is made 8 m ahead of it, on the sea.
            auto isBoat = [ram](int m) {
                const uint32_t infos = rd32(ram, A.modelInfos);
                const uint32_t mi = m >= 0 && static_cast<uint32_t>(m) < rd32(ram, A.modelCount)
                                        ? rd32(ram, infos + static_cast<uint32_t>(m) * 4u) : 0u;
                return mi && ram[(mi + kModelClass) & kRamMask] == 1;
            };
            if (warp && !isBoat(model) &&
                (inCar ? isBoat(rd16(ram, pedVehicle(ram, playerPed(ram)) + kEntityModel)) : lcs::swim::swimming()))
                return "On the water only a boat can be spawned: get onto land first";
            if (inCar)
            {
                code.storePlayerCar();
                code.op(GET_OFFSET_FROM_CAR_IN_WORLD_COORDS).car().f(0.0f).f(8.0f).f(0.5f).local(1).local(2).local(3);
                code.op(GET_CAR_HEADING).car().local(4);
            }
            else
            {
                code.op(GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS).me().f(0.0f).f(5.0f).f(0.0f).local(1).local(2).local(3);
                code.op(GET_CHAR_HEADING).me().local(4);
            }
            code.op(REQUEST_MODEL).i(model);
            code.op(CREATE_CAR).i(model).local(1).local(2).local(3).local(0);
            code.op(SET_CAR_HEADING).local(0).local(4);
            if (warp)
            {
                if (inCar)
                    code.op(WARP_CHAR_FROM_CAR_TO_COORD).me().local(1).local(2).local(3);
                code.op(WARP_CHAR_INTO_CAR).me().local(0);
                if (inCar && rd32(ram, A.cameraLooksAtPlayer) != 0)
                    code.op(RESTORE_CAMERA_JUMPCUT);
            }
            code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(model);
            code.op(MARK_CAR_AS_NO_LONGER_NEEDED).local(0);
            g_protectedCar = 0;
            return nullptr;
        }
    }

    void repairVehicle()
    {
        vehicleAction("Vehicle repaired", [](Scm &code) { code.op(SET_CAR_HEALTH).car().i(1000); });
    }

    void flipVehicle()
    {
        vehicleAction("Vehicle flipped upright", [](Scm &code) {
            code.op(GET_CAR_COORDINATES).car().local(1).local(2).local(3);
            code.op(GET_CAR_HEADING).car().local(4);
            code.op(SET_CAR_COORDINATES).car().local(1).local(2).local(3);
            code.op(SET_CAR_HEADING).car().local(4);
        });
    }

    void boostVehicle()
    {
        vehicleAction("Boost!", [](Scm &code) { code.op(SET_CAR_FORWARD_SPEED).car().f(45.0f); });
    }

    void paintVehicle()
    {
        static std::mt19937 rng{std::random_device{}()};
        const int a = static_cast<int>(rng() % kCarColours), b = static_cast<int>(rng() % kCarColours);
        vehicleAction("Vehicle painted", [a, b](Scm &code) { code.op(CHANGE_CAR_COLOUR).car().i(a).i(b); });
    }

    void teleport(int landmark)
    {
        if (landmark < 0 || landmark >= static_cast<int>(std::size(kLandmarks)))
            return post("No such place");
        const LandmarkDef &l = kLandmarks[landmark];
        const std::string name = nameOf(g_landmarkNames, landmark, l.fallback);
        queue([&l, name](uint8_t *, Scm &code, std::string &notice) {
            moveTo(code, 0, l.x, l.y, l.z != 0.0f ? l.z : -100.0f);
            notice = "Teleported to " + name;
            return true;
        });
    }

    void savePosition()
    {
        queue([](uint8_t *ram, Scm &, std::string &notice) {
            const uint32_t ped = playerPed(ram);
            const uint32_t at = driving(ram, ped) ? pedVehicle(ram, ped) : ped;
            g_saved = {true, rdf(ram, at + kPosition), rdf(ram, at + kPosition + 4), rdf(ram, at + kPosition + 8),
                       rd32(ram, A.currArea)};
            g_hasSaved = true;
            notice = fmt("Position saved (%.0f, %.0f, %.0f)", g_saved.x, g_saved.y, g_saved.z);
            return true;
        });
    }

    void notePosition()
    {
        queue([](uint8_t *ram, Scm &, std::string &notice) {
            float p[3];
            if (!game::bridgepoints::playerPosition(ram, p))
                return (notice = "No player yet", false);
            notice = game::bridgepoints::note(p) ? "Position noted" : "Could not write bridge_points.txt";
            return true;
        });
    }

    void returnToSaved()
    {
        queue([](uint8_t *, Scm &code, std::string &notice) {
            if (!g_saved.valid)
                return (notice = "No position saved", false);
            moveTo(code, g_saved.area, g_saved.x, g_saved.y, g_saved.z);
            notice = "Back at the saved position";
            return true;
        });
    }

    bool hasSavedPosition() { return g_hasSaved.load(std::memory_order_relaxed); }

    void teleportToCamera()
    {
        queue([](uint8_t *ram, Scm &code, std::string &notice) {
            const vcs::freecam::Pose p = lcs::freecam::pose();
            if (!lcs::freecam::flying() || !p.active)
                return (notice = "Turn the free camera on first", false);
            // on the ground below it (SET_CHAR_COORDINATES with z -100 finds the ground), in the area shown; no
            // LOAD_SCENE (see moveTo): the collision there is the camera's, streamed in while it flew
            code.op(SET_AREA_VISIBLE).i(static_cast<int32_t>(rd32(ram, A.currArea)));
            code.op(SET_CHAR_COORDINATES).me().f(p.x).f(p.y).f(-100.0f);
            g_toggles[FreeCamera] = false;
            notice = fmt("Teleported below the camera (%.0f, %.0f)", p.x, p.y);
            return true;
        });
    }

    void setTime(int hour)
    {
        hour = ((hour % 24) + 24) % 24;
        queue([hour](uint8_t *, Scm &code, std::string &notice) {
            code.op(SET_TIME_OF_DAY).i(hour).i(0);
            g_frozenHour = hour;
            g_frozenMinute = 0;
            notice = fmt("Time %02d:00", hour);
            return true;
        });
    }

    void setWeather(int weather)
    {
        if (weather >= static_cast<int>(std::size(kWeatherNames)))
            return post("No such weather");
        queue([weather](uint8_t *, Scm &code, std::string &notice) {
            if (weather < 0)
            {
                code.op(RELEASE_WEATHER);
                notice = "Weather changes normally";
            }
            else
            {
                code.op(FORCE_WEATHER_NOW).i(weather);
                notice = std::string("Weather: ") + kWeatherNames[weather];
            }
            return true;
        });
    }

    // LOAD_AND_LAUNCH_MISSION_INTERNAL: what the game's own mission markers run; the mission starts where the player
    // is (as VCS's trainer). A side job is started in its vehicle (one spawned if he is not in one).
    void launchMission(int index)
    {
        const std::string name = nameOf(g_storyNames, index, nameOf(g_jobNames, index, fmt("Mission %d", index)));
        int vehicle = -1;
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            if (index >= 0 && index < static_cast<int>(g_missionScripts.size()))
                for (const JobDef &j : kJobs)
                    if (g_missionScripts[index] == j.script && g_vehicleByKey.count(j.vehicle))
                        vehicle = g_vehicleByKey.at(j.vehicle);
        }
        if (vehicle >= 0 && !missionRunning())
            queue([vehicle](uint8_t *ram, Scm &code, std::string &) {
                const uint32_t ped = playerPed(ram);
                if (driving(ram, ped) && rd16(ram, pedVehicle(ram, ped) + kEntityModel) == vehicle)
                    return true; // in one already
                spawnCar(ram, code, vehicle, true); // the pool full: no car, and the launch below says so
                return true;
            }, {}, {vehicle});
        // DONH4 "No Money, Mo' Problems" is a payment, not a fight: after its 8-Ball cutscene it checks the player's
        // money (MAIN.SCM DONH4 +0x2D9 IS_SCORE_GREATER 0x010F player 19999); with $20,000 it takes them (+0x87B
        // ADD_SCORE -20000) and passes ("8-ball will let you know when the hardware is ready"), without them it ends
        // at once with "8-ball won't budge unless you pay him $20000" (+0x6C3). The story gives the player no money
        // for it either: said in the notice, nothing set (checked in runs: $100 -> ends, $25,100 -> $5,100 and passes).
        bool payment = false;
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            payment = index >= 0 && index < static_cast<int>(g_missionScripts.size()) && g_missionScripts[index] == "DONH4";
        }
        queue([index, name, vehicle, payment](uint8_t *ram, Scm &code, std::string &notice) {
            if (ram[game::addrs().script.missionRunning & kRamMask])
                return (notice = "A mission is running: finish or fail it first", false);
            if (vehicle >= 0 && !driving(ram, playerPed(ram)))
                return (notice = "Could not get into the job's vehicle", false);
            code.op(LOAD_AND_LAUNCH_MISSION_INTERNAL).i(index);
            notice = "Mission: " + name;
            constexpr uint32_t kPlayerMoney = 0x00408E40u + 0xBCu; // CPlayerInfo[0] +0xBC (above)
            if (payment && static_cast<int32_t>(rd32(ram, kPlayerMoney)) < 20000)
                notice += " (8-Ball wants $20,000: without them it ends at once)";
            if (g_toggles[FreeCamera].exchange(false)) // the mission's start is to be watched, not flown past
                notice += " (free camera off)";
            return true;
        });
    }

    void setMissionScript(const std::vector<uint8_t> &d)
    {
        // MAIN.SCM: u32 the main part's size, u32, then the main part (loaded into ScriptSpace: 0x109D8C..0x109E10).
        // The mission table (0x3025D8, from CTheScripts::Init 0x109E28; ScriptSpace = file + 8): space[3] the first
        // segment's jump, u32 there + 8 -> the mission header: u16, u16, u32 largest mission, u16 count, u16, then the
        // missions' file offsets - 8 (the launcher seeks to offset + 8: 0x26810C). Each mission names itself
        // early with SCRIPT_NAME (0x03A9) and 8 bytes.
        auto u32 = [&d](size_t at) {
            uint32_t v = 0;
            if (at + 4 <= d.size())
                std::memcpy(&v, d.data() + at, 4);
            return v;
        };
        const size_t first = static_cast<size_t>(u32(8 + 3)) + 3;
        const size_t header = static_cast<size_t>(u32(8 + first)) + 8;
        const uint32_t count = u32(8 + header + 8) & 0xFFFFu;
        if (count == 0 || count > 200)
            return;
        std::vector<std::string> names(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t start = static_cast<size_t>(u32(8 + header + 12 + 4 * i)) + 8;
            const size_t end = i + 1 < count ? static_cast<size_t>(u32(8 + header + 12 + 4 * (i + 1))) + 8 : d.size();
            // (most open with it; Karmageddon (101) names itself after a few setup commands)
            for (size_t p = start; p + 10 < std::min({d.size(), end, start + 400}); ++p)
                if (d[p] == (kScriptNameOp & 0xFF) && d[p + 1] == (kScriptNameOp >> 8))
                {
                    for (size_t q = p + 2; q < p + 10 && d[q]; ++q)
                        names[i] += static_cast<char>(d[q]);
                    break;
                }
        }
        std::lock_guard<std::mutex> lock(g_namesMutex);
        g_missionScripts = std::move(names);
        g_namesFrom = 0; // build the lists again
    }
    bool missionRunning() { return g_missionRunning.load(std::memory_order_relaxed); }

    void setGameSpeed(int percent)
    {
        percent = std::clamp(percent, 25, 200);
        g_speed = percent;
        queue([percent](uint8_t *, Scm &code, std::string &notice) {
            code.op(SET_TIME_SCALE).f(percent / 100.0f);
            notice = fmt("Game speed %d%%", percent);
            return true;
        });
    }

    int gameSpeed() { return g_speed.load(std::memory_order_relaxed); }
    int clockHour() { return g_clockHour.load(std::memory_order_relaxed); }

    std::vector<Entry> vehicles()
    {
        std::vector<Entry> out;
        {
            std::lock_guard<std::mutex> lock(g_namesMutex);
            out = g_vehicleNames;
        }
        // [crossover] Vice City Stories' vehicles from the player's own disc (tools/stories_pack.py), after LCS's own:
        // ids crossover::kIdBase + VCS's model id, names tagged (the menu puts a header above them)
        if (!out.empty())
        {
            std::vector<Entry> vcs;
            for (const crossover::Vehicle &v : crossover::vehicles())
                vcs.push_back({v.label + " (VCS)", crossover::kIdBase + v.id, v.cls, true});
            std::sort(vcs.begin(), vcs.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
            out.insert(out.end(), vcs.begin(), vcs.end());
        }
        return out;
    }

    std::vector<Entry> weapons()
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        if (!g_weaponNames.empty())
            return g_weaponNames;
        std::vector<Entry> out;
        for (const WeaponDef &w : kWeapons)
            out.push_back({w.fallback, w.type});
        return out;
    }

    std::vector<Entry> loadouts()
    {
        std::vector<Entry> out;
        for (size_t k = 0; k < std::size(kLoadouts); ++k)
            out.push_back({kLoadouts[k].name, static_cast<int>(k)});
        return out;
    }

    std::vector<Entry> landmarks()
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        if (!g_landmarkNames.empty())
            return g_landmarkNames;
        std::vector<Entry> out;
        for (size_t k = 0; k < std::size(kLandmarks); ++k)
            out.push_back({kLandmarks[k].fallback, static_cast<int>(k)});
        return out;
    }

    std::vector<Entry> weathers()
    {
        std::vector<Entry> out{{"Normal (changes)", -1}};
        for (size_t k = 0; k < std::size(kWeatherNames); ++k)
            out.push_back({kWeatherNames[k], static_cast<int>(k)});
        return out;
    }

    std::vector<Entry> missions(bool story)
    {
        std::lock_guard<std::mutex> lock(g_namesMutex);
        return story ? g_storyNames : g_jobNames;
    }

    // Rows LCS has no code for: the save page's put-back after a load. The save page itself is LCS's saves (game/lcs_save.h, through trainer_select.h).
    bool shown(const std::string &label)
    {
        static const char *const kHidden[] = {"Put me back after loading"};
        return std::none_of(std::begin(kHidden), std::end(kHidden), [&](const char *h) { return label == h; });
    }

    std::string help(const std::string &label)
    {
        if (label == "Side jobs")
            return "Taxi, Paramedic, Vigilante, Firefighter, Pizza Boy, Noodle Punk.";
        if (label == "Teleport")
            return "Every district of Liberty City, and a position of your own.";
        if (label == "Give loadout")
        {
            std::string out;
            for (const LoadoutDef &l : kLoadouts)
                out += std::string(out.empty() ? "" : " ") + l.name + ": " + l.help + ".";
            return out;
        }
        return {};
    }
}

namespace lcs::trainer
{
    // [dualworld] the border handover's arrival (game/dualworld.h). No LOAD_SCENE (a synchronous load can hang this
    // port: moveTo's note); the arrival is at a known height (the sea, a deck)
    static void arriveTry(float x, float y, float z, float heading, int model, float speed, int tries);
    void arrive(float x, float y, float z, float heading, int model, float speed)
    {
        if (model >= crossover::kIdBase) // [crossover] a VCS vehicle crossing over as itself: bound first
        {
            // tools/stories.py's car map: kIdBase + the nearest LCS model x 1000 + VCS's model id
            const int other = (model - crossover::kIdBase) % 1000, nearest = (model - crossover::kIdBase) / 1000;
            queue([x, y, z, heading, other, nearest, speed](uint8_t *ram, Scm &, std::string &notice) {
                std::string why;
                int slot = crossover::bind(ram, other, why);
                if (!slot)
                {
                    std::fprintf(stderr, "[crossover] arrival in VCS vehicle %d: %s (the nearest LCS model %d instead)\n", other,
                                 why.c_str(), nearest);
                    slot = nearest;
                }
                arrive(x, y, z, heading, slot, speed);
                notice.clear();
                return true;
            });
            return;
        }
        arriveTry(x, y, z, heading, model, speed, 0);
    }

    static void arriveTry(float x, float y, float z, float heading, int model, float speed, int tries)
    {
        bool car = model > 0;
        queue([x, y, z, heading, model, speed, car, tries](uint8_t *ram, Scm &code, std::string &notice) mutable {
            // [guard] the vehicle pool full (freeVehicleSlots): asked again next frame while the game frees a slot; after
            // ~2 s the player arrives on foot (as VCS's arrival)
            if (car && vehiclePoolFull(ram))
            {
                if (tries < 120)
                {
                    arriveTry(x, y, z, heading, model, speed, tries + 1);
                    notice.clear();
                    return true; // nothing run this frame
                }
                std::fprintf(stderr, "[trainer] arrival: the vehicle pool stayed full: on foot\n");
                car = false;
                code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(model); // the runner's REQUEST_MODEL for it
            }
            code.op(SET_AREA_VISIBLE).i(0);
            if (inVehicle())
                code.op(WARP_CHAR_FROM_CAR_TO_COORD).me().f(x).f(y).f(z);
            else
                code.op(SET_CHAR_COORDINATES).me().f(x).f(y).f(z);
            if (car)
            {
                code.op(REQUEST_MODEL).i(model);
                code.op(CREATE_CAR).i(model).f(x).f(y).f(z).local(0);
                code.op(SET_CAR_HEADING).local(0).f(heading);
                code.op(WARP_CHAR_INTO_CAR).me().local(0);
                code.op(SET_CAR_FORWARD_SPEED).local(0).f(speed);
                code.op(RESTORE_CAMERA_JUMPCUT);
                code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(model);
                code.op(MARK_CAR_AS_NO_LONGER_NEEDED).local(0);
            }
            notice = fmt("Arrived at (%.0f, %.0f, %.0f)", x, y, z);
            return true;
        }, {}, car ? std::vector<int>{model} : std::vector<int>{});
    }

    void carry(int money, int health)
    {
        queue([money, health](uint8_t *ram, Scm &code, std::string &) {
            int now;
            std::memcpy(&now, ram + 0x00408EFCu, 4); // CPlayerInfo[0] +0xBC money
            if (money >= 0 && money != now)
                code.op(ADD_SCORE).player().i(money - now);
            if (health > 0)
                code.op(SET_CHAR_HEALTH).me().i(health);
            return true;
        });
    }

    void carryKit(int armour, const int (*weapons)[2], int count, int current)
    {
        queue([armour](uint8_t *ram, Scm &code, std::string &) {
            code.op(CLEAR_WANTED_LEVEL).player();
            code.op(REMOVE_ALL_CHAR_WEAPONS).me();
            if (const uint32_t ped = playerPed(ram); ped && armour >= 0) // armour float +0x4BC (ADD_ARMOUR_TO_CHAR)
                wrf(ram, ped + 0x4BCu, static_cast<float>(std::min(armour, 200)));
            return true;
        });
        // one script per weapon (a script's code space is 176 bytes), its model loaded first (as giveLoadout); the
        // one in hand last (a weapon given after it can take the hand: seen with the rocket launcher)
        for (int k = 0; k < 2 * count; ++k)
            if (const WeaponDef *w = (weapons[k % count][0] == current) == (k >= count) ? weaponDef(weapons[k % count][0]) : nullptr)
            {
                const WeaponDef given{w->type, w->textKey, w->fallback, std::max(weapons[k % count][1], 1)};
                const int model = weaponModel(w->type);
                const bool select = w->type == current;
                queue([given, model, select](uint8_t *, Scm &code, std::string &) {
                    g_clipSize[given.type] = 0;
                    return (addWeapon(code, given, model, select), true);
                }, {}, modelList(model));
            }
    }

    void selectWeapon(int type)
    {
        const WeaponDef *w = weaponDef(type);
        if (!w)
            return;
        const int model = weaponModel(type);
        queue([type, model](uint8_t *, Scm &code, std::string &) {
            if (model > 0)
                code.op(REQUEST_MODEL).i(model);
            code.op(SET_CURRENT_CHAR_WEAPON).me().i(type);
            if (model > 0)
                code.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(model);
            return true;
        }, {}, modelList(model));
    }
}
