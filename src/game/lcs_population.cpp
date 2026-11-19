// [lcs] See lcs_population.h. LCS's population code is GTA III's (CCarCtrl::GenerateOneRandomCar, CPopulation::
// AddToPopulation), not VCS's, but it has the same three knobs VCS's density setting turns (vcs_world_detail.cpp):
//
// - A constant factor each spawner multiplies the scripts' density multiplier by, read nowhere else (GTA III's
//   CIniFile::CarNumberMultiplier / PedNumberMultiplier): cars 0.8 at 0x3D9214 (GenerateOneRandomCar 0x212A08: wanted
//   = zone car density x the player's road density x CarDensityMultiplier 0x3D56A0 x 0.8, against NumRandomCars
//   0x3D56A4), peds 0.5 at 0x3D9210 (AddToPopulation 0x2ED880: wanted = zone peds x road density x
//   PedDensityMultiplier 0x3D7CDC x 0.5, against the ped count 0x3DA184). The setting scales the constant: a mission
//   that clears the streets (multiplier 0) still does.
// - The caps: cars 30 at 0x3D56E0 (NumRandomCars + the other car counters 0x3D56A8..0x3D56BC, as VCS's 16); peds 30
//   outdoors at 0x3D7CE8 and 40 elsewhere at 0x3D7CEC (VCS: 25 / 30), chosen by 0x3D8430 (CGame::currArea). Read only
//   by the spawners (the car cap also by 0x21E3E0). Raised only while the pools keep 16 slots free (LCS deletes the
//   car / ped closest to the camera every 8th frame with fewer than 8 free: 0x2156B0 / 0x2ECF18, as VCS), and never
//   below the original. The car generator 0x212938 itself stops at 30 random cars (`slti 0x1E`, VCS: 30 too).
// - The spawn chance: where VCS looks a road's class up in a table {0, 4, 7, 15}, LCS keeps the chance in the path
//   node (+0x12 & 0xF, the lower of the two nodes), and both spawners do `if (chance < (rand() & 15)) return` (cars at
//   0x213050, peds at 0x2EDED0; the chance in $s0 at the call). The game's rand (0x34FCE0, Park-Miller, seed 0x3D9178)
//   is replaced: the same generator on the host, and for those two calls only a draw within the scaled chance
//   ((chance + 1) / 16 x the setting, chance 0 - next to no traffic - as it is, like VCS's class 0) is reported as one
//   within the original chance. The random sequence is the game's either way. (Measured in Portland: the nodes' chance
//   was 15 - always - so this mostly does nothing; it is there for the roads that have less.)
// - The pace (LCS only): GenerateRandomCars (0x212938) makes one try a frame (VCS's six), and most tries find no free
//   spot, so the number of random cars stays under what is wanted (10-13 of 15.2 in Portland at 100%; a higher target
//   alone changed nothing). Above 100% the game's own refill - the 100 tries in one frame it makes after a script
//   changed the density (0x3D56CC counted down to 0) - is asked for every 2 s x 100 / (setting - 100) while fewer cars
//   drive than wanted; its side effect (0x3D9E30 = 20: the parked-car generators refresh) is put back the same frame.
// - What really held the number down (world2, 2026-11-18): the off-screen distance. PossiblyRemoveVehicle removes a
//   random car out of view past 60 m from the player, and GenerateOneRandomCar deletes a car it made out of view past
//   60 m - at every density, so the cars made by the faster pace were removed again (5 -> 5 random cars at 300%
//   measured with it alone). Above 100% that distance is 60 m x the setting (at most 190 m x gen, the in-view one):
//   cars past 60 m out of view are left alone (removeVehicle), and a car the spawner made out of view between 60 m and
//   that distance is handed to the game's in-view rule instead (sphereVisible): it stays only 130..190 x f x gen m from
//   the player and 105 x f x gen from the camera - never near enough to be seen appearing. Measured on a Portland
//   street, on foot: random cars 5 (100%) -> 26 (200%) -> 28 (300%; the generator's own 30); no car appeared in view
//   nearer than 80 m (`popin`), none vanished. (The game's sphere test answered "out of view" for every car the
//   spawner made in these runs - the car is not in the world yet - so on the PS2 too only the 60 m rule applies there.)
// - Near the player (traffic agent, 2026-11-18): world2's 26-28 cars were spread over 180 m - on a Portland street the
//   cars driving within 50 m of the player stayed 3.7 (100%) -> 2.4-4.1 (300%), and its 26 drivers took the street's
//   peds (the ped count includes drivers: on foot within 50 m 7.7-10.5 -> 0.8-4.9). Above 100% now:
//   * the spots: what kept them empty is FindObjectsKindaColliding's "no vehicle within the radius + 20 m" (867 cars
//     made in 40 s, 7 added); that 20 m becomes 13.5 m at 200%, 7 m at 300% (kindaColliding). 1 - 100 / setting of
//     the far tries (in view, so only the 84.5+ m ring keeps them) become near ones 35-55 m away outside the camera's
//     view cone + 12 degrees (coords; a quarter of that while driving, where the far ones fill the road ahead), and a
//     near spot the camera test finds in the picture is answered "in view" (sphereDone: the ring then rejects it);
//   * the keep distance off screen is 60 m + 70 m x sqrt((setting - 100) / 200): 109 m at 200%, 130 m at 300% (not
//     world2's 180 m), so the generator's 30 are spent where they are seen;
//   * the pace: a few tries every frame (1 + (setting - 100) / 50, the game's 100-try refill cut short) within 250 us
//     of GenerateRandomCars a frame, none while the world streams in (3 s after a camera jump > 50 m, more than 4
//     models requested, host frames > 45 ms, or the game's own stop flag 0x3D5AC0);
//   * the drivers of the setting's share of the random cars are left out of the ped count AddToPopulation compares
//     (pedSpawner), within the ped pool's reserve (14 free).
//   Measured (60-75 s, every second; before = world2's code, VCS_LCS_TRAFFIC_NEAR=0 VCS_LCS_TRAFFIC_PACE=0): cars
//   driving within 50 m / 100 m of the player - Portland 920,-410: 3.6 / 4.7 (100%), 4.1 / 10.0 (before 300%),
//   7.2 / 17.4 (300%); Chinatown 1063,-750: 7.6 / 11.3, 7.7 / 9.7, 9.4 / 16.8; Staunton 252,-1262: 7.7 / 10.2,
//   6.6 / 16.8, 13.0 / 20.4; peds on foot within 50 m kept (10.2 at 300% traffic in Staunton, 3.6 before). Driving
//   (lcsworld drive 14) the vehicle pool is the limit (8-10 free): 4.6 / 7.4 -> 6.4 / 15.0. popin 0, vanished 0.
//   VCS_LCS_TRAFFIC_NEAR=0 / VCS_LCS_TRAFFIC_PACE=0 / VCS_LCS_KEEP300 / VCS_LCS_NEAR_DRIVING /
//   VCS_LCS_SPAWN_BUDGET_US for A/B tests.
//
// Cars in view stay (VCS's guard, the open item of lcs_world's draw distance): PossiblyRemoveVehicle (0x215898, from
// RemoveDistantCars 0x215548, every car every frame) removes a car beyond 190 m x GenerationDistMultiplier
// (TheCamera+0x174) - 60 m when its sphere is off screen - once it is no longer drawn. With the draw distance above
// 100% cars are drawn past that, so they went in view. Skipped for a car inside the view and within 190 m x
// GenerationDist x the draw distance in use (lcs_drawdist's scale: the setting up to 1000%, less while its governor
// backs off), as in VCS; the game removes it once it is out of view.
//
// 100% on all three: every hook passes through and the constants are the executable's - the original game.
// VCS_WORLD_DETAIL_HOOKS=0 hooks nothing (as for VCS); VCS_WORLD_KEEP=0 drops the pool reserve and keeps no car in view.
// `lcsworld` -> the state (as VCS's `worlddetail`); a car that disappears in view within the drawn distance is counted
// and logged ([lcs-pop] car vanished in view).
#include "game/lcs_population.h"

