// See vcs_save_anywhere.h and docs/TRAINER.md "Save anywhere". Fork addition (gta-stories-recompiled), not upstream's.
#include "game/vcs_save_anywhere.h"

#include "game/save_shots.h"

#include "debug/rig.h"
#include "game/vcs_freecam.h"
#include "game/vcs_frontend.h"
#include "game/vcs_trainer.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "ui/settings_menu.h"
#include "ui/trainer_menu.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
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
#include <thread>

namespace vcs::saveanywhere
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        // ---- Game addresses (game-internals.md 10, 11, 12; the trainer's) ---------------------------------------------
        constexpr uint32_t kPlayerPed = 0x004E4910u;      // CWorld::Players[0].m_pPed
        constexpr uint32_t kScriptSpacePtr = 0x0048741Cu; // 0 until the script engine runs
        constexpr uint32_t kActiveScripts = 0x004CD3C4u;  // CRunningScript active list (+0 next, +0x10 IP)
        constexpr uint32_t kGameTimeMs = 0x004CD104u;     // CTimer::m_snTimeInMilliseconds: stands still while paused
        // Play time before this session, ms: the save writes kGameTimeMs + this (FUN_003ac9c8, 0x3acaa0..0x3acaac:
        // block +0x20), the load puts that value here (FUN_003acef0, 0x3acf64), a new game sets 0 (0x3baf40); the
        // stats' time played adds the two (0x28b40c). So right after a load it is the loaded save's own stamp.
        constexpr uint32_t kPlayTimeBase = 0x00488284u;
        constexpr uint32_t kCurrArea = 0x00489F7Cu;       // CGame::currArea (0 outdoors)
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8: a mission runs (the safehouse scripts check it too)
        constexpr uint32_t kCameraLooksAtPlayer = 0x006F44D0u + 0x888u; // TheCamera: 0 while a script camera holds it
        constexpr uint32_t kCardManager = 0x00510480u;    // card manager, port 0 (status +0x54: 0x1B fine)
        constexpr uint32_t kCardStatusOk = 0x1Bu;
        constexpr uint32_t kSavingFlag = 0x0048A664u;     // up while FUN_00108128 saves
        // ped fields
        constexpr uint32_t kPedPosition = 0x30u;
        constexpr uint32_t kPedVehicle = 0x480u;
        constexpr uint32_t kPedState = 0x8C4u; // 1 on foot, 0x37 driving, 0x39 / 0x3A dying / dead

        // ---- Game functions ------------------------------------------------------------------------------------------
        constexpr uint32_t kPadRead = 0x00449488u;    // scePadRead: called by the pad update, between frames
        constexpr uint32_t kCardScan = 0x00276BF0u;   // FUN_00276bf0(manager, port): list the card, find the slots
        constexpr uint32_t kSaveSlot = 0x002778A8u;   // FUN_002778a8(manager, slot 0..7): write the save
        constexpr uint32_t kGenericLoad = 0x003ADAE0u; // world init's load of a save (game-internals.md 10d)
        // The newest slot (game-internals.md 10d): FUN_002762a8(manager, port, name out) rescans the card
        // (FUN_00277318: each slot's header date into manager + 0x4C0 + slot * 0x18) and keeps the slot whose date
        // FUN_00274c98(manager, candidate, best) calls newer: 1 takes it (call at 0x276378), otherwise a second call
        // (0x2763dc) takes it on 0; 2 never. Its callers (the boot's card check FUN_00277688, the boot's load
        // FUN_003ae820) load the name it gives.
        constexpr uint32_t kNewestSlot = 0x002762A8u;
        constexpr uint32_t kDateCompare = 0x00274C98u;
        constexpr uint32_t kCompareRa1 = 0x00276380u, kCompareRa2 = 0x002763E4u; // FUN_002762a8's two calls return here

        // ---- Script commands (VCS numbers, checked in game: game-internals.md 11e, docs/TRAINER.md) -------------
        constexpr uint16_t SET_CHAR_COORDINATES = 0x0044, STORE_WANTED_LEVEL = 0x0110, LOAD_SCENE = 0x0253,
                           ACTIVATE_SAVE_MENU = 0x0260, SET_AREA_VISIBLE = 0x02EA, TERMINATE = 0x0023;

        constexpr int kStableFrames = 45;   // frames standing / walking in one area before a save is allowed
        constexpr float kRestoreDistance = 10.0f; // metres: closer than this, the load already put him there

        struct Code // script bytecode, as the trainer's Scm (tools/rig.py's Scm)
        {
            std::vector<uint8_t> bytes;
            Code &op(uint16_t c) { return put(c & 0xFF).put(c >> 8); }
            Code &i(int32_t v)
            {
                put(0x06);
                for (int k = 0; k < 4; ++k)
                    put((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF);
                return *this;
            }
            Code &f(float v)
            {
                uint32_t b;
                std::memcpy(&b, &v, 4);
                put(0x09);
                for (int k = 0; k < 4; ++k)
                    put((b >> (8 * k)) & 0xFF);
                return *this;
            }
            Code &player() { return put(0xCF).put(0xA5); }
            Code &local(int n) { return put(static_cast<uint32_t>(0x0D + n)); }
            Code &put(uint32_t b)
            {
                bytes.push_back(static_cast<uint8_t>(b));
                return *this;
            }
            uint32_t start(uint8_t *ram)
            {
                op(TERMINATE);
                return vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), "saveany");
            }
        };

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

        std::string fmt(const char *format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        void log(const std::string &text) { std::fprintf(stderr, "[save-anywhere] %s\n", text.c_str()); }

        struct Spot
        {
            float x = 0, y = 0, z = 0;
            uint32_t area = 0;
        };

        // ---- The position file: <data>/save_anywhere.txt, "slot stamp x y z area" per line -----------------------------
        struct Line
        {
            uint32_t stamp = 0;
            Spot spot;
        };
        std::map<int, Line> g_lines; // by slot; game thread
        bool g_linesRead = false;

        std::filesystem::path dataDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path(); }
        std::filesystem::path linesPath() { return dataDir() / "save_anywhere.txt"; }

        void readLines()
        {
            if (g_linesRead)
                return;
            g_linesRead = true;
            std::ifstream in(linesPath());
            for (std::string text; std::getline(in, text);)
            {
                std::istringstream s(text);
                int slot = 0;
                Line l;
                if (s >> slot >> l.stamp >> l.spot.x >> l.spot.y >> l.spot.z >> l.spot.area && slot >= 1 && slot <= 8)
                    g_lines[slot] = l;
            }
        }

        void writeLines()
        {
            std::error_code ec;
            const std::filesystem::path path = linesPath(), tmp = path.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                for (const auto &[slot, l] : g_lines)
                    out << fmt("%d %u %.3f %.3f %.3f %u\n", slot, l.stamp, l.spot.x, l.spot.y, l.spot.z, l.spot.area);
                if (!out)
                    return log("cannot write " + tmp.string());
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec)
                log("cannot write " + path.string() + ": " + ec.message());
        }

        // ---- State ------------------------------------------------------------------------------------------------
        enum Kind : int
        {
            None,
            Menu,
            Quick
        };
        std::atomic<int> g_request{None}; // any thread -> the next running frame kick
        std::atomic<bool> g_busy{false};
        std::atomic<bool> g_restore{true};
        std::mutex g_whyMutex;
        std::string g_why = "Not in a game yet";

        enum class Phase
        {
            Idle,
            CheckWanted, // STORE_WANTED_LEVEL into a local of our script
            MenuOpening, // ACTIVATE_SAVE_MENU started: the menu opens and pauses the game
            MenuOpen,    // until the game runs again
            Direct,      // the quick save's chain, at the next scePadRead
        };
        Phase g_phase = Phase::Idle;  // game thread (the hooks run on it too)
        Kind g_kind = None;
        uint32_t g_script = 0;
        uint64_t g_phaseFrames = 0;
        Spot g_spot;                  // where the player stands for the save being made

        uint32_t g_lastTime = 0;
        Spot g_last;
        int g_stable = 0;

        // a slot save seen (FUN_002778a8's entry), made sure of at the next frame kick
        struct SaveSeen
        {
            bool pending = false, ours = false;
            int slot = 0;
            uint32_t stamp = 0;
            Spot spot;
        };
        SaveSeen g_saveSeen;
        int g_savedSlot = 0; // the last save checkSave found made here (the quick save's answer)

        // after a load: find its line, then (once the world has settled) put the player back
        struct AfterLoad
        {
            bool seen = false;   // FUN_003adae0 ran
            bool looked = false; // its stamp was compared with the lines
            int slot = 0;        // the line that matched (0: none)
            Spot spot;
            uint64_t frames = 0;
            uint32_t script = 0; // the restore's script
            int tries = 0;
            uint64_t blocked = 0;  // running frames the restore waited for (a mission flag / the ped's state)
            std::string blockedBy; // what it waits for, logged when it changes
        };
        AfterLoad g_after;
        constexpr uint64_t kSettleFrames = 90; // ~3 s at 30 fps: the load's fade-in and the safehouse scripts' first run
        // At the boot's load the start-up scripts hold the mission flag, and the ped is not on foot yet, for a while
        // after the settle (2026-11-17: refused at once, 173 m away): waited for up to this many running frames
        constexpr uint64_t kBlockedFrames = 900; // ~30 s at 30 fps

        // the quick save's chain inside one scePadRead call (game thread)
        enum class Step
        {
            Off,
            Armed,  // the next scePadRead starts it
            Scan,   // FUN_00276bf0 running
            Save,   // FUN_002778a8 running
            Rescan, // FUN_00276bf0 running
            Done,
        };
        struct Chain
        {
            Step step = Step::Off;
            uint32_t slot = 0; // 0..7
            uint32_t ra = 0, sp = 0, v0 = 0, status = 0;
        };
        Chain g_chain;

        PS2Runtime::RecompiledFunction g_padRead = nullptr, g_saveSlot = nullptr, g_load = nullptr;
        PS2Runtime::RecompiledFunction g_newest = nullptr, g_compare = nullptr;

        // VCS_LOAD_SLOT=<1..8> (the launcher's Saves > Slot N): the boot's load takes that slot instead of the newest.
        // Only in memory: the date comparison answers for it; the card and its files are never touched. Off after the
        // first load; with the slot not on the card (no valid header date) the newest loads, as without it.
        struct BootSlot
        {
            int slot = 0;       // 1..8; 0 off
            bool taken = false; // the current FUN_002762a8 run took it
            bool logged = false;
        };
        BootSlot g_boot;

        // A script of the trainer's / rig's (code in its own slot) is in the active list: a save now would store it.
        bool codeScriptActive(const uint8_t *ram)
        {
            const uint32_t space = rd32(ram, kScriptSpacePtr);
            for (uint32_t s = rd32(ram, kActiveScripts), n = 0; s && n < 256; s = rd32(ram, s), ++n)
            {
                const uint32_t at = space + rd32(ram, s + 0x10);
                if (at - (s + 0x114u) < 0xB0u)
                    return true;
            }
            return false;
        }

        Spot playerSpot(const uint8_t *ram, uint32_t ped)
        {
            const uint32_t car = rd32(ram, ped + kPedVehicle);
            const uint32_t at = car && rd32(ram, ped + kPedState) == 0x37 ? car : ped; // in a car the ped's own matrix is not
            return {rdf(ram, at + kPedPosition), rdf(ram, at + kPedPosition + 4), rdf(ram, at + kPedPosition + 8),
                    rd32(ram, kCurrArea)};
        }

        // ---- Hooks (game thread) ------------------------------------------------------------------------------------
        // scePadRead: normally the game's own. Armed, it reads the pad, keeps the answer, and then calls the menu
        // action's sequence as guest calls whose return address is scePadRead itself - each return lands here again
        // (same stack pointer) for the next step; the last returns to the pad update with the pad's answer. The
        // world does not move meanwhile (one pad read, between frames).
        PS2_GUEST_CC void padReadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Chain &c = g_chain;
            const bool resumed = c.step != Step::Off && c.step != Step::Armed && c.step != Step::Done &&
                                 ctx->pc == kPadRead && GPR_U32(ctx, 29) == c.sp;
            const auto call = [&](uint32_t function, uint32_t a1, Step next) {
                SET_GPR_U32(ctx, 4, kCardManager);
                SET_GPR_U32(ctx, 5, a1);
                SET_GPR_U32(ctx, 31, kPadRead);
                ctx->pc = function;
                c.step = next;
            };
            if (!resumed)
            {
                const bool start = c.step == Step::Armed && ctx->pc == kPadRead && !codeScriptActive(rdram);
                const uint32_t ra = GPR_U32(ctx, 31), sp = GPR_U32(ctx, 29);
                g_padRead(rdram, ctx, runtime);
                if (!start || ctx->pc != ra) // (not armed; or the read did not return normally: next time)
                    return;
                c.ra = ra;
                c.sp = sp;
                c.v0 = GPR_U32(ctx, 2);
                log(fmt("quick save: slot %u", c.slot + 1));
                return call(kCardScan, 0, Step::Scan);
            }
            switch (c.step)
            {
            case Step::Scan:
                wr32(rdram, kSavingFlag, 1);
                return call(kSaveSlot, c.slot, Step::Save);
            case Step::Save:
                wr32(rdram, kSavingFlag, 0);
                c.status = rd32(rdram, kCardManager + 0x54);
                return call(kCardScan, 0, Step::Rescan);
            default: // Rescan: back to the pad update with the pad's answer
                SET_GPR_U32(ctx, 2, c.v0);
                SET_GPR_U32(ctx, 31, c.ra);
                ctx->pc = c.ra;
                c.step = Step::Done;
                return;
            }
        }

        // FUN_002778a8(manager, slot): the slot save, from the Save menu (safehouse or ours) or the quick save.
        PS2_GUEST_CC void saveSlotHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSaveSlot) // a call (a resume after a checkpoint comes with pc inside the function)
            {
                const bool ours = g_phase == Phase::MenuOpening || g_phase == Phase::MenuOpen || g_phase == Phase::Direct;
                g_saveSeen = {true, ours, static_cast<int>(GPR_U32(ctx, 5) & 0xFFu) + 1,
                              rd32(rdram, kGameTimeMs) + rd32(rdram, kPlayTimeBase), g_spot};
            }
            g_saveSlot(rdram, ctx, runtime);
        }

        // FUN_002762a8(manager, port, name out): a new choice of the newest slot
        PS2_GUEST_CC void newestSlotHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kNewestSlot)
                g_boot.taken = false;
            g_newest(rdram, ctx, runtime);
        }

        // FUN_00274c98(manager, candidate date, best date): called by FUN_002762a8 only, for each slot with a valid
        // header date (the slot, 0..7, in its s5). With a boot slot: the chosen slot is always the newer, any other
        // never once it was taken.
        PS2_GUEST_CC void dateCompareHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_boot.slot && ctx->pc == kDateCompare && (ra == kCompareRa1 || ra == kCompareRa2))
            {
                const int slot = static_cast<int>(GPR_U32(ctx, 21)) + 1;
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

        PS2_GUEST_CC void loadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kGenericLoad)
            {
                if (g_boot.slot) // the boot's load: the choice is done with
                {
                    if (!g_boot.logged)
                        log(fmt("boot load: slot %d (VCS_LOAD_SLOT) is not on the card (or its header unreadable): "
                                "the newest loads", g_boot.slot));
                    g_boot = {};
                }
                g_after = AfterLoad{};
                g_after.seen = true;
                if (g_chain.step == Step::Armed) // (a quick save asked for just before: not into the load)
                    g_chain.step = Step::Off;
            }
            g_load(rdram, ctx, runtime);
        }

        // ---- Frame kick -------------------------------------------------------------------------------------------
        std::string reasonNow(uint8_t *ram, uint32_t ped, bool inGame)
        {
            if (!inGame)
                return "Not in a game yet";
            if (ram[kMissionRunning])
                return "Not during a mission";
            if (rd32(ram, kCameraLooksAtPlayer) != 1)
                return "Not during a cutscene or a scripted camera";
            if (freecam::flying())
                return "Turn the free camera off first";
            const uint32_t state = rd32(ram, ped + kPedState);
            if (state == 0x39 || state == 0x3A)
                return "Not while the player is down";
            if (rd32(ram, ped + kPedVehicle) || state == 0x37)
                return "Get out of the vehicle first (a save does not keep it)";
            if (state != 1)
                return "Only on foot, standing or walking";
            if (g_stable < kStableFrames)
                return "Wait until you stand still (not falling, jumping or going through a door)";
            return {};
        }

        void finishRequest(const std::string &notice)
        {
            if (!notice.empty())
                vcs::trainer::notify(notice);
            g_phase = Phase::Idle;
            g_kind = None;
            g_script = 0;
            g_busy = false;
        }

        // A save seen at the last frame(s): made? (the saving flag is down, the card manager's status fine)
        void checkSave(uint8_t *ram)
        {
            if (!g_saveSeen.pending || rd32(ram, kSavingFlag) != 0 || g_chain.step == Step::Scan ||
                g_chain.step == Step::Save || g_chain.step == Step::Rescan)
                return;
            SaveSeen s = g_saveSeen;
            g_saveSeen = {};
            const uint32_t status = g_chain.step == Step::Done ? g_chain.status : rd32(ram, kCardManager + 0x54);
            if (status != kCardStatusOk)
            {
                log(fmt("slot %d: the save failed (card status %u)", s.slot, status));
                return;
            }
            saveshots::pictureForSlot(s.slot, s.ours); // (game/save_shots.h)
            readLines();
            if (!s.ours)
            {
                // the game's own save (a safehouse): the slot's line belonged to the save it replaced
                if (g_lines.erase(s.slot))
                {
                    writeLines();
                    log(fmt("slot %d saved at a safehouse: its position line dropped", s.slot));
                }
                return;
            }
            g_savedSlot = s.slot;
            g_lines[s.slot] = {s.stamp, s.spot};
            writeLines();
            log(fmt("slot %d saved here: stamp %u at (%.2f, %.2f, %.2f) area %u", s.slot, s.stamp, s.spot.x, s.spot.y,
                    s.spot.z, s.spot.area));
            vcs::trainer::notify(fmt("Saved in slot %d, here", s.slot));
        }

        void afterLoad(uint8_t *ram, uint32_t ped, bool running, bool inGame)
        {
            AfterLoad &a = g_after;
            if (!a.seen)
                return;
            if (a.script && vcs::rig::scriptActive(ram, a.script))
            {
                if (!running) // paused before it ran: out, so no save stores it; again when the game runs
                {
                    vcs::rig::cancelCodeScript(ram, a.script);
                    a.script = 0;
                }
                return;
            }
            if (a.script) // it ran
            {
                vcs::trainer::notify(fmt("Back where you saved (slot %d)", a.slot));
                a = {};
                return;
            }
            if (!running || !inGame)
                return;
            if (!a.looked)
            {
                a.looked = true;
                readLines();
                const uint32_t stamp = rd32(ram, kPlayTimeBase);
                for (const auto &[slot, l] : g_lines)
                    if (l.stamp == stamp)
                    {
                        a.slot = slot;
                        a.spot = l.spot;
                    }
                if (!a.slot)
                {
                    log(fmt("a save was loaded (stamp %u): not one saved here", stamp));
                    a = {};
                    return;
                }
                log(fmt("slot %d's save-here save was loaded (stamp %u)", a.slot, stamp));
            }
            if (++a.frames < kSettleFrames)
                return;
            const Spot now = playerSpot(ram, ped);
            const float d = std::sqrt((now.x - a.spot.x) * (now.x - a.spot.x) + (now.y - a.spot.y) * (now.y - a.spot.y) +
                                      (now.z - a.spot.z) * (now.z - a.spot.z));
            if (!a.blocked)
                log(fmt("after the load the player is at (%.2f, %.2f, %.2f) area %u, %.1f m from where he saved", now.x, now.y,
                    now.z, now.area, d));
            if (d < kRestoreDistance && now.area == a.spot.area)
            {
                a = {};
                return;
            }
            if (!g_restore)
            {
                log("not put back: switched off");
                a = {};
                return;
            }
            const uint32_t state = rd32(ram, ped + kPedState);
            const std::string why = ram[kMissionRunning]                     ? "a mission runs"
                                    : rd32(ram, kCameraLooksAtPlayer) != 1   ? "a script camera holds the view"
                                    : rd32(ram, ped + kPedVehicle) || state == 0x37 ? "the player is in a vehicle"
                                    : state != 1                              ? fmt("the player is not on foot (state 0x%X)", state)
                                                                              : std::string();
            if (!why.empty())
            {
                if (why != a.blockedBy)
                {
                    log("waiting to put the player back: " + why);
                    // which scripts hold it: +0x217 the mission, +0x20A mission-type, +0x20F the name (game-internals 11b)
                    std::string list;
                    for (uint32_t sc = rd32(ram, kActiveScripts), n = 0; sc && n < 128; sc = rd32(ram, sc), ++n)
                    {
                        const uint32_t o = sc & kRamMask;
                        if (!ram[o + 0x20A] && !ram[o + 0x217])
                            continue;
                        char name[9] = {};
                        std::memcpy(name, ram + o + 0x20F, 8);
                        list += fmt(" %s%s", name, ram[o + 0x217] ? "(M)" : "");
                    }
                    log("  mission-type scripts:" + (list.empty() ? std::string(" none") : list));
                    a.blockedBy = why;
                }
                if (++a.blocked < kBlockedFrames)
                    return;
                log("not put back: " + why + " for too long");
                // e.g. a save from early in the story: the load starts the opening mission (ch_jerr) at once, and the
                // player must not be moved out of a mission (2026-11-17)
                vcs::trainer::notify(fmt("Slot %d: not moved back to where you saved (%s)", a.slot, why.c_str()));
                a = {};
                return;
            }
            if (!a.blockedBy.empty())
                log(fmt("free to put the player back after %llu frames", static_cast<unsigned long long>(a.blocked)));
            a.blockedBy.clear();
            Code code; // as the trainer's teleports: the area first, the scene loaded, then the player
            code.op(SET_AREA_VISIBLE).i(static_cast<int32_t>(a.spot.area));
            code.op(LOAD_SCENE).f(a.spot.x).f(a.spot.y).f(a.spot.z);
            code.op(SET_CHAR_COORDINATES).player().f(a.spot.x).f(a.spot.y).f(a.spot.z);
            a.script = code.start(ram);
            if (!a.script && ++a.tries > 30)
            {
                log("not put back: no free script slot");
                a = {};
            }
        }
    }

    bool loading() { return g_after.seen; } // (a bool: read across threads, a stale answer is harmless)

    void install(PS2Runtime &runtime)
    {
        // after VCS_TEST_SAVE's / VCS_DUMP_RAM's scePadRead hook (vcs_overrides.cpp): that one is then called inside
        g_padRead = runtime.lookupFunction(kPadRead);
        g_saveSlot = runtime.lookupFunction(kSaveSlot);
        g_load = runtime.lookupFunction(kGenericLoad);
        if (!g_padRead || !g_saveSlot || !g_load)
            return log("the game's pad read / save / load functions are not there: off");
        runtime.replaceFunction(kPadRead, &padReadHook);
        runtime.replaceFunction(kSaveSlot, &saveSlotHook);
        runtime.replaceFunction(kGenericLoad, &loadHook);
        if (const char *e = std::getenv("VCS_LOAD_SLOT"); e && *e)
        {
            const int slot = std::atoi(e);
            g_newest = runtime.lookupFunction(kNewestSlot);
            g_compare = runtime.lookupFunction(kDateCompare);
            if (slot < 1 || slot > 8)
                log(fmt("VCS_LOAD_SLOT=%s: not a slot (1..8): the newest loads", e));
            else if (!g_newest || !g_compare)
                log("VCS_LOAD_SLOT: the game's newest-slot functions are not there: the newest loads");
            else
            {
                g_boot.slot = slot;
                runtime.replaceFunction(kNewestSlot, &newestSlotHook);
                runtime.replaceFunction(kDateCompare, &dateCompareHook);
                log(fmt("VCS_LOAD_SLOT=%d: the boot's load takes slot %d", slot, slot));
            }
        }
        // control channel (tests): saveanywhere here | quick | state
        ps2control::addCommand("saveanywhere", [](const std::string &args, ps2control::Reply reply) {
            if (args == "here")
                saveHere();
            else if (args == "quick")
                quickSave();
            else if (args != "state" && !args.empty())
                return reply("error usage: saveanywhere here | quick | state");
            const std::string why = whyNot();
            reply(fmt("ok busy=%d restore=%d why=", busy() ? 1 : 0, restoreAfterLoad() ? 1 : 0) + (why.empty() ? "-" : why));
        });
    }

    void onGameFrame(uint8_t *ram)
    {
        if (!g_padRead)
            return;
        const uint32_t time = rd32(ram, kGameTimeMs);
        const bool running = time != g_lastTime;
        g_lastTime = time;
        const uint32_t ped = rd32(ram, kPlayerPed);
        const bool inGame = rd32(ram, kScriptSpacePtr) != 0 && ped != 0;
        saveshots::setFreePlay(running && inGame && !vcs::frontend::open() && rd32(ram, kCameraLooksAtPlayer) == 1 &&
                                 !freecam::flying());

        if (running && inGame)
        {
            const Spot now = playerSpot(ram, ped);
            const float dx = now.x - g_last.x, dy = now.y - g_last.y, dz = now.z - g_last.z;
            const bool still = now.area == g_last.area && dx * dx + dy * dy < 0.6f * 0.6f && std::fabs(dz) < 0.12f;
            g_stable = still ? std::min(g_stable + 1, 1000) : 0;
            g_last = now;
        }
        else if (!inGame)
            g_stable = 0;
        const std::string why = reasonNow(ram, ped, inGame);
        {
            std::lock_guard<std::mutex> lock(g_whyMutex);
            g_why = why;
        }

        checkSave(ram);
        afterLoad(ram, ped, running, inGame);
        if (g_chain.step == Step::Done)
            g_chain = {};

        switch (g_phase)
        {
        case Phase::Idle:
        {
            if (!running) // (the pause menu, the side panel): the request waits for the game to run
                break;
            const int kind = g_request.exchange(None);
            if (kind == None)
                break;
            g_busy = true;
            if (!why.empty())
            {
                finishRequest("Cannot save here: " + why);
                break;
            }
            g_kind = static_cast<Kind>(kind);
            Code code;
            code.op(STORE_WANTED_LEVEL).player().local(0);
            g_script = code.start(ram);
            if (!g_script)
            {
                finishRequest("Cannot save now: the game's script slots are all in use");
                break;
            }
            g_phase = Phase::CheckWanted;
            g_phaseFrames = 0;
            break;
        }
        case Phase::CheckWanted:
            if (vcs::rig::scriptActive(ram, g_script))
            {
                if (!running) // paused before it ran: take it out (no save may hold it) and ask again
                {
                    vcs::rig::cancelCodeScript(ram, g_script);
                    g_request = g_kind;
                    g_phase = Phase::Idle;
                }
                break;
            }
            if (const int32_t wanted = static_cast<int32_t>(rd32(ram, g_script + 0x54)); wanted > 0)
            {
                finishRequest(fmt("Cannot save here: lose your wanted level first (%d)", wanted));
                break;
            }
            if (!why.empty())
            {
                finishRequest("Cannot save here: " + why);
                break;
            }
            g_spot = playerSpot(ram, ped);
            saveshots::requestFresh(); // its picture, before the menu or the notices show (onHostFrame)
            g_phaseFrames = 0;
            g_savedSlot = 0;
            if (g_kind == Quick)
            {
                g_chain = {Step::Armed, static_cast<uint32_t>(kQuickSlot - 1)};
                g_phase = Phase::Direct;
                break;
            }
            {
                Code code;
                code.op(ACTIVATE_SAVE_MENU); // what a safehouse runs: the game's Save menu, the game paused under it
                g_script = code.start(ram);
            }
            if (!g_script)
            {
                finishRequest("Cannot save now: the game's script slots are all in use");
                break;
            }
            g_phase = Phase::MenuOpening;
            break;
        case Phase::MenuOpening:
            if (!running)
            {
                if (vcs::rig::scriptActive(ram, g_script)) // paused by something else before it ran: again later
                {
                    vcs::rig::cancelCodeScript(ram, g_script);
                    g_request = g_kind;
                    g_phase = Phase::Idle;
                    break;
                }
                g_phase = Phase::MenuOpen; // the menu is up
                break;
            }
            if (++g_phaseFrames > 90)
                finishRequest("The save menu did not open");
            break;
        case Phase::MenuOpen:
            if (running) // closed: saved or not, checkSave has seen it
                finishRequest({});
            break;
        case Phase::Direct:
            if (g_chain.step == Step::Off) // done (checkSave has told when it worked)
            {
                finishRequest(g_savedSlot == kQuickSlot ? std::string()
                                                        : fmt("Quick save to slot %d failed (see the log)", kQuickSlot));
                break;
            }
            if (g_chain.step == Step::Armed && (!running || !inGame)) // paused (or left play) first: ask again later
            {
                g_chain = {};
                g_request = g_kind;
                g_phase = Phase::Idle;
                break;
            }
            if (g_chain.step == Step::Armed && ++g_phaseFrames > 300)
            {
                g_chain = {};
                finishRequest("Quick save did not start");
            }
            break;
        }
    }

    void onHostFrame()
    {
        saveshots::onHostFrame(); // the save pictures (game/save_shots.h)
    }

    void saveHere()
    {
        if (!busy())
            g_request = Menu;
    }

    void quickSave()
    {
        if (!busy())
            g_request = Quick;
    }

    std::string whyNot()
    {
        std::lock_guard<std::mutex> lock(g_whyMutex);
        return g_why;
    }

    bool busy() { return g_busy.load(std::memory_order_relaxed) || g_request.load(std::memory_order_relaxed) != None; }
    bool restoreAfterLoad() { return g_restore.load(std::memory_order_relaxed); }
    void setRestoreAfterLoad(bool on) { g_restore = on; }

    std::vector<Slot> slots()
    {
        static std::mutex mutex;
        static std::vector<Slot> cached;
        static auto last = std::chrono::steady_clock::time_point{};
        std::lock_guard<std::mutex> lock(mutex);
        const auto t = std::chrono::steady_clock::now();
        if (!cached.empty() && t - last < std::chrono::seconds(1))
            return cached;
        last = t;
        cached.clear();
        std::vector<std::filesystem::file_time_type> newest(9);
        for (int s = 1; s <= 8; ++s)
            cached.push_back({s, {}});
        std::error_code ec;
        const std::filesystem::path folder = PS2Runtime::getIoPaths().mcRoot / "BASLUS-21590GTA40001";
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string file = it->path().filename().string();
            // "<slot> <name>", as the game's slot scan takes them (10c): big enough, a digit 1..8 first
            if (file.size() < 3 || file[0] < '1' || file[0] > '8' || file[1] != ' ' || !it->is_regular_file(ec) ||
                it->file_size(ec) < 0x18000u)
                continue;
            const int s = file[0] - '0';
            const auto when = it->last_write_time(ec);
            if (!cached[s - 1].name.empty() && when < newest[s])
                continue;
            newest[s] = when;
            std::string name; // the card layer's %XX back to the character
            for (size_t k = 2; k < file.size(); ++k)
            {
                if (file[k] == '%' && k + 2 < file.size() && std::isxdigit(static_cast<unsigned char>(file[k + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(file[k + 2])))
                {
                    name += static_cast<char>(std::stoi(file.substr(k + 1, 2), nullptr, 16));
                    k += 2;
                }
                else
                    name += file[k];
            }
            cached[s - 1].name = name.empty() ? "?" : name;
        }
        return cached;
    }
}

// [android] The side panel's save rows call saveHere / quickSave from src/android/side_panel.cpp (nativeSaveAnywhere)
