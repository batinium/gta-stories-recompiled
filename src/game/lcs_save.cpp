// [lcs] See lcs_save.h. LCS's save code is GTA III's PS2 one (CMemoryCard + the frontend's memory card screens), not
// VCS's save menu: the addresses below were found in the LCS ELF and are each commented with their evidence. Saving
// and loading call only the game's own functions, in the order the game's frontend calls them.
#include "game/lcs_save.h"

#include "game/game_addrs.h"
#include "game/save_shots.h"
#include "game/vcs_trainer.h"
#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace lcs::save
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            uint32_t padRead, cardScan, saveSlot, loadCheck, loadCleanup, genericLoad;
            uint32_t cardManager, savingFlag, slotNames, menu, loadRequest;
            uint32_t newestSlot, dateCompare, compareRa1, compareRa2;
        };
        constexpr Addrs kLcs{
            // scePadRead (the runtime's libpad stub, analyzer.toml "scePadRead@0x00360760"): called by the pad update
            // between frames, and in the frontend
            0x00360760u,
            // CMemoryCard "scan the card" (a0 = card manager, a1 = port 0): the twin of VCS's FUN_00276bf0 (best.py
            // ratio 0.949); the frontend calls it before every memory card screen (0x339260, 0x3396E8 ...), and
            // CMemoryCard's slot table (names at 0x630650) is what it fills
            0x002D0FE8u,
            // CMemoryCard::SaveSlot(card manager, slot 0..7): the frontend's "save to slot" calls it (0x339700) with
            // the slot the player picked (menu +0x5A0). It makes the folder if missing (0x2D0EA8 / 0x2D0B18 /
            // 0x2D0C28: icon.sys), names the slot file (0x2F7168: sprintf("%d", slot + 1) + the last mission's
            // name into 0x6305F0), opens it 0x203 (create | write | truncate) and writes the save (0x2F6A28, the
            // block writer: "SIMP" like VCS's 0x3AD850), then CTimer::Update
            0x002D1D18u,
            // the frontend's Load Game action (0x33978C: a0 = the slot 0..7): the slot's name from 0x630650 + slot *
            // 0x3C, CheckDataNotCorrupt 0x2F75F0 (reads the file through, checksum), 1 when it can be loaded
            0x002F7050u,
            // called by the frontend right after a good check (0x33979C) and by SwitchMenuOnAndOff: frees a leftover
            // buffer (0x3D82C0) if there is one
            0x002F6058u,
            // the generic load (world re-init's load of the save: the twin of VCS's 0x3ADAE0, ratio 0.739, "SIMP"
            // check), called from 0x109BC8: observed only, to report where a load put the player
            0x002F6CC0u,
            // the card manager, port 0 (0x4C0 bytes a port as VCS's 0x510480): every card function above is passed
            // it; status word +0x54 == 0x1B when fine (0x2D0EF0, 0x339708)
            0x0060D780u,
            // the saving flag: the frontend sets it to 1 around SaveSlot (0x3396FC / 0x339714), as VCS's 0x48A664
            0x003D505Cu,
            // the card's slot names, UTF-16, 0x3C a slot (0x2D1E18, 0x2F706C)
            0x00630650u,
            // FrontEndMenuManager (CMenuManager): +0x13D menu active, +0x47B the save menu wanted (set by
            // ACTIVATE_SAVE_MENU 0x22A858, taken by SwitchMenuOnAndOff 0x342700), +0x47D "want to load" (0x3397C8)
            0x00634610u,
            // the load request the main loop acts on (0x1F5C90: the frame ends with "restart"; 0x1F60D4: menu +0x47D
            // and +0x144 set, then the re-init 0x2FB700 loads): set by the frontend's load (0x3397D4) and the boot's
            // load 0x2F7958 (0x2F79EC)
            0x003D82DCu,
            // the newest slot (a0 card manager, a1 port, a2 name out): the twin of VCS's FUN_002762a8 (best.py ratio
            // 1.0), called by the boot's load 0x2F7958 and the boot's card check 0x2D1B08. For each slot (in s5) with a
            // valid header date it calls the date comparison 0x2CEF00 (VCS's FUN_00274c98, ratio 1.0): 1 = newer takes
            // the slot (call at 0x2D08D8), else a second call (0x2D093C) takes it on 0
            0x002D0808u,
            0x002CEF00u,
            0x002D08E0u,
            0x002D0944u,
        };
        // CMemoryCard::DeleteSlot(card manager, slot 0..7): finds the slot's name (0x630650 + slot * 0x3C, as
        // SaveSlot), the file by it (0x2CFE60 -> sceMcDelete 0x3677E8); called by the frontend's Delete (0x339518)
        constexpr uint32_t kDeleteSlot = 0x002D1DB8u;
        constexpr uint32_t kMenuActive = 0x13Du, kSaveMenuWanted = 0x47Bu, kWantToLoad = 0x47Du;
        constexpr uint32_t kSlotNameSize = 0x3Cu;
        constexpr uint32_t kCardStatus = 0x54u, kCardStatusOk = 0x1Bu;
        // ped fields
        constexpr uint32_t kPedPosition = 0x30u;  // CPlaceable matrix position, as VCS (lcs_camera: target = this + 0.6 z)
        constexpr uint32_t kPedMoveSpeed = 0x70u; // ACTIVATE_SAVE_MENU zeroes these two vectors (0x22A878 / 0x22A8A8)
        constexpr uint32_t kPedTurnSpeed = 0x80u;
        constexpr uint32_t kPedState = 0x34Cu;    // game/lcs_input.cpp: 0x32 driving; GTA III's enum (54 die, 55 dead)
        constexpr uint32_t kPedStateIdle = 1u;    // PED_IDLE: on foot, standing or walking (checked in a run)
        // the player's wanted level: CWanted inside the ped at +0x7F0 (CLEAR_WANTED_LEVEL 0x0115 -> 0x34D770: a0 = ped +
        // 0x7F0 -> CWanted::SetWantedLevel 0x2F4C48), its level at +0x20 (UpdateWantedLevel 0x2F49E8 stores it)
        constexpr uint32_t kPedWantedLevel = 0x7F0u + 0x20u;
        constexpr uint32_t kCameraLooksAtPlayer = 0x9Cu; // TheCamera +0x9C: 1 while no script camera (lcs_camera.cpp)

        constexpr int kStableFrames = 30; // frames on foot, not moving up or down, before a save is allowed

        // The script globals a save point writes before the game's save menu (MAIN.SCM's subroutine at 0x177A3,
        // GOSUBed by the three safehouse scripts after they set the spawn into globals 0x219..0x21B): global 6 = 1
        // ("saved at a save point", never reset), globals 7..9 = the spawn. A load restores only the 541 globals the
        // SCM lists (0x300C10; 6..9 are listed) and restarts every script; the main script then takes the load path
        // only when global 6 == 1 (0x6983: else the intro; 0x6EC7: else the new-game init that relaunches the opening
        // mission) and copies 7..9 back into 0x219..0x21B (0x7149). A save made without that subroutine (ours: the
        // quick save, "Save game here") has global 6 == 0 and loads as a new game with the money kept. Byte offsets
        // into the script space (a global is space + index * 4).
        constexpr uint32_t kGlobalSavedAtSavePoint = 6u * 4u;
        constexpr uint32_t kGlobalSpawn = 7u * 4u; // x, y, z (floats)

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        // The rest of what the subroutine at 0x177A3 stores before ACTIVATE_SAVE_MENU, opcode by opcode (MAIN.SCM bytes;
        // handlers from the opcode switch tables). Globals by index; every one below but 0x230 / 0x753 / 0x754 is in
        // the load's list, so missions read them after a load:
        //   0004 g230 = 1                          "busy" flag, back to 0 at the end (0x17A2E); not in the load list
        //   0004 g6 = 1, 0086 g7..9 = g219..21B    above (kGlobalSavedAtSavePoint / kGlobalSpawn)
        //   041E x33 (player g218, weapon, -> g7C..g9C)  ammo of a weapon type: handler 0x268200 walks the player
        //        ped's 10 weapon slots (+0x594, 0x1C each: type +0, ammo total +0xC), the last slot of that type wins, 0
        //        when none; weapon types in the order of kAmmoWeapons
        //   0474 (g218 -> g9E)    the type in the current slot (ped +0x6B8, signed byte) (0x26A00C)
        //   04E2 (g21C -> gA0)    armour as an int: ped pool (0x3D5AE8) GetAt 0x112068 of the handle in g21C, float
        //        +0x4BC, CVT.W.S (0x1867F0)
        //   0110 (g218 -> gA1)    money, CPlayerInfo +0xBC (0x110D60)
        //   4x  IF g44B+2k == 1: 064D (g44C+2k -> g122+k) else 0005 g122+k = -1.0 (a float)
        //        064D (0x273498) -> 0x1C7628: pickup handle (0x1CC128: index = low 16 bits into the 0x60-byte array at
        //        0x410300, the high 16 bits must match its u16 at +0x28, else 0.0) -> float +0x10, CVT.W.S
        //   01C5 (g218 -> g9F)    wanted level, ped +0x810 (0x20AFE0)
        //   016E/016F/0170 fade out (no globals)
        //   00BF (-> g754, g753)  GET_TIME_OF_DAY: hours u8 0x3D9B74, minutes u8 0x3D9B80 (0x10FCB8); g752..754 are
        //        only this subroutine's (after the menu it reads the clock again into g752/g753 and, when more than 3
        //        hours passed - SaveSlot moves the clock on - clears the wanted level, 0112 = 0x34D770(ped, 0); our
        //        saves refuse with a wanted level, so nothing to do)
        //   03DD ACTIVATE_SAVE_MENU, then wait for 03DE HAS_SAVE_GAME_FINISHED; after it: teleport to g219..21B,
        //   heading gA2 (0176, read only), fade in, player control on, g230 = 0 (none of these globals change in the
        //   file). The player's control (01B9) and g230 are what a quick save does not need: g230 is set for the save
        //   only (restored right after) so the file matches a save point's.
        constexpr uint32_t kPlayers = 0x00408E40u, kPlayerSize = 0x190u, kPlayerMoney = 0xBCu;
        constexpr uint32_t kPedWeapons = 0x594u, kWeaponSize = 0x1Cu, kWeaponAmmo = 0xCu, kWeaponSlots = 10u;
        constexpr uint32_t kPedCurrentSlot = 0x6B8u, kPedArmour = 0x4BCu, kPedSize = 0xCD0u;
        constexpr uint32_t kPedPool = 0x003D5AE8u;
        constexpr uint32_t kPickups = 0x00410300u, kPickupSize = 0x60u, kPickupCheck = 0x28u, kPickupValue = 0x10u;
        constexpr uint32_t kClockHours = 0x003D9B74u, kClockMinutes = 0x003D9B80u;
        constexpr uint8_t kAmmoWeapons[33] = {36, 1,  2,  7,  3,  4,  6,  8,  10, 5,  9,  11, 12, 15, 14, 13, 17,
                                              18, 19, 20, 21, 22, 24, 23, 25, 27, 26, 30, 32, 31, 33, 28, 29};
        constexpr uint32_t gBusy = 0x230u, gPlayer = 0x218u, gPlayerChar = 0x21Cu, gAmmo = 0x7Cu, gCurrentWeapon = 0x9Eu,
                           gWanted = 0x9Fu, gArmour = 0xA0u, gMoney = 0xA1u, gPickupFlag = 0x44Bu, gPickupValue = 0x122u,
                           gHours = 0x754u, gMinutes = 0x753u;

        // The safehouse script INTERIO (MAIN.SCM 0x2C3D7) and its globals, which a save made away from a save point
        // gets wrong on load (docs/LCS.md "Saves"):
        //   g1116 (0x45C) the player is inside a safehouse: set by INTERIO when he walks in through a door (0x2C883,
        //        after the fade and the interior's set-up), cleared when he leaves (0x2C6E7 / 0x2C742 / 0x2C78A: out of
        //        zone LITTLEI / the Staunton box / SWANKS with g1131/2/3 set; 0x2C9C8.. at the exit marker). While it is
        //        1 INTERIO skips the doors (0x2C810: entry needs g1116 == 0), the phone-call condition 0x22D45 used by
        //        MOBPORT / MOBSTAU / MOBSHOR answers false (0x22D89), and some help texts wait (HELP 0x30226).
        //   g1131/1132/1133 (0x46B..0x46D) safehouse 1/2/3 opened: 0xD83F(index, on) swaps the door models
        //        (SWAP_NEAREST_BUILDING_MODEL 0x03BB) and sets / clears them.
        //   g211 (0xD3) the safehouse saved in: 1/2/3 written just before the save subroutine (0x2C8D3 / 0x2CB18 /
        //        0x2CDC0), -1 at the start (0x6F3A) and when he leaves (0x2CF4B, from 0x2CFA6). So -1 in any save made
        //        outside a safehouse.
        //   On a load, the init script at mission index 5 (IF g6 == 1: the outfit, then 05AE CALL 0x2C24F(g211) =
        //   set up that safehouse's interior and door, then g1116 = 1 at +0x2EF) declares the player inside a safehouse
        //   whatever g211 says. With g211 == -1 (our quick save / Save game here, made outside) no safehouse is set up
        //   and nothing ever clears g1116: the doors never open again (no safehouse save any more) and the phone-call
        //   scripts never get their condition. Checked in runs (slot 8 of the shared free-roam card: g1116 1 after the
        //   load, the Portland door does nothing; poked 0: the door opens, the save icon opens the save menu).
        //   g505 / g506 (0x1F9 / 0x1FA) are only "help text shown" flags (HELP28 / HELP21, main loop 0x2FF22 /
        //   0x2FF35 -> 0x302BC / 0x30332): nothing to do.
        constexpr uint32_t gSavedSafehouse = 211u, gInSafehouse = 1116u, gSafehouseOpen = 1131u; // + 0..2
        constexpr uint32_t kSafehouseWatchFrames = 900; // frames after a load the g1116 fix is applied in

        // g230's value before our save set it (restored once the save is written)
        struct
        {
            bool pending = false;
            uint32_t value = 0;
        } g_busyFlag;

        void restoreBusyFlag(uint8_t *ram)
        {
            if (!g_busyFlag.pending)
                return;
            g_busyFlag.pending = false;
            if (const uint32_t space = rd32(ram, game::addrs().script.space))
                wr32(ram, space + gBusy * 4u, g_busyFlag.value);
        }

        // What the save point's subroutine stores (above), with the player's own position as the spawn (the engine
        // restores the player where he was saved; the main script copies the spawn into 0x219..0x21B). Left set, as
        // the game's own subroutine leaves them (g230 excepted).
        void markSavePoint(uint8_t *ram)
        {
            const game::Addrs &G = game::addrs();
            const uint32_t space = rd32(ram, G.script.space), ped = rd32(ram, G.world.playerPed);
            if (!space || !ped)
                return;
            const auto global = [&](uint32_t index) { return space + index * 4u; };
            if (!g_busyFlag.pending)
                g_busyFlag = {true, rd32(ram, global(gBusy))};
            wr32(ram, global(gBusy), 1);
            std::memcpy(ram + ((space + kGlobalSavedAtSavePoint) & kRamMask), "\1\0\0\0", 4);
            std::memcpy(ram + ((space + kGlobalSpawn) & kRamMask), ram + ((ped + kPedPosition) & kRamMask), 12);

            // the player as the opcodes find him: CWorld::Players[g218].m_pPed
            const uint32_t info = kPlayers + rd32(ram, global(gPlayer)) * kPlayerSize, me = rd32(ram, info);
            for (uint32_t k = 0; k < sizeof(kAmmoWeapons); ++k)
            {
                uint32_t ammo = 0;
                for (uint32_t s = 0; s < kWeaponSlots; ++s)
                    if (rd32(ram, me + kPedWeapons + s * kWeaponSize) == kAmmoWeapons[k])
                        ammo = rd32(ram, me + kPedWeapons + s * kWeaponSize + kWeaponAmmo);
                wr32(ram, global(gAmmo + k), ammo);
            }
            const int32_t slot = static_cast<int8_t>(ram[(me + kPedCurrentSlot) & kRamMask]);
            wr32(ram, global(gCurrentWeapon), rd32(ram, me + kPedWeapons + static_cast<uint32_t>(slot) * kWeaponSize));

            const uint32_t pool = rd32(ram, kPedPool), handle = rd32(ram, global(gPlayerChar));
            const uint32_t index = static_cast<uint32_t>(static_cast<int32_t>(handle) >> 8);
            const uint32_t armourPed =
                ram[(rd32(ram, pool + 4) + index) & kRamMask] == (handle & 0xFFu) ? rd32(ram, pool) + index * kPedSize : 0;
            wr32(ram, global(gArmour), static_cast<uint32_t>(ps2FpuCvtWS(rdf(ram, armourPed + kPedArmour))));
            wr32(ram, global(gMoney), rd32(ram, info + kPlayerMoney));

            for (uint32_t k = 0; k < 4; ++k)
            {
                uint32_t value = 0xBF800000u; // -1.0f
                if (rd32(ram, global(gPickupFlag + 2 * k)) == 1)
                {
                    const uint32_t h = rd32(ram, global(gPickupFlag + 2 * k + 1)), i = h & 0xFFFFu;
                    const uint32_t entry = kPickups + i * kPickupSize;
                    uint16_t check;
                    std::memcpy(&check, ram + ((entry + kPickupCheck) & kRamMask), 2);
                    const float f = h != 0xFFFFFFFFu && (h >> 16) == check ? rdf(ram, entry + kPickupValue) : 0.0f;
                    value = static_cast<uint32_t>(ps2FpuCvtWS(f));
                }
                wr32(ram, global(gPickupValue + k), value);
            }
            wr32(ram, global(gWanted), rd32(ram, me + kPedWantedLevel));
            wr32(ram, global(gHours), ram[kClockHours & kRamMask]);
            wr32(ram, global(gMinutes), ram[kClockMinutes & kRamMask]);
        }

        std::string fmt(const char *format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }
        void log(const std::string &text) { std::fprintf(stderr, "[lcs-save] %s\n", text.c_str()); }
        void notify(const std::string &text)
        {
            log(text);
            vcs::trainer::notify(text);
        }

        // ---- Requests (any thread -> the game thread's next pad read) ---------------------------------------------
        enum Kind : int
        {
            None,
            Here,
            Quick,
            Load
        };
        // a request: kind | slot << 8 | force << 16 ("force": the control channel's tests skip the safety checks)
        std::atomic<int> g_request{None};
        void request(Kind kind, int slot, bool force);
        std::atomic<bool> g_busy{false};
        std::mutex g_whyMutex;
        std::string g_why = "Not in a game yet";
        std::atomic<uint32_t> g_shownState{0}, g_shownStable{0}; // for `lcssave state`

        // ---- The chain of guest calls inside one scePadRead call (game thread) ----------------------------------
        struct Call
        {
            uint32_t function = 0, a0 = 0, a1 = 0;
        };
        struct Chain
        {
            Kind kind = None;
            std::vector<Call> calls;
            size_t next = 0;        // the call to make next
            bool running = false;   // inside the scePadRead call: its calls return to it
            uint32_t ra = 0, sp = 0; // scePadRead's caller and stack
            uint64_t v0 = 0;         // the pad read's own answer, handed back at the end
            std::vector<uint32_t> results;
            uint32_t saveStatus = 0; // the card status right after SaveSlot
            int slot = 0;            // 1..8
        };
        Chain g_chain;
        PS2Runtime::RecompiledFunction g_padRead = nullptr, g_saveSlot = nullptr, g_deleteSlot = nullptr, g_genericLoad = nullptr;
        PS2Runtime::RecompiledFunction g_newest = nullptr, g_compare = nullptr;

        // VCS_LOAD_SLOT=<1..8> (as VCS's, game/vcs_save_anywhere.cpp: the launcher's Saves > Slot N): the boot's load
        // takes that slot instead of the newest. Only in memory: the date comparison answers for it; the card is never
        // touched. Off after the first load; with the slot not on the card the newest loads, as without it.
        struct BootSlot
        {
            int slot = 0;       // 1..8; 0 off
            bool taken = false; // the current newest-slot run took it
            bool logged = false;
        };
        BootSlot g_boot;

        // game-thread state
        uint32_t g_frame = 0xFFFFFFFFu;
        int g_stable = 0;
        float g_lastZ = 0.0f;
        bool g_keySave = false, g_keyLoad = false;
        struct
        {
            bool armed = false;  // a load was asked for
            bool seen = false;   // the generic load ran
            int slot = 0;
            uint64_t frames = 0; // frames since, until the position is reported
        } g_load;
        uint32_t g_safehouseWatch = 0; // frames left in which a load's stray g1116 is cleared
        int g_menuWatch = 0; // frames a "Save game here" waits for the menu to open
        // save pictures (game/save_shots.h): a save of ours asks for its picture before it starts (the game's Save
        // screen would cover the scene) and waits for it; any save then copies the matching picture to slot<N>.png
        bool g_freshAsked = false;
        int g_freshWait = 0;
        int g_shotSlot = 0;     // 1..8: a save the game's own menu (or our "here" menu) started, picture not made yet
        bool g_shotOurs = false;
        bool g_hereMenu = false; // the game's save menu is open for our "Save game here" (not a save point's)

        void setWhy(const std::string &why)
        {
            std::lock_guard<std::mutex> lock(g_whyMutex);
            g_why = why;
        }

        std::string hostSlotName(int slot)
        {
            for (const Slot &s : slots())
                if (s.slot == slot)
                    return s.name;
            return {};
        }

        // Why a save would not be safe now ("" when it would be)
        std::string reasonNow(const uint8_t *ram)
        {
            const game::Addrs &G = game::addrs();
            const uint32_t ped = rd32(ram, G.world.playerPed);
            if (!ped || !rd32(ram, G.script.space))
                return "Not in a game yet";
            if (ram[(kLcs.menu + kMenuActive) & kRamMask] || ram[(kLcs.menu + kSaveMenuWanted) & kRamMask] ||
                ram[G.timer.userPause & kRamMask] || ram[G.timer.codePause & kRamMask])
                return "Not while the game is paused";
            if (ram[G.script.missionRunning & kRamMask])
                return "Not during a mission";
            // inside a safehouse (g1116, above): its own save point saves there and loads back inside; a save of ours
            // would load without the safehouse set up around the player
            if (rd32(ram, rd32(ram, G.script.space) + gInSafehouse * 4u) == 1)
                return "Inside a safehouse: use its save point";
            if (const int32_t wanted = static_cast<int32_t>(rd32(ram, ped + kPedWantedLevel)); wanted > 0)
                return fmt("Lose your wanted level first (%d)", wanted); // as VCS's save anywhere and the safehouses
            if (rd32(ram, G.world.theCamera + kCameraLooksAtPlayer) != 1)
                return "Not during a cutscene or a scripted camera";
            const uint32_t state = rd32(ram, ped + kPedState);
            if (state == 54 || state == 55)
                return "Not while the player is down";
            if (rd32(ram, ped + G.off.pedInVehicle) || state == 0x32)
                return "Get out of the vehicle first (a save does not keep it)";
            if (state != kPedStateIdle)
                return "Only on foot, standing or walking";
            if (g_stable < kStableFrames)
                return "Wait until you stand still (not falling or jumping)";
            return {};
        }

        void startChain(Kind kind, int slot, std::vector<Call> calls)
        {
            g_chain = {};
            g_chain.kind = kind;
            g_chain.slot = slot;
            g_chain.calls = std::move(calls);
            g_busy = true;
        }

        // The chain's last call returned: what it did
        void finishChain(uint8_t *ram)
        {
            Chain &c = g_chain;
            const uint32_t status = rd32(ram, kLcs.cardManager + kCardStatus);
            if (c.kind == Quick)
            {
                // as the frontend (0x339708): the card status right after SaveSlot says whether it was written
                if (c.results.size() >= 3 && c.saveStatus == kCardStatusOk)
                {
                    const uint32_t ped = rd32(ram, game::addrs().world.playerPed);
                    notify(fmt("Saved in slot %d, here", c.slot));
                    if (ped)
                        log(fmt("slot %d saved at (%.2f, %.2f, %.2f)", c.slot, rdf(ram, ped + kPedPosition),
                                rdf(ram, ped + kPedPosition + 4), rdf(ram, ped + kPedPosition + 8)));
                }
                else
                    notify(fmt("Quick save failed (card status %u, now %u)", c.saveStatus, status));
            }
            else if (c.kind == Load)
            {
                // results: scan, check
                const bool ok = c.results.size() >= 2 && c.results[1] != 0;
                if (ok)
                {
                    // what the frontend's load does after a good check (0x33979C..0x3397D4), minus closing the menu
                    // (none is open): the cleanup was the chain's last call; now the load request
                    ram[(kLcs.menu + kWantToLoad) & kRamMask] = 1;
                    wr32(ram, kLcs.loadRequest, 1);
                    g_load = {};
                    g_load.armed = true;
                    g_load.slot = c.slot;
                    notify(fmt("Loading slot %d...", c.slot));
                }
                else
                    notify(fmt("Slot %d cannot be loaded (missing or damaged)", c.slot));
            }
            c = {};
            g_busy = false;
        }

        // Once a frame (the first pad read of a frame), not inside a chain
        void frameWork(uint8_t *ram)
        {
            const game::Addrs &G = game::addrs();
            restoreBusyFlag(ram); // a save of ours was written since the last frame (the game's save menu)
            const uint32_t ped = rd32(ram, G.world.playerPed);
            // stable: on foot and the height not changing (no fall, no jump)
            if (ped)
            {
                const float z = rdf(ram, ped + kPedPosition + 8);
                const bool still = std::fabs(z - g_lastZ) < 0.05f && rd32(ram, ped + kPedState) == kPedStateIdle;
                g_stable = still ? g_stable + 1 : 0;
                g_lastZ = z;
            }
            else
                g_stable = 0;
            // the free-play picture (save_shots.h): playing, no menu / pause / cutscene / script camera, alive
            {
                const uint32_t st = ped ? rd32(ram, ped + kPedState) : 0;
                saveshots::setFreePlay(ped && rd32(ram, G.script.space) && !ram[(kLcs.menu + kMenuActive) & kRamMask] &&
                                       !ram[(kLcs.menu + kSaveMenuWanted) & kRamMask] && !ram[G.timer.userPause & kRamMask] &&
                                       !ram[G.timer.codePause & kRamMask] && rd32(ram, G.world.theCamera + kCameraLooksAtPlayer) == 1 &&
                                       st != 54 && st != 55);
            }
            // a save the game's own menu made (or our "here" menu): done when the saving flag is down; its picture
            if (g_shotSlot && rd32(ram, kLcs.savingFlag) == 0)
            {
                if (rd32(ram, kLcs.cardManager + kCardStatus) == kCardStatusOk)
                    saveshots::pictureForSlot(g_shotSlot, g_shotOurs);
                g_shotSlot = 0;
            }
            const std::string why = reasonNow(ram);
            setWhy(why);
            g_shownState = ped ? rd32(ram, ped + kPedState) : 0xFFFFFFFFu;
            g_shownStable = static_cast<uint32_t>(g_stable);

            // after a load: where it put the player
            if (g_load.seen && ped && ++g_load.frames == 60)
            {
                const std::string which = g_load.slot ? fmt("slot %d", g_load.slot) : std::string("the boot's save");
                log(fmt("after loading %s the player is at (%.2f, %.2f, %.2f), state %u", which.c_str(),
                        rdf(ram, ped + kPedPosition), rdf(ram, ped + kPedPosition + 4), rdf(ram, ped + kPedPosition + 8),
                        rd32(ram, ped + kPedState)));
                if (g_load.slot)
                    notify(fmt("Slot %d loaded", g_load.slot));
                g_load = {};
            }

            // after a load (any: ours, the boot's, the game's menu): a save made outside a safehouse (g211 == -1) has
            // the load's init script set g1116 = 1 without a safehouse set up (above); put it back to "outside" as the
            // safehouse script would have when he left. Not when a safehouse is open (g1131..1133: the game's own
            // state) or a mission runs (VIC1 / SALS1 / SALS2 write g1116 themselves). Every frame of the window, since
            // the init script may run after the first check.
            if (g_safehouseWatch)
            {
                --g_safehouseWatch;
                const uint32_t space = rd32(ram, G.script.space);
                const auto global = [&](uint32_t index) { return static_cast<int32_t>(rd32(ram, space + index * 4u)); };
                if (space && !ram[G.script.missionRunning & kRamMask] && global(kGlobalSavedAtSavePoint / 4u) == 1 &&
                    global(gSavedSafehouse) == -1 && global(gInSafehouse) == 1 && global(gSafehouseOpen) == 0 &&
                    global(gSafehouseOpen + 1) == 0 && global(gSafehouseOpen + 2) == 0)
                {
                    wr32(ram, space + gInSafehouse * 4u, 0);
                    log("loaded a save made outside a safehouse: 'inside a safehouse' (global 1116) cleared");
                }
            }

            // a "Save game here" waiting for the game's menu
            if (g_menuWatch && ++g_menuWatch > 5)
            {
                if (ram[(kLcs.menu + kMenuActive) & kRamMask])
                    log("the game's save menu is open");
                else if (g_menuWatch < 120)
                    return;
                else
                    log("the save menu did not open");
                g_menuWatch = 0;
                g_busy = false;
            }
            // our "Save game here" menu closed
            if (g_hereMenu && !g_menuWatch && !ram[(kLcs.menu + kMenuActive) & kRamMask] &&
                !ram[(kLcs.menu + kSaveMenuWanted) & kRamMask])
                g_hereMenu = false;

            // the keys: F9 quick save, F10 quick load (not while the settings / a menu has the keyboard)
            const bool keySave = ps2control::keyDown(platform::KEY_F9), keyLoad = ps2control::keyDown(platform::KEY_F10);
            if (!ps2control::gameInputBlocked())
            {
                if (keySave && !g_keySave)
                    quickSave();
                if (keyLoad && !g_keyLoad)
                    quickLoad();
            }
            g_keySave = keySave;
            g_keyLoad = keyLoad;

            const int asked = g_request.exchange(None);
            const int request = asked & 0xFF, slot = (asked >> 8) & 0xFF;
            const bool force = (asked >> 16) & 1;
            if (request == None || g_chain.kind != None || g_menuWatch)
                return;
            if (request == Load)
            {
                if (!ped || !rd32(ram, G.script.space) || ram[(kLcs.menu + kMenuActive) & kRamMask] ||
                    ram[(kLcs.menu + kSaveMenuWanted) & kRamMask])
                    return notify("Quick load: not now (no game running, or a menu is open)");
                if (hostSlotName(slot).empty())
                    return notify(fmt("Quick load: slot %d is empty", slot));
                log(fmt("quick load: slot %d", slot));
                return startChain(Load, slot,
                                  {{kLcs.cardScan, kLcs.cardManager, 0}, {kLcs.loadCheck, uint32_t(slot - 1), 0},
                                   {kLcs.loadCleanup, 0, 0}});
            }
            if (!why.empty() && force && ped)
                log("checks skipped (force): " + why);
            else if (!why.empty())
                return notify((request == Here ? "Save game here: " : "Quick save: ") + why);
            // the picture of the scene first (the save screen / the notices would cover it): the shot is taken at the
            // next host frame; wait for it (not long: a host that draws no frames goes on without)
            if (!g_freshAsked)
            {
                g_freshAsked = true;
                g_freshWait = 0;
                saveshots::requestFresh();
            }
            if (!saveshots::freshDone() && ++g_freshWait < 120)
            {
                g_request = asked;
                return;
            }
            g_freshAsked = false;
            if (request == Here)
            {
                // ACTIVATE_SAVE_MENU's own effect (0x22A844..0x22A8C4), minus its stat (safehouse visits)
                ram[(kLcs.menu + kSaveMenuWanted) & kRamMask] = 1;
                for (uint32_t k = 0; k < 3; ++k)
                {
                    wr32(ram, ped + kPedMoveSpeed + 4 * k, 0);
                    wr32(ram, ped + kPedTurnSpeed + 4 * k, 0);
                }
                log("save menu asked for");
                g_menuWatch = 1;
                g_hereMenu = true;
                g_busy = true;
                return;
            }
            log(fmt("quick save: slot %d", slot));
            // the frontend's save (0x3396E8..0x339714): scan, flag up, SaveSlot, flag down (the hook), scan again
            startChain(Quick, slot,
                       {{kLcs.cardScan, kLcs.cardManager, 0}, {kLcs.saveSlot, kLcs.cardManager, uint32_t(slot - 1)},
                        {kLcs.cardScan, kLcs.cardManager, 0}});
        }

        // ---- Hooks (game thread) ----------------------------------------------------------------------------------
        // scePadRead: the game's own; once a frame the work above, and a chain started: after the pad is read, the
        // chain's calls are made as guest calls whose return address is scePadRead - each return lands here again
        // (same stack pointer) for the next call; the last returns to the pad update with the pad's answer. The world
        // does not move meanwhile (as VCS's quick save, game/vcs_save_anywhere.cpp).
        PS2_GUEST_CC void padReadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Chain &c = g_chain;
            const auto call = [&](const Call &k) {
                if (k.function == kLcs.saveSlot)
                    wr32(rdram, kLcs.savingFlag, 1);
                SET_GPR_U32(ctx, 4, k.a0);
                SET_GPR_U32(ctx, 5, k.a1);
                SET_GPR_U32(ctx, 31, kLcs.padRead);
                ctx->pc = k.function;
            };
            if (c.running && ctx->pc == kLcs.padRead && GPR_U32(ctx, 29) == c.sp)
            {
                const Call &done = c.calls[c.next - 1];
                if (done.function == kLcs.saveSlot)
                {
                    wr32(rdram, kLcs.savingFlag, 0);
                    c.saveStatus = rd32(rdram, kLcs.cardManager + kCardStatus);
                    if (c.saveStatus == kCardStatusOk)
                        saveshots::pictureForSlot(c.slot, true);
                    restoreBusyFlag(rdram);
                }
                c.results.push_back(GPR_U32(ctx, 2));
                // a failed check stops a load (no cleanup)
                if (c.kind == Load && done.function == kLcs.loadCheck && GPR_U32(ctx, 2) == 0)
                    c.next = c.calls.size();
                if (c.next < c.calls.size())
                    return call(c.calls[c.next++]);
                SET_GPR_U64(ctx, 2, c.v0);
                SET_GPR_U32(ctx, 31, c.ra);
                ctx->pc = c.ra;
                finishChain(rdram);
                return;
            }
            const uint32_t ra = GPR_U32(ctx, 31), sp = GPR_U32(ctx, 29);
            const bool entry = ctx->pc == kLcs.padRead;
            g_padRead(rdram, ctx, runtime);
            if (!entry || ctx->pc != ra) // (a resume inside it, or it did not return normally: not now)
                return;
            const uint32_t frame = rd32(rdram, game::addrs().timer.frameCounter);
            if (frame != g_frame)
            {
                g_frame = frame;
                frameWork(rdram);
            }
            if (c.kind != None && !c.running && !c.calls.empty())
            {
                c.running = true;
                c.ra = ra;
                c.sp = sp;
                c.v0 = GPR_U64(ctx, 2);
                c.next = 1;
                call(c.calls[0]);
            }
        }

        // CMemoryCard::SaveSlot: any save (the game's menu or ours), logged
        PS2_GUEST_CC void saveSlotHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kLcs.saveSlot)
            {
                const bool ours = g_chain.kind == Quick || g_hereMenu;
                log(fmt("SaveSlot(slot %u)%s", GPR_U32(ctx, 5) + 1,
                        g_chain.kind == Quick ? " (quick save)" : g_hereMenu ? " (save game here)" : ""));
                // before the original (it may yield mid-body): a save not made through a save point's script gets
                // what that script stores, or it would load as a new game
                if (ours)
                    markSavePoint(rdram);
                if (g_chain.kind != Quick) // (the chain's own picture is made where it ends)
                {
                    g_shotSlot = static_cast<int>(GPR_U32(ctx, 5)) + 1;
                    g_shotOurs = ours;
                }
            }
            g_saveSlot(rdram, ctx, runtime);
        }

        // CMemoryCard::DeleteSlot (card manager, slot 0..7): the frontend's Delete (0x339518) and the card layer's own
        // clean-up of an unreadable slot file before a save (0x2D03F4: saving flag up, then the save's picture comes
        // after it). Its picture goes with the save.
        PS2_GUEST_CC void deleteSlotHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kDeleteSlot && rd32(rdram, kLcs.savingFlag) == 0 && GPR_U32(ctx, 5) < 8)
            {
                log(fmt("DeleteSlot(slot %u)", GPR_U32(ctx, 5) + 1));
                saveshots::removePicture(static_cast<int>(GPR_U32(ctx, 5)) + 1);
            }
            g_deleteSlot(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void newestSlotHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kLcs.newestSlot)
                g_boot.taken = false;
            g_newest(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void dateCompareHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_boot.slot && ctx->pc == kLcs.dateCompare && (ra == kLcs.compareRa1 || ra == kLcs.compareRa2))
            {
                const int slot = static_cast<int>(GPR_U32(ctx, 21)) + 1; // s5: the slot loop's counter (0x2D0994)
                const bool take = slot == g_boot.slot;
                if (take || g_boot.taken)
                {
                    if (take && !g_boot.logged)
                    {
                        log(fmt("boot load: slot %d (VCS_LOAD_SLOT) taken as the newest", slot));
                        g_boot.logged = true;
                    }
                    if (take)
                        g_boot.taken = true;
                    SET_GPR_U32(ctx, 2, take ? 1u : 2u); // 1: newer (taken); 2: older (neither call takes it)
                    ctx->pc = ra;
                    return;
                }
            }
            g_compare(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void genericLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kLcs.genericLoad)
            {
                log(g_load.armed ? fmt("loading the save (slot %d)", g_load.slot) : std::string("loading a save"));
                if (g_boot.slot && !g_boot.logged)
                    log(fmt("boot load: slot %d (VCS_LOAD_SLOT) is not on the card: the newest loads", g_boot.slot));
                g_boot = {};
                const int slot = g_load.slot;
                g_load = {};
                g_load.seen = true;
                g_load.slot = slot;
                g_safehouseWatch = kSafehouseWatchFrames;
            }
            g_genericLoad(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const game::Addrs &G = game::addrs();
        if (const char *e = std::getenv("LCS_SAVE"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs] save anywhere / quick save / quick load off (LCS_SAVE=0)\n");
            return;
        }
        g_padRead = runtime.lookupFunction(kLcs.padRead); // (chains a hook installed before this one)
        g_saveSlot = runtime.lookupFunction(kLcs.saveSlot);
        g_deleteSlot = runtime.lookupFunction(kDeleteSlot);
        g_genericLoad = runtime.lookupFunction(kLcs.genericLoad);
        g_newest = runtime.lookupFunction(kLcs.newestSlot);
        g_compare = runtime.lookupFunction(kLcs.dateCompare);
        if (!g_padRead || !g_saveSlot || !g_genericLoad || !g_newest || !g_compare || !runtime.lookupFunction(kLcs.cardScan) ||
            !runtime.lookupFunction(kLcs.loadCheck) || !runtime.lookupFunction(kLcs.loadCleanup) ||
            !game::all({G.world.playerPed, G.world.theCamera, G.script.space, G.script.missionRunning,
                        G.timer.frameCounter, G.timer.userPause, G.timer.codePause, G.off.pedInVehicle}))
        {
            game::logUnavailable("save anywhere / quick save / quick load");
            return;
        }
        runtime.replaceFunction(kLcs.padRead, &padReadHook);
        runtime.replaceFunction(kLcs.saveSlot, &saveSlotHook);
        if (g_deleteSlot)
            runtime.replaceFunction(kDeleteSlot, &deleteSlotHook);
        runtime.replaceFunction(kLcs.genericLoad, &genericLoadHook);
        runtime.replaceFunction(kLcs.newestSlot, &newestSlotHook);
        runtime.replaceFunction(kLcs.dateCompare, &dateCompareHook);
        if (const char *e = std::getenv("VCS_LOAD_SLOT"); e && std::atoi(e) >= 1 && std::atoi(e) <= 8)
        {
            g_boot.slot = std::atoi(e);
            log(fmt("boot load: slot %d asked for (VCS_LOAD_SLOT)", g_boot.slot));
        }
        std::fprintf(stderr, "[lcs] save anywhere: F9 quick save (slot %d), F10 quick load\n", kQuickSlot);

        // lcssave here | quick [slot] | load [slot] | state | slots
        ps2control::addCommand("lcssave", [](const std::string &args, ps2control::Reply reply) {
            char verb[16] = {}, extra[16] = {};
            int slot = kQuickSlot;
            std::sscanf(args.c_str(), "%15s %d %15s", verb, &slot, extra);
            const std::string v = verb;
            const bool force = std::string(extra) == "force"; // tests: save without the safety checks
            if (slot < 1 || slot > 8)
                return reply("error slot 1..8");
            if (v == "here")
                return request(Here, slot, force), reply("ok");
            if (v == "quick")
                return request(Quick, slot, force), reply("ok");
            if (v == "load")
                return request(Load, slot, false), reply("ok");
            if (v == "slots")
            {
                std::string out = "ok";
                for (const Slot &s : slots())
                    out += fmt(" %d=", s.slot) + (s.name.empty() ? std::string("-") : "\"" + s.name + "\"");
                return reply(out);
            }
            if (v == "state" || v.empty())
            {
                const std::string why = whyNot();
                return reply(fmt("ok busy=%d pedstate=%d stable=%u why=", busy() ? 1 : 0,
                                 static_cast<int>(g_shownState.load()), g_shownStable.load()) +
                             (why.empty() ? std::string("-") : "\"" + why + "\""));
            }
            reply("error usage: lcssave here | quick [slot [force]] | load [slot] | state | slots");
        });
    }

    namespace
    {
        void request(Kind kind, int slot, bool force)
        {
            if (!busy() && slot >= 1 && slot <= 8)
                g_request = kind | slot << 8 | (force ? 1 << 16 : 0);
        }
    }

    void saveHere() { request(Here, kQuickSlot, false); }
    void quickSave(int slot) { request(Quick, slot, false); }
    void quickLoad(int slot) { request(Load, slot, false); }

    std::string whyNot()
    {
        std::lock_guard<std::mutex> lock(g_whyMutex);
        return g_why;
    }

    bool busy() { return g_busy.load(std::memory_order_relaxed) || g_request.load(std::memory_order_relaxed) != None; }

    std::vector<Slot> slots()
    {
        std::vector<Slot> out;
        std::vector<std::filesystem::file_time_type> newest(9);
        for (int s = 1; s <= 8; ++s)
            out.push_back({s, {}});
        std::error_code ec;
        // the save folder (0x398660 "/BASLUS-21423GTA30001", used by the card functions 0x2CED58 / 0x2D0A88)
        const std::filesystem::path folder = PS2Runtime::getIoPaths().mcRoot / "BASLUS-21423GTA30001";
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string file; // the card layer's %XX back to the character (it stores ':' and a trailing '.' so)
            {
                const std::string raw = it->path().filename().string();
                for (size_t k = 0; k < raw.size(); ++k)
                {
                    if (raw[k] == '%' && k + 2 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[k + 1])) &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 2])))
                    {
                        file += static_cast<char>(std::stoi(raw.substr(k + 1, 2), nullptr, 16));
                        k += 2;
                    }
                    else
                        file += raw[k];
                }
            }
            // "<slot>: <name>": 0x2F7168 names them sprintf("%i: ") (0x3D8318) + the last mission passed, cut to 21
            // characters with "..." (0x3D8320); "1: In the beginning..." (ITBEG) before any
            if (file.size() < 4 || file[0] < '1' || file[0] > '8' || file[1] != ':' || file[2] != ' ' ||
                !it->is_regular_file(ec) || it->file_size(ec) == 0)
                continue;
            const int s = file[0] - '0';
            const auto when = it->last_write_time(ec);
            if (!out[s - 1].name.empty() && when < newest[s])
                continue;
            newest[s] = when;
            out[s - 1].name = file.size() > 3 ? file.substr(3) : "?";
        }
        return out;
    }
}