#include "game/game_table.h"
#include "game/lcs_drawdist.h"
#include "game/vcs_display.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace lcs::population
{
    namespace
    {
        struct Addrs
        {
            uint32_t carFactor, pedFactor;   // float: the spawners' own factor (CIniFile::Car/PedNumberMultiplier)
            uint32_t carCap;                 // int: random cars + the other car counters
            uint32_t carCounters;            // NumRandomCars +0, +4, +8, +0xC, (+0x10 not counted), +0x14, +0x18
            uint32_t pedCapOut, pedCapIn;    // int: peds outdoors / elsewhere
            uint32_t pedCount;               // the ped count AddToPopulation compares
            uint32_t carSpawner, pedSpawner; // GenerateOneRandomCar, AddToPopulation
            uint32_t rand, randSeed;         // rand(): Park-Miller; its seed
            uint32_t carChanceRet, pedChanceRet; // return addresses of the spawn-chance draws (chance in $s0)
            uint32_t removeVehicle;          // PossiblyRemoveVehicle(vehicle a0)
            uint32_t removeDistantCars;      // RemoveDistantCars(): once a frame
            uint32_t worldRemove;            // CWorld::Remove(entity a0)
            uint32_t pedPool, vehiclePool;   // -> CPool {entries, flags, size}
            uint32_t pedSize, vehicleSize;
            uint32_t camera;                 // TheCamera: right +0, forward +0x10, up +0x20, position +0x30
            uint32_t genDist;                // TheCamera + GenerationDistMultiplier
            uint32_t activeCam, cams, camSize, camFov; // the active CCam's FOV: camera + cams + index x size + fov
            uint32_t playerPed;              // Players[0].m_pPed
            uint32_t trafficMult;            // Players[0] m_nTrafficMultiplier (u16)
            uint32_t carDensity;             // CarDensityMultiplier (the scripts')
            uint32_t carGenerator;           // GenerateRandomCars: one GenerateOneRandomCar a frame, or a refill
            uint32_t refillCounter;          // u8: counted down by it; the frame it reaches 0: 100 tries
            uint32_t parkedRefresh;          // u8: set to 20 after a refill (the parked-car generators run at once)
            uint32_t timeMs;                 // CTimer::m_snTimeInMilliseconds
            uint32_t sphereVisible;          // CEntity sphere-in-view test (vehicle a0) -> v0 0 / 1
            uint32_t spawnVisibleRet;        // its call in GenerateOneRandomCar returns here
            uint32_t coords, coordsRet;      // NewGenerateCarCreationCoors; its call in GenerateOneRandomCar
            uint32_t colliding, spacingRet;  // FindObjectsKindaColliding; its 20 m spacing call in GenerateOneRandomCar
        };
        constexpr Addrs kLcs{
            0x003D9214u, // 0.8: one lwc1 in the whole executable, GenerateOneRandomCar 0x212AF4: lhu zone density;
                         // x player+0x150 (road density) -> m_nTrafficMultiplier; x [0x3D56A0]; x [0x3D9214]; c.ole
                         // with NumRandomCars [0x3D56A4] - VCS's 0.7 0x48A680 in FUN_00430290
            0x003D9210u, // 0.5: one lwc1, AddToPopulation 0x2EDB98 (wanted = (zone peds) x road density x [0x3D7CDC]
                         // x [0x3D9210] x ...) - VCS's 0.4 0x48A67C in FUN_002B17A8
            0x003D56E0u, // 30: lw at 0x212B58 (GenerateOneRandomCar, `cap <= NumRandomCars + 0x3D56A8 + AC + B0 + B8 +
                         // BC` -> return), 0x21E444, 0x2EDA8C; no writer
            0x003D56A4u, // NumRandomCars (its writers: the car constructors / destructors), then +4..+0x18
            0x003D7CE8u, // 30: lw at 0x2EDB90 (if [0x3D8430] == 0), the only access
            0x003D7CECu, // 40: lw at 0x2EDB8C (otherwise), the only access
            0x003DA184u, // recounted by AddToPopulation's caller 0x2EC290 right before it calls it, compared at 0x2EDBD8
            0x00212A08u, // best.py twin of VCS's FUN_00430290 (0.63), called by GenerateRandomCars 0x212938
            0x002ED880u, // twin of VCS's FUN_002B17A8 (0.67), called by 0x2EC290 / 0x2EC460
            0x0034FCE0u, // lw [0x3D9178]; div by 127773; 16807 x lo - 2836 x hi; +0x7FFFFFFF if <= 0; sw; return it
            0x003D9178u,
            0x00213058u, // jal 0x34FCE0 at 0x213050; andi 0xF; sltu $s0 (min of the two nodes' +0x12 & 0xF) -> return
            0x002EDED8u, // jal 0x34FCE0 at 0x2EDED0; andi 0xF; slt $s0 (the same, the ped path's nodes) -> return 0
            0x00215898u, // twin of VCS's FUN_00433378 (0.85); 190 x [TheCamera+0x174] or 60, the sphere test 0x305E38
            0x00215548u, // twin of VCS's FUN_00433008 (0.82): every vehicle -> 0x215898; called once by 0x2FBAF0
            0x00173718u, // called before every delete in 0x215898 / 0x2156B0 / 0x2ECF18 (CWorld::Remove, VCS 0x18FA50)
            0x003D5AE8u, // ped pool: 0x2ECF18 walks it with stride 0xCD0
            0x003D5AECu, // vehicle pool: 0x215548 / 0x2156B0 walk it with stride 0x780
            0xCD0u,
            0x780u,
            0x0043BBF0u, // TheCamera (LCS address report); forward z +0x18 tested against -0.9 in both spawners
            0x0043BBF0u + 0x174u, // read in 0x215898 (x 190) and 0x212A08 (x 140), as VCS's +0x7D0
            0x10Cu, 0x220u, 0x2B0u, 0xE8u, // LCS address report (ActiveCam, Cams[0], sizeof(CCam), CCam FOV)
            0x00408E40u,
            0x00408F8Cu, // sh at 0x212B34: zone density x [player+0x150] (0x408E40 + PlayerInFocus x 0x190 + 0x14C)
            0x003D56A0u, // lwc1 at 0x212AEC; scripts' swc1 (0x108A2C, 0x2058A4, 0x20C27C, ...)
            0x00212938u, // called once a frame by CGame::Process 0x2FBAF0 (at 0x2FBE9C, after the parked-car
                         // generators 0x3205F0 at 0x2FBD80, before RemoveDistantCars at 0x2FBEAC)
            0x003D56CCu, // lbu/sb in 0x212938: if nonzero, -1; reaching 0 -> 0x212A08 x 100, then [0x3D9E30] = 20;
                         // set by script commands (0x205640, 0x21A148, 0x21A218) - the game's refill after a change
            0x003D9E30u, // sb 20 at 0x2129B4; read by the parked-car generators 0x320128 / 0x3205F0 (counted down)
            0x003D9B70u, // LCS address report
            0x00305E38u, // FUN_00305E38: the model's bounding sphere (+0x14 of its CModelInfo's col model) through
                         // FUN_0029D520 (frustum 0x6199C0) and FUN_0029F258 -> v0 != 0 in view; PossiblyRemoveVehicle
                         // calls it three times, GenerateOneRandomCar once
            0x00214468u, // `jal 0x305E38` at 0x214460 (GenerateOneRandomCar, right after the car's matrix is set);
                         // 0 -> `60 < dist(player centre)` rejects (lui 0x4270 at 0x214660; 90 m, lui 0x42B4, for flag
                         // bit 55 of +0x250, at 0x21463C), 1 -> the ring: 130 (0x214518) .. 190 (0x2144BC) m x gen x
                         // f from the player and 105 (0x214558) x gen x f from the camera
            0x00148670u, // GTA III's CPathFind::NewGenerateCarCreationCoors(x f12, y f13, dir f14/f15, dist f16,
                         // cos limit f17, a1 within/outside): node pairs (+4/+6 x 1/8 = x/y, links +0xC / +0x10 & 0xF)
                         // that straddle the f16 circle, checked against the cone; a candidate list of the nodes within
                         // 340 m (0x43AA) rebuilt every 32 frames by 0x149F78
            0x00212FE8u, // `jal 0x148670` at 0x212FE0 in GenerateOneRandomCar; its arguments set just before: on foot
                         // (0x212EE8) frame & 1 = 0 -> 140 (0x430C) x 0.65 x [TheCamera+0x174] within 0.707, = 1 -> 40
                         // (0x4220) outside 0.707; f14/f15 = TheCamera+0x154/+0x158 (CamFrontXNorm/YNorm)
            0x00178340u, // GTA III's CWorld::FindObjectsKindaColliding (arguments in kindaColliding); twice in 0x212A08
            0x002146E4u, // the first call (0x2146DC): f12 = colmodel radius (model info +0x14 -> +0xC) + 20 (0x41A0)
        };

        constexpr float kCarFactorOrig = 0.8f, kPedFactorOrig = 0.5f;
        constexpr int kCarCapOrig = 30, kPedCapOutOrig = 30, kPedCapInOrig = 40;
        constexpr int kMaxPercent = 300;     // the settings' range (vcs_world_detail.cpp)
        constexpr int kCarCapMax = 45;       // the generator stops at 30 random cars on its own; the rest is the
                                             // other counters (mission, police, parked...)
        constexpr int kPedCapMax = 80;
        constexpr int kVehicleReserve = 16;  // free slots kept: the game deletes the closest car below 8
        constexpr int kPedReserve = 16;
        constexpr int kDriverSlots = 2;      // a random car's driver (and a passenger) come from the ped pool
        constexpr int kDriverReserve = 14;   // ped pool slots kept free when the extra drivers are left out of the count
        constexpr float kRemoveDist = 190.0f; // PossiblyRemoveVehicle's in-view distance (x GenerationDist)
        constexpr float kHiddenDist = 60.0f;  // its off-screen distance (lui 0x4270 at 0x215AAC), as the spawner's
        constexpr float kCarRadius = 6.0f;
        constexpr int kMaxDrawPercent = lcs::drawdist::kMaxPercent; // the draw distance's range on LCS (1000%)
        constexpr float kPopInDist = 80.0f;   // a car appearing in view nearer than this is counted (the game's own
                                              // in-view spawns are 84.5 m and more on foot: 130 x 0.65)

        const Addrs &A = kLcs;
        bool g_installed = false, g_valid = false, g_checked = false;
        bool g_keep = true;
        std::atomic<int> g_traffic{100}, g_peds{100}, g_draw{100};
        PS2Runtime::RecompiledFunction g_origCar = nullptr, g_origPed = nullptr, g_origRand = nullptr,
                                       g_origRemoveVehicle = nullptr, g_origRemoveDistant = nullptr,
                                       g_origWorldRemove = nullptr;

        // for the command
        std::atomic<int> g_stCarCap{0}, g_stPedCapOut{0}, g_stPedCapIn{0}, g_stCars{-1}, g_stPedCount{-1},
            g_stVehicleFree{-1}, g_stPedFree{-1}, g_stVehiclesUsed{-1}, g_stPedsUsed{-1}, g_stVehicleSize{-1},
            g_stPedSize{-1};
        std::atomic<uint64_t> g_stKept{0}, g_stVanished{0}, g_stCarChanceLet{0}, g_stPedChanceLet{0},
            g_stCarDraws{0}, g_stPedDraws{0};
        std::atomic<uint32_t> g_stMinVehicleFree{999}, g_stMinPedFree{999};
        std::atomic<int> g_stCarChanceLast{-1}, g_stPedChanceLast{-1};
        std::atomic<float> g_stCarWanted{0.0f};
        std::atomic<int> g_stRandomCars{-1};
        std::atomic<uint64_t> g_stRefills{0};
        bool g_refill = false;      // game thread: a refill asked for this frame
        uint8_t g_savedParked = 0;  // the parked-car refresh counter before it
        uint32_t g_lastRefill = 0;
        PS2Runtime::RecompiledFunction g_origCarGen = nullptr;
        std::atomic<uint64_t> g_stCarCalls{0};

        // the spawner's pace and cost (game thread)
        bool g_pace = true;          // VCS_LCS_TRAFFIC_PACE=0: world2's bursts of 100 tries
        bool g_near = true;          // VCS_LCS_TRAFFIC_NEAR=0: no near spots, no in-view guard, world2's keep distance
        int g_allowTries = 0, g_triesFrame = 0;
        uint64_t g_genStartUs = 0;
        uint64_t g_budgetUs = 250;   // our extra tries stop once GenerateRandomCars has taken this long in a frame
        uint32_t g_holdUntil = 0;    // no extra tries / ped caps until then (after a teleport: the world streams in)
        float g_frameMsAvg = 16.7f;  // the host's frame time, smoothed: slow frames get no extra tries either
        uint64_t g_lastFrameUs = 0;
        uint32_t g_nearSeed = 0x2545F491u;
        float g_nearDriving = 0.25f; // share of the near conversions kept while driving (VCS_LCS_NEAR_DRIVING)
        float g_keep300 = 130.0f;     // the off-screen keep distance at 300% with near spots (VCS_LCS_KEEP300)
        PS2Runtime::RecompiledFunction g_origCoords = nullptr, g_origColliding = nullptr;
        std::atomic<uint64_t> g_stSpaced{0};
        std::atomic<int> g_stDriversOff{0};
        std::atomic<uint64_t> g_stTries{0}, g_stSkipped{0}, g_stGenUs{0}, g_stGenFrames{0}, g_stGenMaxUs{0},
            g_stNearTries{0}, g_stGuarded{0}, g_stHeldFrames{0}, g_stPedUs{0}, g_stPedCalls{0};
        constexpr float kNearMin = 35.0f, kNearMax = 55.0f; // the near spots' distance from the player
        constexpr uint32_t kNumRequested = 0x003D9D14u; // CStreaming models requested (lcs_drawdist.cpp)
        constexpr uint32_t kGenDisabled = 0x003D5AC0u;  // nonzero: GenerateRandomCars makes no car (0x212940)

        uint32_t rd32(const uint8_t *ram, uint32_t a);
        uint64_t hostUs()
        {
            using namespace std::chrono;
            return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
        }

        // extra spawning waits while the world streams in after a teleport / a cut (camera > 50 m in a frame: 3 s,
        // and while more than 4 models are still requested), and while frames are slow (host frame > 45 ms)
        bool held(const uint8_t *ram, uint32_t now)
        {
            const bool h = (now < g_holdUntil && g_holdUntil - now < 10000u) ||
                           static_cast<int32_t>(rd32(ram, kNumRequested)) > 4 || g_frameMsAvg > 45.0f ||
                           rd32(ram, kGenDisabled) != 0;
            return h;
        }
        float camFov(const uint8_t *ram);

        // the spawner's visibility test (game thread): its caller's return address while the original runs
        PS2Runtime::RecompiledFunction g_origSphere = nullptr;
        uint32_t g_sphereRa = 0, g_sphereCar = 0;
        bool g_widen = true; // VCS_LCS_TRAFFIC_RING=0: the PS2's 60 m off screen at every density (A/B tests)
        std::atomic<uint64_t> g_stMadeSeen{0}, g_stMadeHidden{0}, g_stMadeHiddenFar{0}, g_stWidened{0},
            g_stHiddenKept{0}, g_stPopIn{0};
        int g_prevFlag[256];

        // tests: what the player sees (`lcsworld` near=...): every 10 frames, cars driving (status 2/3) and peds on
        // foot within 50 / 100 m of the player, and on screen (inside the picture, within the drawn distance)
        uint8_t *g_ram = nullptr;
        std::atomic<int> g_stC50{-1}, g_stC100{-1}, g_stCScr{-1}, g_stCScr100{-1}, g_stA50{-1}, g_stP50{-1},
            g_stP100{-1}, g_stPScr{-1};
        uint32_t g_nearFrame = 0;

        // tests: `lcsworld drive v` - the player's vehicle follows the car path nodes at v m/s (ThePaths 0x3D0314 ->
        // nodes +0 (0x14 each: x/y/z int16 x 1/8 at +4/+6/+8, first link +0xC, flags +0x10: links & 0xF, 0x20
        // switched off), links +8 (u16 & 0x3FFF) - as NewGenerateCarCreationCoors 0x148670 reads them)
        constexpr uint32_t kPaths = 0x003D0314u;
        struct Drive
        {
            bool on = false;
            float v = 0.0f;
            int from = -1, to = -1;
            float s = 0.0f;
            uint32_t lastMs = 0;
            int legs = 0;
        };
        Drive g_drive;
        std::atomic<float> g_driveReq{-1.0f};
        std::atomic<int> g_stDriveLegs{0}, g_stDriveOn{0};

        // the vanish detector: per vehicle slot, its flag byte when it was near and in view last frame
        int g_nearInView[256];
        float g_nearDist[256];
        uint32_t g_removedBy[256];
        float g_teleport[3];
        std::atomic<bool> g_teleportPending{false};

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v)
        {
            if (rd32(ram, a) != v)
                std::memcpy(ram + (a & 0x01FFFFFFu), &v, 4);
        }
        void wrf(uint8_t *ram, uint32_t a, float f)
        {
            uint32_t v;
            std::memcpy(&v, &f, 4);
            wr32(ram, a, v);
        }

        int clampPercent(int v) { return std::clamp(v, 100, kMaxPercent); }
        int clampDraw(int v) { return std::clamp(v, 100, kMaxDrawPercent); }

        // free entries of a CPool (+0 entries, +4 flag bytes - bit 0x80 free -, +8 size); -1 before it exists
        int poolFree(const uint8_t *ram, uint32_t poolPtr, int *used = nullptr, int *sizeOut = nullptr)
        {
            if (used)
                *used = -1;
            const uint32_t pool = rd32(ram, poolPtr) & 0x01FFFFFFu;
            if (!pool)
                return -1;
            const uint32_t flags = rd32(ram, pool + 4) & 0x01FFFFFFu;
            const int size = static_cast<int>(rd32(ram, pool + 8));
            if (!flags || size <= 0 || size > 4096)
                return -1;
            if (sizeOut)
                *sizeOut = size;
            int spare = 0;
            for (int i = 0; i < size; ++i)
                spare += (ram[flags + i] & 0x80) ? 1 : 0;
            if (used)
                *used = size - spare;
            return spare;
        }

        void noteMin(std::atomic<uint32_t> &m, int v)
        {
            if (v >= 0 && static_cast<uint32_t>(v) < m.load(std::memory_order_relaxed))
                m.store(static_cast<uint32_t>(v), std::memory_order_relaxed);
        }

        int scaledCap(int original, int percent, int limit)
        {
            if (percent <= 100)
                return original;
            return std::clamp(static_cast<int>(std::lround(original * percent / 100.0)), original, std::max(original, limit));
        }

        // above the original only as far as the pools keep their reserve (vcs_world_detail.cpp's poolCap)
        int poolCap(int original, int scaled, int count, int room)
        {
            if (scaled <= original || !g_keep)
                return scaled;
            return std::max(original, std::min(scaled, count + std::max(room, 0)));
        }

        int carCount(const uint8_t *ram)
        {
            int sum = 0;
            for (uint32_t off : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x14u, 0x18u})
                sum += static_cast<int>(rd32(ram, A.carCounters + off));
            return sum;
        }

        // the executable's values, checked once (another build or not loaded yet: leave everything alone)
        bool valid(const uint8_t *ram)
        {
            if (!g_checked)
            {
                g_checked = true;
                const float cf = rdf(ram, A.carFactor), pf = rdf(ram, A.pedFactor);
                const int cc = static_cast<int>(rd32(ram, A.carCap)), po = static_cast<int>(rd32(ram, A.pedCapOut)),
                          pi = static_cast<int>(rd32(ram, A.pedCapIn));
                g_valid = cf == kCarFactorOrig && pf == kPedFactorOrig && cc == kCarCapOrig && po == kPedCapOutOrig &&
                          pi == kPedCapInOrig;
                if (!g_valid)
                    std::fprintf(stderr,
                                 "[lcs-pop] unexpected spawner values (car %g cap %d, ped %g caps %d/%d): density left "
                                 "alone\n",
                                 cf, cc, pf, po, pi);
            }
            return g_valid;
        }

        void writeCar(uint8_t *ram)
        {
            const int traffic = g_traffic.load(std::memory_order_relaxed);
            wrf(ram, A.carFactor, traffic == 100 ? kCarFactorOrig : kCarFactorOrig * (traffic / 100.0f));
            const int vehicleFree = poolFree(ram, A.vehiclePool), pedFree = poolFree(ram, A.pedPool);
            const int room = std::min(vehicleFree - kVehicleReserve, pedFree - kPedReserve - kDriverSlots);
            const int cap = poolCap(kCarCapOrig, scaledCap(kCarCapOrig, traffic, kCarCapMax), carCount(ram), room);
            wr32(ram, A.carCap, static_cast<uint32_t>(cap));
            g_stCarCap.store(cap, std::memory_order_relaxed);
            noteMin(g_stMinVehicleFree, vehicleFree);
            noteMin(g_stMinPedFree, pedFree);
        }

        void writePed(uint8_t *ram, bool hold)
        {
            // while the world streams in (held): the original caps and factor - no extra peds made then
            const int peds = hold ? 100 : g_peds.load(std::memory_order_relaxed);
            wrf(ram, A.pedFactor, peds == 100 ? kPedFactorOrig : kPedFactorOrig * (peds / 100.0f));
            const int pedFree = poolFree(ram, A.pedPool);
            const int count = static_cast<int>(rd32(ram, A.pedCount));
            const int room = pedFree - kPedReserve;
            const int out = poolCap(kPedCapOutOrig, scaledCap(kPedCapOutOrig, peds, kPedCapMax), count, room);
            const int in = poolCap(kPedCapInOrig, scaledCap(kPedCapInOrig, peds, kPedCapMax), count, room);
            wr32(ram, A.pedCapOut, static_cast<uint32_t>(out));
            wr32(ram, A.pedCapIn, static_cast<uint32_t>(in));
            g_stPedCapOut.store(out, std::memory_order_relaxed);
            g_stPedCapIn.store(in, std::memory_order_relaxed);
            g_stPedCount.store(count, std::memory_order_relaxed);
            noteMin(g_stMinPedFree, pedFree);
        }

        PS2_GUEST_CC void carSpawner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // our refill (carGenerator) has its tries a frame and its time budget: the rest of the game's 100-try loop
            // returns at once (GenerateOneRandomCar returns nothing)
            if (g_refill)
            {
                if (g_triesFrame >= g_allowTries || (g_pace && hostUs() - g_genStartUs > g_budgetUs))
                {
                    g_stSkipped.fetch_add(1, std::memory_order_relaxed);
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
            }
            ++g_triesFrame;
            g_stTries.fetch_add(1, std::memory_order_relaxed);
            if (valid(rdram))
            {
                writeCar(rdram);
                // what GenerateOneRandomCar compares NumRandomCars with (last frame's m_nTrafficMultiplier: player +0x14C)
                const int16_t mult = static_cast<int16_t>(rd32(rdram, A.trafficMult) & 0xFFFFu);
                g_stCarWanted.store(static_cast<float>(static_cast<uint16_t>(mult)) * rdf(rdram, A.carDensity) *
                                        rdf(rdram, A.carFactor),
                                    std::memory_order_relaxed);
                g_stCarCalls.fetch_add(1, std::memory_order_relaxed);
            }
            g_origCar(rdram, ctx, runtime);
        }

        // GenerateRandomCars (once a frame): one try a frame is the original's pace, and fewer cars than wanted come of
        // it (10-13 of 15 wanted in Portland at 100%): the target alone does not add traffic. Above 100% the game's
        // own refill (the 100 tries it makes after a script changes the density) is asked for now and then - every
        // 1 s at 200%, 0.5 s at 300% - while fewer random cars drive than wanted; each try ends at once when enough do.
        // Measured (VCS_LCS_POP_TRACE): most made cars fail the game's ring test (visible: 130-190 m away; hidden:
        // beyond 60 m) and are deleted again - in Portland 1 in 80 tries ends as a car; the refills' tries still add
        // some (7-9 -> 10-13 random cars at 300%)
        // Its side effect, the parked-car generators' refresh, is put back before they run again (removeDistantCars)
        PS2_GUEST_CC void carGenerator(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int traffic = g_traffic.load(std::memory_order_relaxed);
            const uint32_t now = rd32(rdram, A.timeMs);
            g_triesFrame = 0;
            g_genStartUs = hostUs();
            if (g_valid && traffic > 100 && !g_refill && rdram[A.refillCounter] == 0 &&
                (!g_pace || !held(rdram, now)))
            {
                const int random = static_cast<int>(rd32(rdram, A.carCounters));
                if (random < 30 && random < g_stCarWanted.load(std::memory_order_relaxed))
                {
                    if (g_pace)
                    {
                        // a few tries every frame (the game's 100-try refill, cut short by carSpawner): 200% 3, 300% 5
                        g_allowTries = 1 + (traffic - 100) / 50;
                        g_refill = true;
                        g_savedParked = rdram[A.parkedRefresh];
                        rdram[A.refillCounter] = 1;
                        g_stRefills.fetch_add(1, std::memory_order_relaxed);
                    }
                    else // VCS_LCS_TRAFFIC_PACE=0: world2's pace - all 100 tries every 1 s x 100 / (setting - 100)
                    {
                        const uint32_t interval = static_cast<uint32_t>(1000 * 100 / (traffic - 100));
                        if (now - g_lastRefill >= interval || now < g_lastRefill)
                        {
                            g_lastRefill = now;
                            g_allowTries = 1000;
                            g_refill = true;
                            g_savedParked = rdram[A.parkedRefresh];
                            rdram[A.refillCounter] = 1;
                            g_stRefills.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
            g_origCarGen(rdram, ctx, runtime);
            // the cost (tests; a frame that returned part-way is counted short)
            const uint64_t us = hostUs() - g_genStartUs;
            g_stGenUs.fetch_add(us, std::memory_order_relaxed);
            g_stGenFrames.fetch_add(1, std::memory_order_relaxed);
            if (us > g_stGenMaxUs.load(std::memory_order_relaxed))
                g_stGenMaxUs.store(us, std::memory_order_relaxed);
        }

        // NewGenerateCarCreationCoors (0x148670) from GenerateOneRandomCar (ra 0x212FE8): a0 ThePaths, f12/f13 the
        // player's centre, f14/f15 the direction, f16 the distance on whose circle the spot lies, f17 the cosine
        // limit, a1 1 = within it (ahead) / 0 = outside it. On foot the game asks for 91 m ahead (140 x 0.65 x gen,
        // within 45 degrees: in view, so only the in-view ring keeps it) or 40 m outside 45 degrees, every other frame;
        // in a car mostly ahead (narrower) and 1 in 4 at 40 m to the side. Above 100% some far tries become near ones
        // out of the picture: 35-55 m from the player, outside the camera's view cone + 12 degrees - behind him, to the
        // sides, round the corners next to him. (A spot that still ends up in view is caught in sphereDone.)
        PS2_GUEST_CC void coords(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int traffic = g_traffic.load(std::memory_order_relaxed);
            if (g_valid && g_near && traffic > 100 && GPR_U32(ctx, 31) == A.coordsRet && ctx->f[16] > kHiddenDist)
            {
                // q = 1 - 100 / setting of the far tries: 200% 1/2, 300% 2/3
                uint32_t &seed = g_nearSeed;
                seed = seed * 1664525u + 1013904223u;
                const float r1 = static_cast<float>(seed >> 8) / 16777216.0f;
                seed = seed * 1664525u + 1013904223u;
                const float r2 = static_cast<float>(seed >> 8) / 16777216.0f;
                // driving (player's vehicle faster than 5 m/s: m_vecMoveSpeed +0x70 per 1/50 s) the far tries ahead
                // are what fills the road in front: only g_nearDriving of them become near spots
                float q = 1.0f - 100.0f / static_cast<float>(traffic);
                if (const uint32_t me = rd32(rdram, A.playerPed) & 0x01FFFFFFu; me && rdram[me + 0x538u])
                    if (const uint32_t car = rd32(rdram, me + 0x534u) & 0x01FFFFFFu;
                        car && std::hypot(rdf(rdram, car + 0x70u), rdf(rdram, car + 0x74u)) * 50.0f > 5.0f)
                        q *= g_nearDriving;
                if (r1 < q)
                {
                    const float fx = rdf(rdram, A.camera + 0x154u), fy = rdf(rdram, A.camera + 0x158u); // CamFront*Norm
                    const float n = std::hypot(fx, fy);
                    if (n > 0.1f)
                    {
                        const float t = std::tan(std::clamp(camFov(rdram), 10.0f, 170.0f) * 3.14159265f / 360.0f);
                        const float half = std::atan(t * std::max(1.0f, vcs::aspectWidening()));
                        ctx->f[14] = fx / n;
                        ctx->f[15] = fy / n;
                        ctx->f[16] = kNearMin + (kNearMax - kNearMin) * r2;
                        ctx->f[17] = std::cos(std::min(half + 0.21f, 3.0f));
                        SET_GPR_U32(ctx, 5, 0);
                        g_stNearTries.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            g_origCoords(rdram, ctx, runtime);
        }

        // What really keeps the spots empty (VCS_LCS_POP_TRACE in Portland: 867 cars made in 40 s, 7 added): after the
        // distance tests GenerateOneRandomCar (testForCollision, set on foot and at low speed) asks
        // FindObjectsKindaColliding (0x178340: a0 position, a1 check height, a2 -> count, a3 max 2, t0 list 0, t1
        // buildings 0, t2 vehicles 1, t3 peds 0; f12 radius) whether any vehicle is within the model's radius + 20 m
        // (lui 0x41A0 at 0x2146A4; call at 0x2146DC) - a parked car or another random car 20 m away is enough - and
        // then (0x214734) whether a vehicle or ped touches it. Above 100% (near spots on) the 20 m becomes
        // 20 - 6.5 x (setting - 100) / 100: 13.5 m at 200%, 7 m at 300%; the touch test stays.
        PS2_GUEST_CC void kindaColliding(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int traffic = g_traffic.load(std::memory_order_relaxed);
            if (g_valid && g_near && traffic > 100 && GPR_U32(ctx, 31) == A.spacingRet)
            {
                const float gap = 20.0f - 6.5f * static_cast<float>(traffic - 100) / 100.0f;
                ctx->f[12] = std::max(ctx->f[12] - 20.0f + gap, 1.0f);
                g_stSpaced.fetch_add(1, std::memory_order_relaxed);
            }
            g_origColliding(rdram, ctx, runtime);
        }

        // VCS_LCS_POP_TRACE=1: how far GenerateOneRandomCar's tries get - calls of its callees, by callee (tests)
        struct Stage
        {
            uint32_t fn;
            const char *name;
        };
        constexpr Stage kStages[] = {
            {0x00214DE0u, "model"},    // pick a model (-1: none)
            {0x00148670u, "nodes"},    // find a node pair in the spawn ring
            {0x0021EF88u, "spot"},     // the spot's test before the chance draw
            {0x00327978u, "new"},      // operator new: a car object made
            {0x00305E38u, "visible"},  // its sphere test (then the ring distances)
            {0x00178340u, "clear"},    // collision-free tests
            {0x001735D0u, "added"},    // CWorld::Add: a car in the world
        };
        constexpr int kStageCount = sizeof(kStages) / sizeof(kStages[0]);
        PS2Runtime::RecompiledFunction g_origStage[kStageCount] = {};
        std::atomic<uint64_t> g_stStage[kStageCount];
        template <int I>
        PS2_GUEST_CC void stageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra > 0x00212A08u && ra < 0x00214D00u)
                g_stStage[I].fetch_add(1, std::memory_order_relaxed);
            g_origStage[I](rdram, ctx, runtime);
        }
        template <int I>
        void installStages(PS2Runtime &runtime)
        {
            if constexpr (I < kStageCount)
            {
                if ((g_origStage[I] = runtime.lookupFunction(kStages[I].fn)))
                    runtime.replaceFunction(kStages[I].fn, &stageHook<I>);
                installStages<I + 1>(runtime);
            }
        }

        PS2_GUEST_CC void pedSpawner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (valid(rdram))
            {
                const bool hold = g_pace && held(rdram, rd32(rdram, A.timeMs));
                writePed(rdram, hold);
                writeCar(rdram); // it reads the car cap too (cars short: peds come closer)
                // The ped count AddToPopulation compares with its wanted number and the cap (0x3DA184, summed by
                // 0x2EF9E8 in its caller 0x2EC290 just before this call, read only at 0x2EDBD8) counts the random cars'
                // drivers too: world2's 300% traffic (26 random cars) took the street's peds (on foot within 50 m:
                // 10.5 at 100% -> 1.2). Above 100% traffic the drivers of the cars beyond what 100% would want are
                // left out of it for this call (it is summed again before the next)
                const int traffic = g_traffic.load(std::memory_order_relaxed);
                if (g_near && traffic > 100 && !hold)
                {
                    // the setting's share of the random cars (300%: 2/3 of them), within the ped pool's reserve
                    const int random = static_cast<int>(rd32(rdram, A.carCounters));
                    const int extra = std::min(static_cast<int>(random * (1.0f - 100.0f / traffic)),
                                               poolFree(rdram, A.pedPool) - kDriverReserve);
                    const int count = static_cast<int>(rd32(rdram, A.pedCount));
                    if (extra > 0 && count > 0)
                    {
                        wr32(rdram, A.pedCount, static_cast<uint32_t>(std::max(count - extra, 0)));
                        g_stDriversOff.store(extra, std::memory_order_relaxed);
                    }
                }
            }
            const uint64_t t0 = hostUs();
            g_origPed(rdram, ctx, runtime);
            g_stPedUs.fetch_add(hostUs() - t0, std::memory_order_relaxed); // tests (a call cut short counts short)
            g_stPedCalls.fetch_add(1, std::memory_order_relaxed);
        }

        // the chance (0..15) a spawner accepts at `percent`: (chance + 1) / 16 times it, 0 as it is (VCS's scaledChance)
        int scaledChance(int chance, int percent)
        {
            if (percent <= 100 || chance <= 0)
                return chance;
            return std::clamp(static_cast<int>(std::lround((chance + 1) * percent / 100.0)) - 1, chance, 15);
        }

        // rand() (0x34FCE0) on the host, as the game's: Schrage's Park-Miller step. For the two spawn-chance draws a
        // value within the scaled chance is reported as one within the original (its low nibble lowered to it)
        PS2_GUEST_CC void randHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const bool car = ra == A.carChanceRet, ped = ra == A.pedChanceRet;
            if (!car && !ped)
            {
                g_origRand(rdram, ctx, runtime);
                return;
            }
            const int32_t seed = static_cast<int32_t>(rd32(rdram, A.randSeed));
            const int32_t hi = seed / 127773, lo = seed % 127773;
            int32_t next = static_cast<int32_t>(static_cast<uint32_t>(lo * 16807) - static_cast<uint32_t>(hi * 2836));
            if (next <= 0)
                next = static_cast<int32_t>(static_cast<uint32_t>(next) + 0x7FFFFFFFu);
            wr32(rdram, A.randSeed, static_cast<uint32_t>(next));
            uint32_t v = static_cast<uint32_t>(next);
            const int chance = static_cast<int>(GPR_U32(ctx, 16)); // $s0
            const int percent = (car ? g_traffic : g_peds).load(std::memory_order_relaxed);
            (car ? g_stCarDraws : g_stPedDraws).fetch_add(1, std::memory_order_relaxed);
            (car ? g_stCarChanceLast : g_stPedChanceLast).store(chance, std::memory_order_relaxed);
            if (g_valid && chance >= 0 && chance < 15)
            {
                const int n = static_cast<int>(v & 0xFu), scaled = scaledChance(chance, percent);
                if (n > chance && n <= scaled)
                {
                    v = (v & ~0xFu) | static_cast<uint32_t>(chance);
                    (car ? g_stCarChanceLet : g_stPedChanceLet).fetch_add(1, std::memory_order_relaxed);
                }
            }
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(v));
            ctx->pc = ra;
        }

        float camFov(const uint8_t *ram)
        {
            const uint32_t active = ram[(A.camera + A.activeCam) & 0x01FFFFFFu];
            const float fov = rdf(ram, A.camera + A.cams + std::min(active, 2u) * A.camSize + A.camFov);
            return (fov > 1.0f && fov < 179.0f) ? fov : 70.0f;
        }

        // inside the camera's view (a little wider than the picture): TheCamera's matrix and the active CCam's FOV,
        // widened by the Hor+ factor or made taller by Vert+'s (vcs_world_detail.cpp's inView)
        bool inView(const uint8_t *ram, const float *p, float radius)
        {
            const float d[3] = {p[0] - rdf(ram, A.camera + 0x30), p[1] - rdf(ram, A.camera + 0x34),
                                p[2] - rdf(ram, A.camera + 0x38)};
            auto dot = [&](uint32_t row) {
                return d[0] * rdf(ram, A.camera + row) + d[1] * rdf(ram, A.camera + row + 4) + d[2] * rdf(ram, A.camera + row + 8);
            };
            const float y = dot(0x10);
            if (y < -radius)
                return false;
            const float t = std::tan(std::clamp(camFov(ram), 10.0f, 170.0f) * 3.14159265f / 360.0f) * 1.15f;
            const float r = vcs::aspectWidening();
            const float tanH = t * std::max(1.0f, r), tanV = t * 0.75f / std::clamp(r, 0.1f, 1.0f);
            return std::fabs(dot(0x00)) <= std::max(y, 0.0f) * tanH + radius &&
                   std::fabs(dot(0x20)) <= std::max(y, 0.0f) * tanV + radius;
        }

        // how far cars are drawn: 190 m x GenerationDist x the draw distance in use this frame (lcs_drawdist's scale:
        // the setting, up to 1000%, less while its memory / render-list governor backs off)
        float drawnDistance(const uint8_t *ram)
        {
            const float set = g_draw.load(std::memory_order_relaxed) / 100.0f;
            const float used = lcs::drawdist::lodScale();
            return kRemoveDist * rdf(ram, A.genDist) * std::max(1.0f, used > 1.0f ? std::min(used, set) : set);
        }

        // where the game measures a car from: FindPlayerCentreOfWorld (0x107390) - the player's vehicle, else the ped
        bool playerCentre(const uint8_t *ram, float *out)
        {
            const uint32_t ped = rd32(ram, A.playerPed) & 0x01FFFFFFu;
            if (!ped)
                return false;
            uint32_t at = ped;
            const uint32_t veh = rd32(ram, ped + 0x534u) & 0x01FFFFFFu; // PED_VEHICLE (+0x538: in it)
            if (veh && ram[ped + 0x538u])
                at = veh;
            out[0] = rdf(ram, at + 0x30u);
            out[1] = rdf(ram, at + 0x34u);
            return true;
        }

        // Off screen a random car stays within 60 m (PossiblyRemoveVehicle, and the spawner keeps a car it makes out of
        // view only that near) - the same distance at every density, so above 100% most cars made were removed again.
        // Above 100%: 60 m x the setting, never past the in-view distance (190 m x gen): 120 m at 200%, 180 at 300%
        float hiddenKeep(const uint8_t *ram)
        {
            const int traffic = g_traffic.load(std::memory_order_relaxed);
            if (!g_widen || !g_keep || traffic <= 100)
                return kHiddenDist;
            const float ring = kRemoveDist * rdf(ram, A.genDist);
            if (!g_near) // world2's: 60 m x the setting - the cars spread over up to 180 m, most of them far away
                return std::max(kHiddenDist, std::min(kHiddenDist * traffic / 100.0f, ring));
            // with near spots the cars are kept where they are seen: 60 m + (keep300 - 60) x (setting - 100) / 200
            // (default 109 m at 200%, 130 m at 300%); beyond it the generator's 30 are better spent near the player
            return std::max(kHiddenDist, std::min(kHiddenDist + (g_keep300 - kHiddenDist) *
                                                                   std::sqrt((traffic - 100) / 200.0f), ring));
        }

        bool active()
        {
            return g_valid && (g_traffic.load(std::memory_order_relaxed) > 100 || g_peds.load(std::memory_order_relaxed) > 100 ||
                               g_draw.load(std::memory_order_relaxed) > 100);
        }

        PS2_GUEST_CC void removeVehicle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t car = GPR_U32(ctx, 4) & 0x01FFFFFFu;
            if (car && g_keep && active())
            {
                const float p[3] = {rdf(rdram, car + 0x30), rdf(rdram, car + 0x34), rdf(rdram, car + 0x38)};
                const float dx = p[0] - rdf(rdram, A.camera + 0x30), dy = p[1] - rdf(rdram, A.camera + 0x34);
                const float drawn = drawnDistance(rdram);
                if (dx * dx + dy * dy <= drawn * drawn && inView(rdram, p, kCarRadius))
                {
                    g_stKept.fetch_add(1, std::memory_order_relaxed);
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
                // off screen, past the game's 60 m but within the density's distance: a car driving (status +0x40 bits
                // 36-40 = 2 simple / 3 physics; wrecked 5 and the rest go to the game) is left alone this frame. What
                // the game would still do to it here - its stuck-car clean-up (> 22 m off screen) - waits until it is
                // within 60 m again or past this distance
                const float keep = hiddenKeep(rdram);
                float c[2];
                if (keep > kHiddenDist && playerCentre(rdram, c))
                {
                    uint64_t flags;
                    std::memcpy(&flags, rdram + car + 0x40u, 8);
                    const unsigned status = static_cast<unsigned>((flags >> 36) & 0x1Fu);
                    const float d = std::hypot(p[0] - c[0], p[1] - c[1]);
                    if ((status == 2 || status == 3) && d > kHiddenDist && d <= keep)
                    {
                        g_stHiddenKept.fetch_add(1, std::memory_order_relaxed);
                        ctx->pc = GPR_U32(ctx, 31);
                        return;
                    }
                }
            }
            g_origRemoveVehicle(rdram, ctx, runtime);
        }

        // The sphere test (0x305E38) for GenerateOneRandomCar's call only: the original runs with ra = this function's
        // address (its answer is read when it returns there), then a car the game found out of view
        // past 60 m - which it would delete at once - is reported "in view" while it is within hiddenKeep(): the game's
        // own in-view rule then decides (130..190 m x gen x f from the player, 105 x gen x f from the camera: a car
        // made that far away is never seen appearing). A car the game sees in view is left as it is.
        // Note: the runtime's call dispatch (dispatchGuestBranchSlow) takes a replaced function that returns with pc still
        // at its own entry for one that returned without setting pc, and goes on at the caller's return address: the
        // original's `jr $ra` to this function's address is caught right here, after the call (ctx->pc == the entry:
        // it ran to its end). Only when it stopped part-way (an EE checkpoint: pc elsewhere) does the scheduler come
        // back to this function later with ra = its address
        void sphereDone(uint8_t *rdram, R5900Context *ctx)
        {
            {
                const uint32_t back = g_sphereRa;
                g_sphereRa = 0;
                if (GPR_U32(ctx, 2))
                    g_stMadeSeen.fetch_add(1, std::memory_order_relaxed);
                else
                {
                    g_stMadeHidden.fetch_add(1, std::memory_order_relaxed);
                    const uint32_t car = g_sphereCar;
                    float c[2];
                    if (car && playerCentre(rdram, c))
                    {
                        const float p[3] = {rdf(rdram, car + 0x30u), rdf(rdram, car + 0x34u), rdf(rdram, car + 0x38u)};
                        const float d = std::hypot(p[0] - c[0], p[1] - c[1]);
                        // above 100% (near spots): the game's test said "out of view" for every new car, so a spot
                        // within 60 m it keeps could be in the picture - our camera test decides there: in view ->
                        // "in view", and the in-view ring (84.5 m and more) rejects it. Never seen appearing.
                        const int traffic = g_traffic.load(std::memory_order_relaxed);
                        if (g_near && g_widen && traffic > 100 && d <= kHiddenDist && inView(rdram, p, 4.0f))
                        {
                            SET_GPR_U32(ctx, 2, 1);
                            g_stGuarded.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (d > kHiddenDist)
                        {
                            g_stMadeHiddenFar.fetch_add(1, std::memory_order_relaxed);
                            if (d <= hiddenKeep(rdram))
                            {
                                SET_GPR_U32(ctx, 2, 1);
                                g_stWidened.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                SET_GPR_U32(ctx, 31, back);
                ctx->pc = back;
            }
        }

        PS2_GUEST_CC void sphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == A.sphereVisible && g_sphereRa) // the original ended after an EE checkpoint
                return sphereDone(rdram, ctx);
            if (ra == A.spawnVisibleRet && g_valid)
            {
                g_sphereRa = ra;
                g_sphereCar = GPR_U32(ctx, 4) & 0x01FFFFFFu;
                SET_GPR_U32(ctx, 31, A.sphereVisible);
                ctx->pc = A.sphereVisible;
                g_origSphere(rdram, ctx, runtime);
                if (ctx->pc == A.sphereVisible && g_sphereRa) // it ran to its end (`jr $ra`)
                    sphereDone(rdram, ctx);
                return;
            }
            g_origSphere(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void worldRemove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (const uint32_t pool = rd32(rdram, A.vehiclePool) & 0x01FFFFFFu)
            {
                const uint32_t entries = rd32(rdram, pool) & 0x01FFFFFFu, e = GPR_U32(ctx, 4) & 0x01FFFFFFu;
                const uint32_t size = rd32(rdram, pool + 8);
                if (size <= 256 && e >= entries && e < entries + size * A.vehicleSize && (e - entries) % A.vehicleSize == 0)
                    g_removedBy[(e - entries) / A.vehicleSize] = GPR_U32(ctx, 31);
            }
            g_origWorldRemove(rdram, ctx, runtime);
        }

        // tests: inside the picture (no margin), at most maxDist from the camera
        bool onScreen(const uint8_t *ram, const float *p, float maxDist)
        {
            const float d[3] = {p[0] - rdf(ram, A.camera + 0x30), p[1] - rdf(ram, A.camera + 0x34),
                                p[2] - rdf(ram, A.camera + 0x38)};
            auto dot = [&](uint32_t row) {
                return d[0] * rdf(ram, A.camera + row) + d[1] * rdf(ram, A.camera + row + 4) + d[2] * rdf(ram, A.camera + row + 8);
            };
            const float y = dot(0x10);
            if (y < 1.0f || y > maxDist)
                return false;
            const float t = std::tan(std::clamp(camFov(ram), 10.0f, 170.0f) * 3.14159265f / 360.0f);
            const float r = vcs::aspectWidening();
            return std::fabs(dot(0x00)) <= y * t * std::max(1.0f, r) &&
                   std::fabs(dot(0x20)) <= y * t * 0.75f / std::clamp(r, 0.1f, 1.0f);
        }

        void nearStats(const uint8_t *ram)
        {
            float c[2];
            if (!playerCentre(ram, c))
                return;
            const uint32_t me = rd32(ram, A.playerPed) & 0x01FFFFFFu;
            const uint32_t myCar = me ? (rd32(ram, me + 0x534u) & 0x01FFFFFFu) : 0;
            const float drawn = drawnDistance(ram);
            int c50 = 0, c100 = 0, cs = 0, cs100 = 0, a50 = 0, p50 = 0, p100 = 0, ps = 0;
            for (int kind = 0; kind < 2; ++kind)
            {
                const uint32_t pool = rd32(ram, kind ? A.pedPool : A.vehiclePool) & 0x01FFFFFFu;
                if (!pool)
                    continue;
                const uint32_t entries = rd32(ram, pool) & 0x01FFFFFFu, flags = rd32(ram, pool + 4) & 0x01FFFFFFu;
                const int n = std::min(static_cast<int>(rd32(ram, pool + 8)), 256);
                const uint32_t size = kind ? A.pedSize : A.vehicleSize;
                for (int i = 0; i < n && entries && flags; ++i)
                {
                    if (ram[flags + i] & 0x80)
                        continue;
                    const uint32_t e = entries + static_cast<uint32_t>(i) * size;
                    if (e == me || e == myCar)
                        continue;
                    const float p[3] = {rdf(ram, e + 0x30), rdf(ram, e + 0x34), rdf(ram, e + 0x38)};
                    const float d = std::hypot(p[0] - c[0], p[1] - c[1]);
                    if (kind == 0)
                    {
                        uint64_t fl;
                        std::memcpy(&fl, ram + e + 0x40u, 8);
                        const unsigned status = static_cast<unsigned>((fl >> 36) & 0x1Fu);
                        a50 += d <= 50.0f;
                        if (status != 2 && status != 3)
                            continue;
                        c50 += d <= 50.0f;
                        c100 += d <= 100.0f;
                        const bool s = onScreen(ram, p, drawn);
                        cs += s;
                        cs100 += s && d <= 100.0f;
                    }
                    else
                    {
                        if (ram[e + 0x538u]) // in a vehicle
                            continue;
                        p50 += d <= 50.0f;
                        p100 += d <= 100.0f;
                        ps += onScreen(ram, p, 120.0f);
                    }
                }
            }
            g_stC50.store(c50);
            g_stC100.store(c100);
            g_stCScr.store(cs);
            g_stCScr100.store(cs100);
            g_stA50.store(a50);
            g_stP50.store(p50);
            g_stP100.store(p100);
            g_stPScr.store(ps);
        }

        // tests: a car path node's position
        bool nodePos(const uint8_t *ram, int i, float *out)
        {
            const uint32_t paths = rd32(ram, kPaths) & 0x01FFFFFFu;
            if (!paths || i < 0)
                return false;
            const uint32_t nodes = rd32(ram, paths) & 0x01FFFFFFu;
            const int count = static_cast<int>(rd32(ram, paths + 0x18u));
            if (!nodes || i >= count)
                return false;
            const uint32_t e = nodes + static_cast<uint32_t>(i) * 0x14u;
            int16_t v[3];
            std::memcpy(v, ram + e + 4, 6);
            out[0] = v[0] / 8.0f;
            out[1] = v[1] / 8.0f;
            out[2] = v[2] / 8.0f;
            return true;
        }

        // the node linked to `at` that goes on straightest from `from` (none: -1)
        int nextNode(const uint8_t *ram, int from, int at, float dx, float dy)
        {
            const uint32_t paths = rd32(ram, kPaths) & 0x01FFFFFFu;
            const uint32_t nodes = rd32(ram, paths) & 0x01FFFFFFu, links = rd32(ram, paths + 8u) & 0x01FFFFFFu;
            const uint32_t e = nodes + static_cast<uint32_t>(at) * 0x14u;
            int16_t first;
            std::memcpy(&first, ram + e + 0xC, 2);
            const int n = ram[e + 0x10] & 0xF;
            float a[3];
            nodePos(ram, at, a);
            int best = -1;
            float bestDot = -0.3f; // never turn back
            for (int j = 0; j < n; ++j)
            {
                uint16_t l;
                std::memcpy(&l, ram + links + 2u * static_cast<uint32_t>(first + j), 2);
                const int to = l & 0x3FFF;
                float b[3];
                if (to == from || !nodePos(ram, to, b))
                    continue;
                uint16_t fl;
                std::memcpy(&fl, ram + nodes + static_cast<uint32_t>(to) * 0x14u + 0x10, 2);
                if (fl & 0x20)
                    continue;
                const float ex = b[0] - a[0], ey = b[1] - a[1], len = std::hypot(ex, ey);
                if (len < 0.5f)
                    continue;
                const float dt = (ex * dx + ey * dy) / len;
                if (dt > bestDot)
                {
                    bestDot = dt;
                    best = to;
                }
            }
            return best;
        }

        // tests: `lcsworld drive v` - the player's vehicle along the nodes (speed set each frame, pulled to the line)
        void driveStep(uint8_t *ram)
        {
            const float req = g_driveReq.exchange(-1.0f);
            const uint32_t me = rd32(ram, A.playerPed) & 0x01FFFFFFu;
            const uint32_t car = me && ram[me + 0x538u] ? (rd32(ram, me + 0x534u) & 0x01FFFFFFu) : 0;
            const uint32_t now = rd32(ram, A.timeMs);
            if (req >= 0.0f)
            {
                g_drive = Drive{};
                if (req > 0.0f && car)
                {
                    // the nearest node, then the link most along the car's heading
                    const uint32_t paths = rd32(ram, kPaths) & 0x01FFFFFFu;
                    const int count = paths ? static_cast<int>(rd32(ram, paths + 0x18u)) : 0;
                    const float px = rdf(ram, car + 0x30), py = rdf(ram, car + 0x34);
                    float best = 1e9f;
                    for (int i = 0; i < count && i < 20000; ++i)
                    {
                        float p[3];
                        nodePos(ram, i, p);
                        const float d = std::hypot(p[0] - px, p[1] - py);
                        if (d < best)
                        {
                            best = d;
                            g_drive.from = i;
                        }
                    }
                    const float fx = rdf(ram, car + 0x10), fy = rdf(ram, car + 0x14);
                    g_drive.to = g_drive.from >= 0 ? nextNode(ram, -1, g_drive.from, fx, fy) : -1;
                    g_drive.on = g_drive.to >= 0;
                    g_drive.v = req;
                    g_drive.lastMs = now;
                }
            }
            g_stDriveOn.store(g_drive.on ? 1 : 0);
            if (!g_drive.on || !car)
                return;
            const float dt = std::min((now - g_drive.lastMs) / 1000.0f, 0.1f);
            g_drive.lastMs = now;
            float a[3], b[3];
            nodePos(ram, g_drive.from, a);
            nodePos(ram, g_drive.to, b);
            float len = std::max(std::hypot(b[0] - a[0], b[1] - a[1]), 0.5f);
            g_drive.s += g_drive.v * dt;
            while (g_drive.s >= len)
            {
                const int next = nextNode(ram, g_drive.from, g_drive.to, (b[0] - a[0]) / len, (b[1] - a[1]) / len);
                if (next < 0)
                {
                    g_drive.on = false;
                    return;
                }
                g_drive.s -= len;
                g_drive.from = g_drive.to;
                g_drive.to = next;
                ++g_drive.legs;
                g_stDriveLegs.store(g_drive.legs);
                nodePos(ram, g_drive.from, a);
                nodePos(ram, g_drive.to, b);
                len = std::max(std::hypot(b[0] - a[0], b[1] - a[1]), 0.5f);
            }
            const float dx = (b[0] - a[0]) / len, dy = (b[1] - a[1]) / len;
            // keep to the right of the centre line (the lanes; GTA's roads are right-hand)
            const float tx = a[0] + dx * g_drive.s + dy * 3.0f, ty = a[1] + dy * g_drive.s - dx * 3.0f;
            const float px = rdf(ram, car + 0x30), py = rdf(ram, car + 0x34);
            const float vx = dx * g_drive.v + (tx - px) * 2.0f, vy = dy * g_drive.v + (ty - py) * 2.0f;
            wrf(ram, car + 0x70, vx / 50.0f); // m_vecMoveSpeed (per 1/50 s step; read as such at 0x212D3C)
            wrf(ram, car + 0x74, vy / 50.0f);
            for (uint32_t o : {0x7Cu, 0x80u, 0x84u}) // m_vecTurnSpeed
                wrf(ram, car + o, 0.0f);
            // heading along the road: forward +0x10, right +0x00 (fy, -fx), up +0x20
            const float h = std::hypot(vx, vy);
            if (h > 0.5f)
            {
                const float fx = vx / h, fy = vy / h;
                wrf(ram, car + 0x00, fy);
                wrf(ram, car + 0x04, -fx);
                wrf(ram, car + 0x08, 0.0f);
                wrf(ram, car + 0x10, fx);
                wrf(ram, car + 0x14, fy);
                wrf(ram, car + 0x18, 0.0f);
                wrf(ram, car + 0x20, 0.0f);
                wrf(ram, car + 0x24, 0.0f);
                wrf(ram, car + 0x28, 1.0f);
            }
        }

        // once a frame (CGame::Process, after the car generator): the settings, the state, the vanish detector
        PS2_GUEST_CC void removeDistantCars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_ram = rdram;
            {
                const uint64_t us = hostUs();
                if (g_lastFrameUs)
                    g_frameMsAvg = g_frameMsAvg * 0.9f + std::min((us - g_lastFrameUs) / 1000.0f, 500.0f) * 0.1f;
                g_lastFrameUs = us;
            }
            if (g_valid && held(rdram, rd32(rdram, A.timeMs)))
                g_stHeldFrames.fetch_add(1, std::memory_order_relaxed);
            driveStep(rdram);
            if (g_valid && ++g_nearFrame % 10 == 0)
                nearStats(rdram);
            g_traffic.store(clampPercent(settings::value("enhance.traffic")), std::memory_order_relaxed);
            g_peds.store(clampPercent(settings::value("enhance.peds")), std::memory_order_relaxed);
            g_draw.store(clampDraw(settings::value("enhance.draw_distance")), std::memory_order_relaxed);
            if (g_refill) // our refill ran (GenerateRandomCars is before this in CGame::Process): its parked-car refresh
            {
                rdram[A.parkedRefresh] = g_savedParked;
                g_refill = false;
            }
            if (g_teleportPending.exchange(false))
                if (const uint32_t ped = rd32(rdram, A.playerPed) & 0x01FFFFFFu)
                    for (int i = 0; i < 3; ++i)
                        wrf(rdram, ped + 0x30 + 4 * i, g_teleport[i]);
            if (valid(rdram))
            {
                int used = -1, size = -1;
                g_stVehicleFree.store(poolFree(rdram, A.vehiclePool, &used, &size), std::memory_order_relaxed);
                g_stVehiclesUsed.store(used, std::memory_order_relaxed);
                g_stVehicleSize.store(size, std::memory_order_relaxed);
                g_stPedFree.store(poolFree(rdram, A.pedPool, &used, &size), std::memory_order_relaxed);
                g_stPedsUsed.store(used, std::memory_order_relaxed);
                g_stPedSize.store(size, std::memory_order_relaxed);
                g_stCars.store(carCount(rdram), std::memory_order_relaxed);
                g_stRandomCars.store(static_cast<int>(rd32(rdram, A.carCounters)), std::memory_order_relaxed);

                if (const uint32_t pool = rd32(rdram, A.vehiclePool) & 0x01FFFFFFu)
                {
                    static float lastCam[2] = {0, 0};
                    const float cam[2] = {rdf(rdram, A.camera + 0x30), rdf(rdram, A.camera + 0x34)};
                    const bool cut = std::hypot(cam[0] - lastCam[0], cam[1] - lastCam[1]) > 30.0f;
                    if (std::hypot(cam[0] - lastCam[0], cam[1] - lastCam[1]) > 50.0f) // a teleport / cut: hold 3 s
                        g_holdUntil = rd32(rdram, A.timeMs) + 3000u;
                    lastCam[0] = cam[0];
                    lastCam[1] = cam[1];
                    const float drawn = drawnDistance(rdram);
                    const uint32_t entries = rd32(rdram, pool) & 0x01FFFFFFu, flags = rd32(rdram, pool + 4) & 0x01FFFFFFu;
                    const int n = std::min(static_cast<int>(rd32(rdram, pool + 8)), 256);
                    for (int i = 0; i < n && entries && flags; ++i)
                    {
                        const int flag = rdram[flags + i];
                        if (g_nearInView[i] >= 0 && g_nearInView[i] != flag && !cut)
                        {
                            g_stVanished.fetch_add(1, std::memory_order_relaxed);
                            std::fprintf(stderr,
                                         "[lcs-pop] car vanished in view (slot %d, %.0f m, removed from 0x%x; traffic %d%%, "
                                         "draw %d%%)\n",
                                         i, g_nearDist[i], g_removedBy[i], g_traffic.load(), g_draw.load());
                        }
                        g_nearInView[i] = -1;
                        const bool fresh = flag != g_prevFlag[i]; // a new car in this slot (the flag byte's id moves on)
                        g_prevFlag[i] = flag;
                        if (flag & 0x80)
                            continue;
                        const uint32_t car = entries + static_cast<uint32_t>(i) * A.vehicleSize;
                        const float p[3] = {rdf(rdram, car + 0x30), rdf(rdram, car + 0x34), rdf(rdram, car + 0x38)};
                        const float dist = std::hypot(p[0] - cam[0], p[1] - cam[1]);
                        if (fresh && !cut && dist < kPopInDist && inView(rdram, p, 1.0f))
                        {
                            g_stPopIn.fetch_add(1, std::memory_order_relaxed);
                            std::fprintf(stderr, "[lcs-pop] car appeared in view (slot %d, %.0f m; traffic %d%%)\n", i, dist,
                                         g_traffic.load());
                        }
                        if (dist < drawn && inView(rdram, p, 1.0f))
                        {
                            g_nearInView[i] = flag;
                            g_nearDist[i] = dist;
                        }
                    }
                }
            }
            g_origRemoveDistant(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("VCS_WORLD_DETAIL_HOOKS"); env && env[0] == '0')
        {
            std::fprintf(stderr, "[lcs-pop] density left alone (VCS_WORLD_DETAIL_HOOKS=0)\n");
            return;
        }
        if (const char *keep = std::getenv("VCS_WORLD_KEEP"); keep && keep[0] == '0')
        {
            g_keep = false;
            std::fprintf(stderr, "[lcs-pop] no pool reserve, no cars kept in view (VCS_WORLD_KEEP=0)\n");
        }
        if (const char *ring = std::getenv("VCS_LCS_TRAFFIC_RING"); ring && ring[0] == '0')
        {
            g_widen = false;
            std::fprintf(stderr, "[lcs-pop] off-screen cars kept within 60 m at every density (VCS_LCS_TRAFFIC_RING=0)\n");
        }
        if (const char *v = std::getenv("VCS_LCS_TRAFFIC_NEAR"); v && v[0] == '0')
        {
            g_near = false;
            std::fprintf(stderr, "[lcs-pop] no near spawn spots, world2's keep distance (VCS_LCS_TRAFFIC_NEAR=0)\n");
        }
        if (const char *v = std::getenv("VCS_LCS_TRAFFIC_PACE"); v && v[0] == '0')
            g_pace = false;
        if (const char *v = std::getenv("VCS_LCS_NEAR_DRIVING"))
            g_nearDriving = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        if (const char *v = std::getenv("VCS_LCS_KEEP300"))
            g_keep300 = std::clamp(static_cast<float>(std::atof(v)), 60.0f, 190.0f);
        if (const char *v = std::getenv("VCS_LCS_SPAWN_BUDGET_US"))
            g_budgetUs = static_cast<uint64_t>(std::clamp(std::atoi(v), 50, 5000));
        std::fill(std::begin(g_nearInView), std::end(g_nearInView), -1);
        std::fill(std::begin(g_prevFlag), std::end(g_prevFlag), -1);
        g_origSphere = runtime.lookupFunction(A.sphereVisible);
        g_origCar = runtime.lookupFunction(A.carSpawner);
        g_origPed = runtime.lookupFunction(A.pedSpawner);
        g_origRand = runtime.lookupFunction(A.rand);
        g_origRemoveVehicle = runtime.lookupFunction(A.removeVehicle);
        g_origRemoveDistant = runtime.lookupFunction(A.removeDistantCars);
        g_origWorldRemove = runtime.lookupFunction(A.worldRemove);
        g_origCarGen = runtime.lookupFunction(A.carGenerator);
        g_origCoords = runtime.lookupFunction(A.coords);
        g_origColliding = runtime.lookupFunction(A.colliding);
        if (!g_origCar || !g_origPed || !g_origRand || !g_origRemoveVehicle || !g_origRemoveDistant || !g_origWorldRemove ||
            !g_origCarGen || !g_origSphere || !g_origCoords || !g_origColliding)
        {
            game::logUnavailable("traffic / pedestrian density (lcs_population: spawners not found)");
            return;
        }
        runtime.replaceFunction(A.carSpawner, &carSpawner);
        runtime.replaceFunction(A.carGenerator, &carGenerator);
        runtime.replaceFunction(A.pedSpawner, &pedSpawner);
        runtime.replaceFunction(A.rand, &randHook);
        runtime.replaceFunction(A.removeVehicle, &removeVehicle);
        runtime.replaceFunction(A.removeDistantCars, &removeDistantCars);
        runtime.replaceFunction(A.worldRemove, &worldRemove);
        runtime.replaceFunction(A.sphereVisible, &sphereVisible); // before the trace's stage hook (it wraps this)
        runtime.replaceFunction(A.coords, &coords);
        runtime.replaceFunction(A.colliding, &kindaColliding);
        g_installed = true;
        std::fprintf(stderr, "[lcs-pop] traffic / pedestrian density, cars in view kept\n");
        if (const char *trace = std::getenv("VCS_LCS_POP_TRACE"); trace && trace[0] == '1')
        {
            installStages<0>(runtime);
            // lcsworld stages -> ok model=.. nodes=.. ... (calls from GenerateOneRandomCar since the start)
            ps2control::addCommand("lcspopstages", [](const std::string &, ps2control::Reply reply) {
                std::string out = "ok";
                for (int i = 0; i < kStageCount; ++i)
                    out += std::string(" ") + kStages[i].name + "=" + std::to_string(g_stStage[i].load());
                reply(out);
            });
        }

        // lcsworld -> ok installed valid traffic peds draw carcap cars pedcap=out/in pedcount vehfree/used/size
        //   pedfree/used/size minvehfree minpedfree (since the last query) kept vanished chance=carlet/draws,pedlet/draws
        // `lcsworld teleport x y z`: (tests) the player on foot to that point at the next frame
        ps2control::addCommand("lcsworld", [](const std::string &args, ps2control::Reply reply) {
            float x, y, z;
            if (std::sscanf(args.c_str(), " teleport %f %f %f", &x, &y, &z) == 3)
            {
                g_teleport[0] = x;
                g_teleport[1] = y;
                g_teleport[2] = z;
                g_teleportPending.store(true);
                return reply("ok");
            }
            if (args.find("cars") != std::string::npos && g_ram) // tests: every vehicle: x,y,z,status,on screen
            {
                std::string out = "ok";
                char buf[96];
                const uint32_t pool = rd32(g_ram, A.vehiclePool) & 0x01FFFFFFu;
                const uint32_t entries = pool ? rd32(g_ram, pool) & 0x01FFFFFFu : 0;
                const uint32_t flags = pool ? rd32(g_ram, pool + 4) & 0x01FFFFFFu : 0;
                const int n = pool ? std::min(static_cast<int>(rd32(g_ram, pool + 8)), 256) : 0;
                for (int i = 0; i < n && entries && flags; ++i)
                {
                    if (g_ram[flags + i] & 0x80)
                        continue;
                    const uint32_t e = entries + static_cast<uint32_t>(i) * A.vehicleSize;
                    const float p[3] = {rdf(g_ram, e + 0x30), rdf(g_ram, e + 0x34), rdf(g_ram, e + 0x38)};
                    uint64_t fl;
                    std::memcpy(&fl, g_ram + e + 0x40u, 8);
                    std::snprintf(buf, sizeof(buf), " %.0f,%.0f,%.0f,%u,%d", p[0], p[1], p[2],
                                  static_cast<unsigned>((fl >> 36) & 0x1Fu), onScreen(g_ram, p, 1000.0f) ? 1 : 0);
                    out += buf;
                }
                return reply(out);
            }
            float v;
            if (std::sscanf(args.c_str(), " drive %f", &v) == 1) // tests: the player's car along the nodes (0: stop)
            {
                g_driveReq.store(std::max(v, 0.0f));
                return reply("ok");
            }
            if (std::sscanf(args.c_str(), " nodes %f %f %f", &x, &y, &z) == 3 && g_ram) // tests: nodes within z m
            {
                const uint32_t paths = rd32(g_ram, kPaths) & 0x01FFFFFFu;
                std::string out = "ok";
                char buf[160];
                if (paths)
                {
                    std::snprintf(buf, sizeof(buf), " hdr=%x,%x,%x,%x,%x,%x,%x,%x", rd32(g_ram, paths + 0x0),
                                  rd32(g_ram, paths + 0x4), rd32(g_ram, paths + 0x8), rd32(g_ram, paths + 0xC),
                                  rd32(g_ram, paths + 0x10), rd32(g_ram, paths + 0x14), rd32(g_ram, paths + 0x18),
                                  rd32(g_ram, paths + 0x1C));
                    out += buf;
                    const int count = static_cast<int>(rd32(g_ram, paths + 0x18u));
                    const uint32_t nodes = rd32(g_ram, paths) & 0x01FFFFFFu;
                    for (int i = 0; i < count && i < 20000 && out.size() < 3500; ++i)
                    {
                        float p[3];
                        nodePos(g_ram, i, p);
                        if (std::hypot(p[0] - x, p[1] - y) > z)
                            continue;
                        uint16_t fl;
                        std::memcpy(&fl, g_ram + nodes + i * 0x14u + 0x10, 2);
                        std::snprintf(buf, sizeof(buf), " %d:%.0f,%.0f,%.0f,%x", i, p[0], p[1], p[2], fl);
                        out += buf;
                    }
                }
                return reply(out);
            }
            char text[1400];
            std::snprintf(text, sizeof(text),
                          "ok installed=%d valid=%d traffic=%d peds=%d draw=%d carcap=%d cars=%d pedcap=%d/%d pedcount=%d "
                          "veh=%d/%d/%d ped=%d/%d/%d minvehfree=%u minpedfree=%u kept=%llu vanished=%llu "
                          "chance=%llu/%llu,%llu/%llu last=%d/%d random=%d wanted=%.1f calls=%llu refills=%llu "
                          "madeseen=%llu madehidden=%llu hiddenfar=%llu widened=%llu hiddenkept=%llu popin=%llu ring=%d",
                          g_installed ? 1 : 0, g_valid ? 1 : 0, g_traffic.load(), g_peds.load(), g_draw.load(),
                          g_stCarCap.load(), g_stCars.load(), g_stPedCapOut.load(), g_stPedCapIn.load(),
                          g_stPedCount.load(), g_stVehicleFree.load(), g_stVehiclesUsed.load(), g_stVehicleSize.load(),
                          g_stPedFree.load(), g_stPedsUsed.load(), g_stPedSize.load(), g_stMinVehicleFree.exchange(999),
                          g_stMinPedFree.exchange(999), (unsigned long long)g_stKept.load(),
                          (unsigned long long)g_stVanished.load(), (unsigned long long)g_stCarChanceLet.load(),
                          (unsigned long long)g_stCarDraws.load(), (unsigned long long)g_stPedChanceLet.load(),
                          (unsigned long long)g_stPedDraws.load(), g_stCarChanceLast.load(), g_stPedChanceLast.load(),
                          g_installed && g_valid ? static_cast<int>(g_stRandomCars.load()) : -1, g_stCarWanted.load(),
                          (unsigned long long)g_stCarCalls.load(), (unsigned long long)g_stRefills.load(),
                          (unsigned long long)g_stMadeSeen.load(), (unsigned long long)g_stMadeHidden.load(),
                          (unsigned long long)g_stMadeHiddenFar.load(), (unsigned long long)g_stWidened.load(),
                          (unsigned long long)g_stHiddenKept.load(), (unsigned long long)g_stPopIn.load(), g_widen ? 1 : 0);
            // near=cars driving within 50/100 m, on screen (drawn distance / within 100 m), all vehicles within 50 m;
            // peds on foot within 50/100 m, on screen (120 m); cost: tries/skipped, GenerateRandomCars us total /
            // frames / max, ped spawner us / calls; neartries guarded held(frames) keep(m) drive=on/legs
            const size_t len = std::strlen(text);
            std::snprintf(text + len, sizeof(text) - len,
                          " near=%d/%d/%d/%d/%d peds=%d/%d/%d tries=%llu skipped=%llu genus=%llu genframes=%llu "
                          "genmax=%llu pedus=%llu pedcalls=%llu neartries=%llu guarded=%llu held=%llu keep=%.0f "
                          "frame=%.1f drive=%d/%d nearmode=%d pace=%d spaced=%llu driversoff=%d",
                          g_stC50.load(), g_stC100.load(), g_stCScr.load(), g_stCScr100.load(), g_stA50.load(),
                          g_stP50.load(), g_stP100.load(), g_stPScr.load(), (unsigned long long)g_stTries.load(),
                          (unsigned long long)g_stSkipped.load(), (unsigned long long)g_stGenUs.load(),
                          (unsigned long long)g_stGenFrames.load(), (unsigned long long)g_stGenMaxUs.exchange(0),
                          (unsigned long long)g_stPedUs.load(), (unsigned long long)g_stPedCalls.load(),
                          (unsigned long long)g_stNearTries.load(), (unsigned long long)g_stGuarded.load(),
                          (unsigned long long)g_stHeldFrames.load(), g_ram ? hiddenKeep(g_ram) : 0.0f, g_frameMsAvg,
                          g_stDriveOn.load(), g_stDriveLegs.load(), g_near ? 1 : 0, g_pace ? 1 : 0,
                          (unsigned long long)g_stSpaced.load(), g_stDriversOff.load());
            reply(text);
        });
    }
}
