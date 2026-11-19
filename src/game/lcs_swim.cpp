// [lcs] See lcs_swim.h. How it works (docs/LCS.md "Swimming"):
//
// CPed::ProcessControl (0x1D6F30) clears the ped's in-water flag (+0x140 bit 20) and calls CPed::ProcessBuoyancy
// (0x1D65E0, at 0x1D6FFC) once a frame, before the ped's movement: the buoyancy (mod_Buoyancy 0x60E2B0, 0x2D53F0) pushes
// the ped up; then, when more than ~90% of him is under water (impulse.z / mass > step * 0.008) or his origin is 0.6
// below the surface (the level the buoyancy left at 0x60E330), it damps his speed (pow(0.9, step)), clears bIsStanding
// (+0x190 bit 32), sets +0x198 bit 48 and calls CPed::InflictDamage (0x1C1F78) with step * 3 of WEAPONTYPE_DROWNING
// (0x2B): ~50 health a second, Toni dies in about two seconds (measured: 100 -> 0 in 2.4 s at (1123, -1260)).
//
// With swimming on, ProcessBuoyancy is replaced for the player's ped on foot while he is in deep water: he is held with
// his origin kDepth below the surface (the water level is computed here as CWaterLevel::GetWaterLevel 0x20D970 does);
// the game's own player control picks the move state and heading (walk, run, sprint, turning - slowed here and with a
// little drift), the camera follows as on foot; his speed is set here from the move state, as nothing moves a ped
// without ground under him; nothing hurts him (no drowning, no fall) and his weapon stays down (no aiming or shooting:
// CPlayerPed::ProcessPlayerWeapon is skipped).
//
// The look: the game would show its in-the-air pose (CPed::SetInTheAir 0x1E3410 blends FALL_glide every frame the ped
// is neither standing nor in the air): that is skipped while he swims. With the "Swim animations (from your VCS disc)"
// mod (tools/stories_pack.py swim: Vice City Stories' swim set, read once from the player's own VCS disc into
// <data>/mods/stories_vcs_swim/; the active.txt line `anims set=swim file=...`), the animations are built once into an
// animation group of their own in guest RAM and played: swim_idle (treading water) still, swim (breaststroke) walking or
// running, swim_fast (crawl) sprinting. Without it the game's own idle / walk / run play, slowed (a stand-in).
// The group (layouts read in a run, 2026-11-18, docs/LCS.md "Swimming"): CAnimManager (*0x3D9D24) +0x10 the groups,
// 0x14 bytes each {+0 the anim block, +4 the template associations (0x44 each), +8 count, +0xC first id, +0x10 its
// index} (CAnimManager::GetAnimAssociation 0x1F7488 -> CAnimBlendAssocGroup::GetAnimation 0x1B9F40: templates +
// (id - first) * 0x44); group 53 is empty in LCS (between the ped move groups 31-59) and takes the swim set with ids
// from 0x1C0 (the game's run to 0x1BB). An association {+0 next, +4 prev, +8 u16 flags (1 running, 2 loops, 4 delete
// when faded out, 0x10 partial, 0x20 movement, 0x40 root translation), +0x10 blend, +0x18 node count, +0x1C nodes,
// +0x20 hierarchy, +0x24 blend delta, +0x28 time, +0x2C speed, +0x30 s16 id, +0x32 s16 group}; a node 0x1C {theta0,
// theta1, frame a, frame b (byte offsets), remaining, sequence, association}, one per skeleton frame (24 for a ped);
// a hierarchy 0x24 {+0 sequences, +4 name[0x14], +0x1C s16 count, s16 0xAAAA, +0x20 length}; a sequence 0xC {u16 flags,
// u16 frames, keyframes, u32 bone tag} (tools/stories_pack.py: the keyframes are VCS's as they are). A swim animation
// is started by CAnimManager::BlendAnimation (0x1F7630: f12 delta, a0 clump, a1 group, a2 id) - as a tail call from this
// replacement of ProcessBuoyancy (its return goes back to ProcessControl), never called from here.
//
// Out of the water: where the sea bed rises (a beach, a slope) the ground's collision lifts him out and the game takes
// over again (he lands as from a jump). Jump (Space / Square) from the water: a ledge within reach in front of him
// (a quay's edge, a pontoon, a deck - found with CWorld::ProcessVerticalLine 0x175CA8, called the same way, from 2.5 m
// above the water down to the surface, 0.9 m ahead) is climbed onto; elsewhere a small hop. No climbing up walls
// without a ledge in reach. Enter-vehicle boards a boat next to him (the game refuses it to a ped in the air, so he
// stands for that frame). Other peds, the player in a vehicle, cars in water and the dead go through the original code.
#include "game/lcs_swim.h"

#include "game/game_addrs.h"
#include "game/game_table.h"
#include "game/vcs_modfiles.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lcs::swim
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        // ---- functions ----
        // CPed::ProcessBuoyancy(ped a0): read in the decompile (above); its only caller is CPed::ProcessControl.
        constexpr uint32_t kPedBuoyancy = 0x001D65E0u;
        // CPed::SetInTheAir(ped a0) 0x1E3410: if not bIsInTheAir: set it, fall distance 0, FALL_glide (0x8C) template
        // flags = 0x16, BlendAnimation(4.0, clump, 0, 0x8C) (decompile 2026-11-18)
        constexpr uint32_t kPedSetInTheAir = 0x001E3410u;
        // CPlayerPed::ProcessPlayerWeapon(player a0, pad a1) 0x34AF20: aiming, firing, the weapon's camera (called from
        // CPlayerPed::ProcessControl 0x347030 when the ped may use a weapon)
        constexpr uint32_t kPlayerWeapon = 0x0034AF20u;
        // CAnimManager::BlendAnimation(float delta f12, clump a0, group a1, id a2) -> the association (0x1F7630: fades
        // the clump's other associations of the same partial kind, adds the id's from its template when missing)
        constexpr uint32_t kBlendAnimation = 0x001F7630u;
        // CWorld::ProcessVerticalLine(point a0, z stop f12, colpoint a1, entity* a2, buildings a3, vehicles t0, peds t1,
        // objects t2, dummies t3) 0x175CA8: as CPed::InTheAir 0x1E34C0 calls it (1, 1, 0, 1, 0); the colpoint's point
        // at +0, its normal at +0x20 (each a vector + w; read in a run: point (1124.3, -1306.6, 1.2), normal (0.45, 0.21, 0.87))
        constexpr uint32_t kVerticalLine = 0x00175CA8u;

        // ---- globals ----
        // CWaterLevel's data: GetWaterLevel 0x20D970 loads it from gp - 0x19C8 (gp 0x3D6EF0): +0x100C a 128 x 128 byte
        // grid of 32 m cells (index (y + 2048) / 32 + (x + 2048) / 32 * 128; 0x80 = no water), +4 a pointer to the
        // levels (float per grid value). In a run every level of the main map is 0.0 (the sea); 63.2 for a few cells.
        constexpr uint32_t kWaterData = 0x003D5528u;
        constexpr uint32_t kTimeMs = 0x003D9B70u;   // CTimer::m_snTimeInMilliseconds (lcs_addrs.cpp)
        constexpr uint32_t kTimeStep = 0x003D9B8Cu; // CTimer::ms_fTimeStep (1 = 1/50 s)
        constexpr uint32_t kWind = 0x003D9E2Cu;     // CWeather::Wind (the waves' height: 0.2 + wind * 0.4)
        constexpr uint32_t kAnimManager = 0x003D9D24u; // CAnimManager's object (GetAnimAssociation 0x1F7488 reads it)

        // ---- CPed (LCS layout; evidence in ProcessBuoyancy / ProcessControl / CPed::InTheAir 0x1E34C0) ----
        constexpr uint32_t kPos = 0x30u;          // matrix position x, y, z
        constexpr uint32_t kPhysFlags = 0x48u;    // CPhysical flags: bit 3 bTouchingWater (buoyancy sets / clears it)
        constexpr uint32_t kClump = 0x50u;        // RpClump (its +0x18: the animation plugin, *+0 the associations)
        constexpr uint32_t kSpeed = 0x70u;        // m_vecMoveSpeed (per 1/50 s)
        constexpr uint32_t kPedFlagsHi = 0x194u;  // the upper word of the flags at +0x190: bit 0 bIsStanding (InTheAir
                                                  // lands when it is set; the drowning clears it), bit 11 bIsInTheAir
                                                  // (ProcessControl calls InTheAir while it is set; set in a fall)
        constexpr uint32_t kStanding = 1u << 0;
        constexpr uint32_t kInTheAir = 1u << 11;
        constexpr uint32_t kHitWall = 1u << 14;   // set by the collision while he runs into a wall (seen in runs: on
                                                  // land against a wall, his speed 0; clear in open water)
        constexpr uint32_t kDrownFlags = 0x19Cu;  // +0x198 bit 48: set by the drowning branch
        constexpr uint32_t kDrowning = 1u << 16;
        constexpr uint32_t kFallDist = 0x1A8u;    // float: InTheAir adds the frame's drop, 10 m -> the fall animation
        constexpr uint32_t kState = 0x34Cu;       // u32 m_nPedState (1 idle / walking, 0x36 dying, 0x37 dead, 0x32 driving)
        constexpr uint32_t kHealth = 0x4B8u;      // float
        constexpr uint32_t kInVehicle = 0x538u;   // ProcessBuoyancy returns at once when set (lcs_addrs.cpp)
        constexpr uint32_t kPadSquare = 0x26u;    // s16 CPad (0x408C60) Square / jump, 255 held
        constexpr uint32_t kPadTriangle = 0x28u;  // s16 Triangle / enter-vehicle
        constexpr uint32_t kStateDying = 0x36u, kStateDead = 0x37u;
        constexpr uint32_t kStateJump = 0x29u;    // set by a jump (Space / Square); PlayerControlZelda tests it
        constexpr uint32_t kMoveState = 0x354u;   // u32 1 still, 2 walk, 4 run, 5 sprint (CPlayerPed::ProcessControl
                                                  // 0x347030 maps 2 / 4 / 5 to the wanted speed +0xB54 1.0 / 1.8 / 2.5)
        constexpr uint32_t kHeading = 0x4E0u;    // float m_fRotationCur; forward (-sin, cos)
        constexpr uint32_t kHeadingWant = 0x4E4u; // float m_fRotationDest (the player control sets it; ProcessControl
                                                  // turns +0x4E0 towards it after the buoyancy)

        // ---- associations (see the top) ----
        constexpr uint32_t kAssocFlags = 0x08u, kAssocBlend = 0x10u, kAssocNodeCount = 0x18u, kAssocNodes = 0x1Cu,
                           kAssocHier = 0x20u, kAssocDelta = 0x24u, kAssocTime = 0x28u, kAssocSpeed = 0x2Cu,
                           kAssocId = 0x30u, kAssocGroup = 0x32u, kAssocSize = 0x44u;
        constexpr uint32_t kNodeSize = 0x1Cu, kNodeSeq = 0x14u, kNodeAssoc = 0x18u;
        constexpr uint16_t kFlagRunning = 0x1u, kFlagRepeat = 0x2u, kFlagDeleteFaded = 0x4u, kFlagPartial = 0x10u, kFlagMovement = 0x20u,
                           kFlagTranslation = 0x40u;
        constexpr int16_t kAnimJumpGlide = 0x89, kAnimFallGlide = 0x8C; // group 0 (names in their hierarchies)
        constexpr uint32_t kSwimGroup = 53u;     // empty in LCS (group 52 / 54 are move groups); checked before use
        constexpr uint32_t kSwimFirstId = 0x1C0u;

        // ---- tuning (m/s) ----
        constexpr float kDepth = 0.30f;   // the ped's origin (his hips) this far under the surface: head and shoulders out
        constexpr float kDepthAnims = 0.05f; // with the swim animations: they lower his hips themselves (swim_idle's root
                                             // is 0.3 m down; the strokes lie flat at the root's height)
        constexpr float kEnter = 0.10f;    // swim once the origin is below surface - kDepth + this
        constexpr float kLeave = 0.50f;    // hand back to the game once the ground has lifted him this far above that
        constexpr float kSwimWalk = 1.5f, kSwimRun = 2.8f, kSwimSprint = 4.2f;
        constexpr float kHopSpeed = 0.08f;    // per 1/50 s: a ~1 m hop out of the water (0.13 rose 2.4 m in a run)
        constexpr float kHopForward = 0.03f;  // 1.5 m/s forwards while rising
        constexpr float kClimbSpeed = 0.06f;  // 3 m/s up onto a ledge
        constexpr float kLedgeReach = 2.5f;   // m above the water: the highest ledge he can pull himself onto
        constexpr float kLedgeAhead = 1.0f;   // m in front of his origin where the ledge is looked for
        constexpr float kTurnRate = 2.6f;     // rad/s: how fast he turns in the water (on foot it is near instant)
        constexpr float kWaterGrip = 0.94f;   // per 1/50 s: the share of the speed kept from the last frame (drift)

        PS2Runtime::RecompiledFunction g_orig = nullptr, g_origSetInTheAir = nullptr, g_origWeapon = nullptr;
        std::atomic<bool> g_setting{false}, g_forced{false}, g_installed{false}, g_swimming{false};
        bool g_log = false;
        uint32_t g_testMove = 0; // LCS_SWIM_TEST_MOVE=1/2/4/5: tests (the free camera leaves the pad alone): that move state

        // ---- the swim animations (the mod's .anm; tools/stories_pack.py) ----
        struct Seq
        {
            uint16_t flags = 0, frames = 0;
            uint32_t tag = 0;
            std::vector<uint8_t> data;
        };
        struct Anim
        {
            std::string name;
            uint32_t hint = 0;
            float length = 0.0f;
            std::vector<Seq> seqs;
        };
        enum Slot { kIdle, kBreast, kCrawl, kSlots };
        const char *const kSlotNames[kSlots] = {"swim_idle", "swim", "swim_fast"};
        struct Pack
        {
            bool tried = false, built = false, failed = false;
            std::vector<Anim> anims;
            int slot[kSlots] = {-1, -1, -1}; // index in anims (= id - kSwimFirstId)
            float travel[kSlots] = {};       // m/s the stroke's root moves forwards at its own pace
        } g_pack;

        PS2Runtime *g_runtime = nullptr;
        uint32_t g_probe = 0; // guest scratch for the ledge probe: point (16), colpoint (0x40), entity (4)

        struct Tail
        {
            uint32_t fn = 0;
            uint32_t a[8] = {};
            float f12 = 0.0f;
        };

        struct Run
        {
            uint32_t ped = 0;
            bool swimming = false;
            uint32_t lastState = 0;
            uint32_t jumpUntil = 0;   // game ms: a hop or a climb out of the water is under way
            uint32_t nextLog = 0;
            uint32_t enteredAt = 0;
            bool square = false;
            bool climbing = false;
            float ledgeZ = 0.0f;
            bool triangle = false;
            uint32_t boardUntil = 0;
            int probe = 0;            // 1: the ledge probe was called last frame, read its answer
            bool animsOn = false;     // our swim animation was started (fade it when he leaves the water)
            int playing = -1;
            uint32_t blends = 0;
            bool haveHeading = false;
            float heading = 0.0f;
        } g_run;

        template <typename T> T rd(const uint8_t *ram, uint32_t a)
        {
            T v;
            std::memcpy(&v, ram + (a & kRamMask), sizeof v);
            return v;
        }
        template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & kRamMask), &v, sizeof v); }

        float wrapPi(float a)
        {
            while (a > 3.14159265f)
                a -= 6.2831853f;
            while (a < -3.14159265f)
                a += 6.2831853f;
            return a;
        }

        // CWaterLevel::GetWaterLevel (0x20D970) without its z test (its a4 = 1): the cell's level plus the waves.
        bool waterLevel(const uint8_t *ram, float x, float y, float &level)
        {
            const uint32_t data = rd<uint32_t>(ram, kWaterData);
            if (!data)
                return false;
            const int ix = static_cast<int>((x + 2048.0f) * 0.03125f);
            const int iy = static_cast<int>((y + 2048.0f) * 0.03125f);
            if (ix < 0 || iy < 0 || ix >= 128 || iy >= 128)
                return false;
            const uint8_t cell = rd<uint8_t>(ram, data + 0x100Cu + static_cast<uint32_t>(iy + ix * 0x80));
            if (cell == 0x80)
                return false;
            const uint32_t levels = rd<uint32_t>(ram, data + 4u);
            if (!levels)
                return false;
            level = rd<float>(ram, levels + cell * 4u);
            const float phase = static_cast<float>(rd<uint32_t>(ram, kTimeMs) & 0xFFFu) * 0.0015339355f +
                                (((x + 2048.0f) - static_cast<float>(ix) * 32.0f) +
                                 ((y + 2048.0f) - static_cast<float>(iy) * 32.0f)) * 0.19634375f;
            level += (rd<float>(ram, kWind) * 0.4f + 0.2f) * std::sin(phase);
            return std::isfinite(level);
        }

        // ---- the pack: active.txt's `anims set=swim file=...` (written by tools/stories_pack.py through the mod
        // manager; a disabled mod has no line; VCS_MODS=0 and LCS_SWIM_ANIMS=0 turn it off for a run) ----
        std::filesystem::path modsDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path() / "mods"; }

        bool parsePack(const std::vector<uint8_t> &b, std::vector<Anim> &out, std::string &err)
        {
            size_t p = 0;
            auto need = [&](size_t n) { return p + n <= b.size(); };
            auto u32 = [&]() { uint32_t v; std::memcpy(&v, b.data() + p, 4); p += 4; return v; };
            auto u16 = [&]() { uint16_t v; std::memcpy(&v, b.data() + p, 2); p += 2; return v; };
            if (!need(12) || std::memcmp(b.data(), "STANIM01", 8) != 0)
                return err = "not a Stories animation pack", false;
            p = 8;
            const uint32_t n = u32();
            if (n == 0 || n > 64)
                return err = "bad animation count", false;
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!need(36))
                    return err = "cut short", false;
                Anim a;
                a.name.assign(reinterpret_cast<const char *>(b.data() + p), strnlen(reinterpret_cast<const char *>(b.data() + p), 24));
                p += 24;
                a.hint = u32();
                std::memcpy(&a.length, b.data() + p, 4);
                p += 4;
                const uint32_t count = u32();
                if (count == 0 || count > 64 || !(a.length > 0.0f && a.length < 60.0f))
                    return err = "bad animation " + a.name, false;
                for (uint32_t s = 0; s < count; ++s)
                {
                    if (!need(12))
                        return err = "cut short", false;
                    Seq q;
                    q.flags = u16();
                    q.frames = u16();
                    q.tag = u32();
                    const uint32_t bytes = u32();
                    const uint32_t stride = (q.flags & 2u) ? 16u : 10u;
                    if (q.frames == 0 || bytes != q.frames * stride || !need(bytes) || (q.flags & ~0x13u) || !(q.flags & 1u))
                        return err = "bad sequence in " + a.name, false;
                    q.data.assign(b.begin() + static_cast<long>(p), b.begin() + static_cast<long>(p + bytes));
                    p += bytes + ((4u - bytes % 4u) % 4u);
                    a.seqs.push_back(std::move(q));
                }
                out.push_back(std::move(a));
            }
            return true;
        }

        void loadPack()
        {
            Pack &k = g_pack;
            k.tried = true;
            const char *off = std::getenv("LCS_SWIM_ANIMS");
            const char *mods = std::getenv("VCS_MODS");
            if ((off && *off == '0') || (mods && *mods == '0'))
                return;
            std::ifstream in(modsDir() / "active.txt");
            std::string line, mod = "?", file, fileMod;
            std::vector<std::string> skipped;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::istringstream s(line);
                std::string kind, tok;
                if (!(s >> kind) || kind[0] == '#')
                    continue;
                if (kind == "mod")
                {
                    s >> mod;
                    continue;
                }
                std::string set, f;
                while (s >> tok)
                {
                    if (tok.rfind("set=", 0) == 0)
                        set = tok.substr(4);
                    else if (tok.rfind("file=", 0) == 0)
                        f = tok.substr(5);
                }
                if (kind == "needs" && !vcs::mods::files::available(vcs::mods::files::resolve(f, modsDir())))
                    skipped.push_back(mod);
                else if (kind == "anims" && set == "swim" && !f.empty())
                    file = f, fileMod = mod;
            }
            if (file.empty())
            {
                std::fprintf(stderr, "[lcs-swim] no swim animations (the mod \"Swim animations (from your VCS disc)\": "
                                     "tools/stories_pack.py swim <VCS iso>): the stand-in pose\n");
                return;
            }
            for (const std::string &m : skipped)
                if (m == fileMod)
                    return;
            const vcs::mods::files::Location l = vcs::mods::files::resolve(file, modsDir());
            std::vector<uint8_t> bytes;
            std::string err;
            if (!l.error.empty() || !vcs::mods::files::read(l, bytes, 1u << 20, err) || !parsePack(bytes, k.anims, err))
            {
                std::fprintf(stderr, "[lcs-swim] swim animations %s: %s (the stand-in pose)\n", l.shown.c_str(),
                             (l.error.empty() ? err : l.error).c_str());
                k.anims.clear();
                return;
            }
            for (int s = 0; s < kSlots; ++s)
                for (size_t i = 0; i < k.anims.size(); ++i)
                    if (k.anims[i].name == kSlotNames[s])
                        k.slot[s] = static_cast<int>(i);
            if (k.slot[kIdle] < 0 || k.slot[kBreast] < 0)
            {
                std::fprintf(stderr, "[lcs-swim] swim animations %s: no swim_idle / swim in it\n", l.shown.c_str());
                k.anims.clear();
                return;
            }
            if (k.slot[kCrawl] < 0)
                k.slot[kCrawl] = k.slot[kBreast];
            // the strokes' own speed: the root's forward (y) travel over the loop (its translation keyframes' last y -
            // first y, half floats at +10 / +12 of a 16-byte frame), so the swimming speed matches the arms
            for (int s = 0; s < kSlots; ++s)
            {
                const Anim &a = k.anims[k.slot[s]];
                for (const Seq &q : a.seqs)
                    if ((q.tag & 0xFFFFu) == 0 && (q.flags & 2u) && q.frames > 1)
                    {
                        auto half = [](const uint8_t *p) {
                            const uint16_t h = static_cast<uint16_t>(p[0] | p[1] << 8);
                            const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
                            const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                                                   : std::ldexp(static_cast<float>(m | 0x400), e - 25);
                            return (h & 0x8000) ? -v : v;
                        };
                        const float y0 = half(q.data.data() + 12), y1 = half(q.data.data() + (q.frames - 1) * 16 + 12);
                        k.travel[s] = (y1 - y0) / a.length;
                    }
            }
            if (!(k.travel[kBreast] > 0.3f && k.travel[kBreast] < 5.0f))
                k.travel[kBreast] = 1.3f;
            if (!(k.travel[kCrawl] > 0.3f && k.travel[kCrawl] < 6.0f))
                k.travel[kCrawl] = 2.7f;
            std::fprintf(stderr, "[lcs-swim] swim animations: %zu from %s (mod %s)\n", k.anims.size(), l.shown.c_str(),
                         fileMod.c_str());
        }

        // The pack as animation group kSwimGroup in guest RAM, once (the game thread, on the first swimming frame:
        // CAnimManager and the ped templates exist then). False: the stand-in is used.
        bool buildGroup(uint8_t *ram)
        {
            Pack &k = g_pack;
            if (k.built || k.failed || k.anims.empty())
                return k.built;
            k.failed = true; // until it worked
            const uint32_t mgr = rd<uint32_t>(ram, kAnimManager);
            const uint32_t groups = mgr ? rd<uint32_t>(ram, mgr + 0x10u) : 0;
            if (!groups || !g_runtime)
                return false;
            const uint32_t g0 = groups, gs = groups + kSwimGroup * 0x14u;
            const uint32_t templates = rd<uint32_t>(ram, g0 + 4u), count = rd<uint32_t>(ram, g0 + 8u);
            // the slot must be the empty group it is in LCS (nothing in it, its index in place)
            if (rd<uint32_t>(ram, gs) || rd<uint32_t>(ram, gs + 4u) || rd<uint32_t>(ram, gs + 8u) ||
                rd<uint32_t>(ram, gs + 0x10u) != kSwimGroup || !templates || count < 4u || count > 0x400u)
            {
                std::fprintf(stderr, "[lcs-swim] animation group %u is not free (or no group 0): stand-in pose\n", kSwimGroup);
                return false;
            }
            // the donor: group 0's IDLE_stance (id 3), a full-body looping animation; its node count is the ped
            // skeleton's (24)
            const uint32_t donor = templates + 3u * kAssocSize;
            const uint32_t donorHier = rd<uint32_t>(ram, donor + kAssocHier);
            const uint32_t nodes = rd<uint32_t>(ram, donor + kAssocNodeCount);
            char dname[0x15] = {};
            std::memcpy(dname, ram + ((donorHier + 4u) & kRamMask), 0x14);
            if (std::strcmp(dname, "IDLE_stance") != 0 || nodes == 0 || nodes > 64)
            {
                std::fprintf(stderr, "[lcs-swim] group 0 id 3 is '%s' (%u nodes), not IDLE_stance: stand-in pose\n", dname,
                             nodes);
                return false;
            }
            // node -> bone tag, from the sequences group 0's templates bind to each node
            std::vector<uint32_t> tag(nodes, 0xFFFFFFFFu);
            for (uint32_t t = 0; t < count; ++t)
            {
                const uint32_t tm = templates + t * kAssocSize;
                if (rd<uint32_t>(ram, tm + kAssocNodeCount) != nodes)
                    continue;
                const uint32_t nd = rd<uint32_t>(ram, tm + kAssocNodes);
                if (!nd)
                    continue;
                for (uint32_t i = 0; i < nodes; ++i)
                    if (tag[i] == 0xFFFFFFFFu)
                        if (const uint32_t seq = rd<uint32_t>(ram, nd + i * kNodeSize + kNodeSeq))
                            tag[i] = rd<uint32_t>(ram, seq + 8u);
            }
            // sizes: per animation a template, its nodes, a hierarchy, its sequences and keyframes
            uint32_t total = 0;
            for (const Anim &a : k.anims)
            {
                total += kAssocSize + nodes * kNodeSize + 0x24u + static_cast<uint32_t>(a.seqs.size()) * 0xCu;
                for (const Seq &q : a.seqs)
                    total += (static_cast<uint32_t>(q.data.size()) + 3u) & ~3u;
                total = (total + 15u) & ~15u;
            }
            const uint32_t base = g_runtime->guestMalloc(total, 16u);
            if (!base || (base & kRamMask) + total > kRamMask)
            {
                std::fprintf(stderr, "[lcs-swim] no guest memory for the swim animations (%u bytes): stand-in pose\n", total);
                return false;
            }
            std::memset(ram + (base & kRamMask), 0, total);
            const uint32_t tmplBase = base;                       // the templates, contiguous (GetAnimation indexes them)
            uint32_t at = base + static_cast<uint32_t>(k.anims.size()) * kAssocSize;
            const uint32_t donorHierWord = rd<uint32_t>(ram, donorHier + 0x1Cu) & 0xFFFF0000u; // its 0xAAAA half
            int bound = 0, unbound = 0;
            for (size_t i = 0; i < k.anims.size(); ++i)
            {
                const Anim &a = k.anims[i];
                const uint32_t tm = tmplBase + static_cast<uint32_t>(i) * kAssocSize;
                const uint32_t nd = at;
                at += nodes * kNodeSize;
                const uint32_t hier = at;
                at += 0x24u;
                const uint32_t seqs = at;
                at += static_cast<uint32_t>(a.seqs.size()) * 0xCu;
                bool translation = false;
                for (size_t s = 0; s < a.seqs.size(); ++s)
                {
                    const Seq &q = a.seqs[s];
                    const uint32_t sq = seqs + static_cast<uint32_t>(s) * 0xCu;
                    std::memcpy(ram + (at & kRamMask), q.data.data(), q.data.size());
                    wr<uint16_t>(ram, sq, q.flags);
                    wr<uint16_t>(ram, sq + 2u, q.frames);
                    wr<uint32_t>(ram, sq + 4u, at);
                    wr<uint32_t>(ram, sq + 8u, q.tag);
                    at += (static_cast<uint32_t>(q.data.size()) + 3u) & ~3u;
                    if ((q.tag & 0xFFFFu) == 0 && (q.flags & 2u))
                        translation = true;
                }
                at = (at + 15u) & ~15u;
                wr<uint32_t>(ram, hier, seqs);
                std::memcpy(ram + ((hier + 4u) & kRamMask), a.name.c_str(), std::min<size_t>(a.name.size(), 0x13));
                wr<uint32_t>(ram, hier + 0x1Cu, donorHierWord | static_cast<uint32_t>(a.seqs.size()));
                wr<float>(ram, hier + 0x20u, a.length);
                // the template: the donor's, with its own nodes, hierarchy, flags and id
                std::memcpy(ram + (tm & kRamMask), ram + (donor & kRamMask), kAssocSize);
                wr<uint32_t>(ram, tm, 0u);
                wr<uint32_t>(ram, tm + 4u, 0u);
                uint16_t flags = (a.hint & 1u) ? kFlagRepeat : 0u;
                if (translation)
                    flags |= kFlagMovement | kFlagTranslation; // the root's forward travel taken out, as the walk's
                wr<uint16_t>(ram, tm + kAssocFlags, flags);
                wr<float>(ram, tm + kAssocBlend, 1.0f);
                wr<uint32_t>(ram, tm + kAssocNodeCount, nodes);
                wr<uint32_t>(ram, tm + kAssocNodes, nd);
                wr<uint32_t>(ram, tm + kAssocHier, hier);
                wr<float>(ram, tm + kAssocDelta, 0.0f);
                wr<float>(ram, tm + kAssocTime, 0.0f);
                wr<float>(ram, tm + kAssocSpeed, 1.0f);
                wr<int16_t>(ram, tm + kAssocId, static_cast<int16_t>(kSwimFirstId + i));
                wr<int16_t>(ram, tm + kAssocGroup, static_cast<int16_t>(kSwimGroup));
                const uint32_t donorNodes = rd<uint32_t>(ram, donor + kAssocNodes);
                for (uint32_t n = 0; n < nodes; ++n)
                {
                    const uint32_t node = nd + n * kNodeSize;
                    std::memcpy(ram + (node & kRamMask), ram + ((donorNodes + n * kNodeSize) & kRamMask), kNodeSize);
                    uint32_t seq = 0;
                    for (size_t s = 0; s < a.seqs.size() && !seq; ++s)
                        if (a.seqs[s].tag == tag[n] && tag[n] != 0xFFFFFFFFu)
                            seq = seqs + static_cast<uint32_t>(s) * 0xCu;
                    wr<uint32_t>(ram, node + kNodeSeq, seq);
                    wr<uint32_t>(ram, node + kNodeAssoc, tm);
                    (seq ? bound : unbound)++;
                }
            }
            // the group: group 0's anim block, our templates
            wr<uint32_t>(ram, gs, rd<uint32_t>(ram, g0));
            wr<uint32_t>(ram, gs + 4u, tmplBase);
            wr<uint32_t>(ram, gs + 8u, static_cast<uint32_t>(k.anims.size()));
            wr<uint32_t>(ram, gs + 0xCu, kSwimFirstId);
            k.failed = false;
            k.built = true;
            std::fprintf(stderr, "[lcs-swim] swim animations built: group %u, ids 0x%X-0x%X, %u bytes at 0x%X, %d bones bound, "
                                 "%d not in them\n", kSwimGroup, kSwimFirstId,
                         kSwimFirstId + static_cast<uint32_t>(k.anims.size()) - 1u, total, base, bound, unbound);
            return true;
        }

        // The clump's associations: f(address) for each (the plugin's list, as RpAnimBlendClumpGetAssociation 0x146458)
        template <typename F> void eachAssoc(const uint8_t *ram, uint32_t ped, F f)
        {
            const uint32_t clump = rd<uint32_t>(ram, ped + kClump);
            const uint32_t plugin = clump ? rd<uint32_t>(ram, clump + 0x18u) : 0;
            uint32_t a = plugin ? rd<uint32_t>(ram, plugin) : 0;
            for (int n = 0; a && n < 64; ++n)
            {
                const uint32_t next = rd<uint32_t>(ram, a);
                f(a);
                a = next;
            }
        }

        void fadeOut(uint8_t *ram, uint32_t a, float rate)
        {
            wr<uint16_t>(ram, a + kAssocFlags, rd<uint16_t>(ram, a + kAssocFlags) | kFlagDeleteFaded);
            if (rd<float>(ram, a + kAssocDelta) > -rate)
                wr<float>(ram, a + kAssocDelta, -rate);
        }

        // The stroke for a move state and its pace (the animation's speed; the swimming speed follows from it):
        // walk -> breaststroke slowly, run -> breaststroke, sprint -> crawl
        int strokeOf(uint32_t move) { return move == 5u ? kCrawl : (move == 2u || move == 4u) ? kBreast : kIdle; }
        float paceOf(uint32_t move) { return move == 2u ? 0.9f : move == 4u ? 1.35f : move == 5u ? 1.15f : 1.0f; }

        // The look while he floats: the in-the-air glide faded out; the swim animation for his move state started (a
        // tail call into BlendAnimation when it isn't the one fully playing), its pace from his speed. Without the pack
        // the game's own walk / run / idle play, slowed.
        void animFrame(uint8_t *ram, uint32_t ped, uint32_t move, Tail &tail)
        {
            Run &r = g_run;
            const bool pack = buildGroup(ram);
            const int want = !pack ? -1 : g_pack.slot[strokeOf(move)];
            const int16_t wantId = static_cast<int16_t>(kSwimFirstId + want);
            uint32_t mine = 0;
            bool rival = false;
            eachAssoc(ram, ped, [&](uint32_t a) {
                const int16_t id = rd<int16_t>(ram, a + kAssocId);
                const int16_t group = rd<int16_t>(ram, a + kAssocGroup);
                const uint16_t flags = rd<uint16_t>(ram, a + kAssocFlags);
                if (group == 0 && (id == kAnimFallGlide || id == kAnimJumpGlide))
                    fadeOut(ram, a, 4.0f);
                if (!pack)
                {
                    // the stand-in: the ped move groups' walk / run / sprint (ids 0-2) at a swimmer's pace
                    if (group >= 31 && id >= 0 && id <= 2)
                        wr<float>(ram, a + kAssocSpeed, 0.55f);
                    return;
                }
                if (group == static_cast<int16_t>(kSwimGroup) && id == wantId)
                    mine = a;
                else if (!(flags & kFlagPartial) && group != static_cast<int16_t>(kSwimGroup) &&
                         rd<float>(ram, a + kAssocBlend) > 0.0f)
                {
                    // the game's own idle / walk / run, which its move code keeps blending back in: held at nothing
                    // while the stroke plays (kept, so it isn't added again every frame; deleted when he leaves)
                    if (mine || rd<float>(ram, a + kAssocDelta) >= 0.0f)
                        rival = true;
                }
            });
            if (mine && rd<float>(ram, mine + kAssocBlend) > 0.5f)
            {
                rival = false;
                eachAssoc(ram, ped, [&](uint32_t a) {
                    const uint16_t flags = rd<uint16_t>(ram, a + kAssocFlags);
                    if (!(flags & kFlagPartial) && rd<int16_t>(ram, a + kAssocGroup) != static_cast<int16_t>(kSwimGroup))
                    {
                        // still running (an association at nothing stops running - its RUNNING bit cleared - and
                        // the game's move code blends it back in without starting it: out of the water he stood in
                        // the run pose, going nowhere)
                        wr<float>(ram, a + kAssocBlend, 0.0f);
                        wr<float>(ram, a + kAssocDelta, 0.0f);
                        wr<uint16_t>(ram, a + kAssocFlags, static_cast<uint16_t>((flags & ~kFlagDeleteFaded) | kFlagRunning));
                    }
                });
            }
            if (!pack)
                return;
            const bool settled = mine && rd<float>(ram, mine + kAssocDelta) >= 0.0f &&
                                 (rd<float>(ram, mine + kAssocBlend) >= 0.999f || rd<float>(ram, mine + kAssocDelta) > 0.0f);
            if (mine)
            {
                wr<float>(ram, mine + kAssocSpeed, paceOf(move));
            }
            if ((!settled || rival) && tail.fn == 0)
            {
                const uint32_t clump = rd<uint32_t>(ram, ped + kClump);
                tail.fn = kBlendAnimation;
                tail.a[0] = clump;
                tail.a[1] = kSwimGroup;
                tail.a[2] = static_cast<uint32_t>(static_cast<uint16_t>(wantId));
                tail.f12 = 3.0f;
                ++r.blends;
                if (g_log && want != r.playing)
                    std::fprintf(stderr, "[lcs-swim] animation %s (blend call %u)\n", g_pack.anims[want].name.c_str(), r.blends);
            }
            r.playing = want;
            r.animsOn = true;
        }

        // Out of the water: our animations fade (and are deleted); the game puts its own back.
        void animsOff(uint8_t *ram, uint32_t ped)
        {
            Run &r = g_run;
            if (!r.animsOn)
                return;
            r.animsOn = false;
            r.playing = -1;
            eachAssoc(ram, ped, [&](uint32_t a) {
                if (rd<int16_t>(ram, a + kAssocGroup) == static_cast<int16_t>(kSwimGroup))
                    fadeOut(ram, a, 8.0f);
                else if (!(rd<uint16_t>(ram, a + kAssocFlags) & kFlagPartial))
                {
                    // the game's own: running again; held at nothing -> deleted, its move code adds what it needs
                    wr<uint16_t>(ram, a + kAssocFlags, rd<uint16_t>(ram, a + kAssocFlags) | kFlagRunning);
                    if (rd<float>(ram, a + kAssocBlend) <= 0.0f)
                        fadeOut(ram, a, 1.0f);
                }
            });
        }

        // The ledge probe: ProcessVerticalLine from kLedgeReach above the water down to it, kLedgeAhead in front.
        void probeLedge(uint8_t *ram, float x, float y, float level, float fx, float fy, Tail &tail)
        {
            if (!g_probe && g_runtime)
                g_probe = g_runtime->guestMalloc(0x60u, 16u);
            if (!g_probe)
                return;
            wr<float>(ram, g_probe, x + fx * kLedgeAhead);
            wr<float>(ram, g_probe + 4u, y + fy * kLedgeAhead);
            wr<float>(ram, g_probe + 8u, level + kLedgeReach + 0.3f);
            wr<float>(ram, g_probe + 0xCu, 0.0f);
            std::memset(ram + ((g_probe + 0x10u) & kRamMask), 0, 0x44u);
            wr<float>(ram, g_probe + 0x10u + 8u, -1.0e9f); // the colpoint's z: untouched = no hit
            tail.fn = kVerticalLine;
            tail.a[0] = g_probe;
            tail.a[1] = g_probe + 0x10u;
            tail.a[2] = g_probe + 0x50u;
            tail.a[3] = 1u; // buildings
            tail.a[4] = 1u; // vehicles
            tail.a[5] = 0u; // peds
            tail.a[6] = 1u; // objects
            tail.a[7] = 0u; // dummies
            tail.f12 = level + 0.1f;
        }

        // The probe's answer: the top's height, or NAN (none within reach, a slope, or no probe)
        float ledgeAnswer(const uint8_t *ram, float level)
        {
            if (!g_probe)
                return NAN;
            const float z = rd<float>(ram, g_probe + 0x10u + 8u), nz = rd<float>(ram, g_probe + 0x10u + 0x28u);
            if (g_log)
            {
                std::fprintf(stderr, "[lcs-swim] ledge probe: hit z %.2f normal z %.2f entity 0x%x (water %.2f) colpoint",
                             z, nz, rd<uint32_t>(ram, g_probe + 0x50u), level);
                for (uint32_t i = 0; i < 0x40u; i += 4)
                    std::fprintf(stderr, " %08x", rd<uint32_t>(ram, g_probe + 0x10u + i));
                std::fprintf(stderr, "\n");
            }
            if (!(z > level + 0.15f && z < level + kLedgeReach + 0.25f) || !(nz > 0.6f))
                return NAN;
            return z;
        }

        // The swimming frame (instead of the buoyancy). False: not swimming, run the original.
        bool swimFrame(uint8_t *ram, uint32_t ped, Tail &tail)
        {
            Run &r = g_run;
            if (r.ped != ped)
                r = Run{}, r.ped = ped;
            const uint32_t state = rd<uint32_t>(ram, ped + kState);
            const float x = rd<float>(ram, ped + kPos), y = rd<float>(ram, ped + kPos + 4), z = rd<float>(ram, ped + kPos + 8);
            float level = 0.0f;
            const uint32_t now = rd<uint32_t>(ram, kTimeMs);
            const bool dead = state == kStateDying || state == kStateDead || rd<float>(ram, ped + kHealth) <= 0.0f;
            if (dead || rd<uint32_t>(ram, ped + kInVehicle) || !std::isfinite(z) || !waterLevel(ram, x, y, level))
            {
                r.swimming = false, r.climbing = false, r.probe = 0, r.haveHeading = false;
                animsOff(ram, ped);
                return false;
            }
            if (!g_pack.tried)
                loadPack();
            const float target = level - (g_pack.anims.empty() || g_pack.failed ? kDepth : kDepthAnims);
            float vz = rd<float>(ram, ped + kSpeed + 8);
            float step = rd<float>(ram, kTimeStep);
            if (!(step > 0.0f) || step > 3.0f)
                step = 1.0f;
            if (state != r.lastState && g_log)
                std::fprintf(stderr, "[lcs-swim] state %u -> %u at z %.2f (water %.2f)\n", r.lastState, state, z, level);
            r.lastState = state;
            // the jump button, read from the pad (CPad 0 +0x26 Square, the other state's copy at +0x56; found by holding
            // the keys in a run: Triangle +0x28, Cross +0x2A, the left stick's y +0x4), its edge kept here every frame
            // the player is over water; not while the controls are disabled (cutscenes: +0x82)
            const uint32_t pad = game::addrs().world.pad0;
            const bool controls = pad && rd<int16_t>(ram, pad + game::addrs().off.padDisableControls) == 0;
            const bool square = controls && rd<int16_t>(ram, pad + kPadSquare) != 0;
            const bool pressed = square && !r.square;
            r.square = square;
            const bool triangle = controls && rd<int16_t>(ram, pad + kPadTriangle) != 0;
            const bool enterPressed = triangle && !r.triangle;
            r.triangle = triangle;

            const float heading = rd<float>(ram, ped + kHeading);
            const float fx = -std::sin(heading), fy = std::cos(heading); // the ped's forward (PlayerControlZelda 0x349168)

            // a hop or a climb out of the water under way: the game's buoyancy (which would see him under water) stays
            // off. Climbing: up at kClimbSpeed until his feet are over the ledge, then forwards onto it
            if (now < r.jumpUntil && r.jumpUntil - now < 10000u && (vz > 0.0f || r.climbing))
            {
                animsOff(ram, ped);
                if (r.climbing)
                {
                    if (z < r.ledgeZ + 1.05f)
                    {
                        wr<float>(ram, ped + kSpeed + 8, kClimbSpeed);
                        wr<float>(ram, ped + kSpeed, fx * 0.004f);
                        wr<float>(ram, ped + kSpeed + 4, fy * 0.004f);
                        r.jumpUntil = now + 600u;
                    }
                    else
                    {
                        // over the edge: forwards onto the top, then the game's own landing
                        r.climbing = false;
                        r.jumpUntil = now + 400u;
                        wr<float>(ram, ped + kSpeed + 8, 0.02f);
                        wr<float>(ram, ped + kSpeed, fx * 0.05f);
                        wr<float>(ram, ped + kSpeed + 4, fy * 0.05f);
                        if (g_log)
                            std::fprintf(stderr, "[lcs-swim] climbed onto the ledge at (%.1f, %.1f, %.2f)\n", x, y, z);
                    }
                    wr<float>(ram, ped + kFallDist, 0.0f);
                    const uint32_t hi = rd<uint32_t>(ram, ped + kPedFlagsHi);
                    wr<uint32_t>(ram, ped + kPedFlagsHi, (hi & ~kStanding) | kInTheAir);
                }
                else
                {
                    wr<float>(ram, ped + kSpeed, fx * kHopForward);
                    wr<float>(ram, ped + kSpeed + 4, fy * kHopForward);
                }
                return true;
            }
            r.climbing = false;
            if (r.swimming && z > target + kLeave)
            {
                // the ground lifted him out (a beach, a slope): he lands as from a jump - in the air, going down, so
                // CPed::InTheAir (0x1E34C0) finds the ground within 1.3 m and calls SetLanding (0x1E3680), which ends
                // the in-the-air animation (without it he stood in that pose, his run going nowhere)
                r.swimming = false, r.haveHeading = false;
                animsOff(ram, ped);
                const uint32_t hi = rd<uint32_t>(ram, ped + kPedFlagsHi);
                wr<uint32_t>(ram, ped + kPedFlagsHi, hi | kInTheAir);
                if (vz > -0.02f)
                    wr<float>(ram, ped + kSpeed + 8, -0.02f);
                if (g_log)
                    std::fprintf(stderr, "[lcs-swim] out of the water at (%.1f, %.1f, %.2f), water %.2f\n", x, y, z, level);
                return false;
            }
            else if (!r.swimming && z < target + kEnter)
                r.swimming = true, r.enteredAt = now;
            if (!r.swimming)
            {
                animsOff(ram, ped);
                return false;
            }

            // the jump button: the ledge probe this frame, its answer the next: a ledge within reach -> climb onto it,
            // else a hop. (A jump from a deck or a quay that ends in the water leaves the jump state set - it waits for a
            // landing: back to swimming still.)
            if (state == kStateJump && !pressed && r.probe == 0)
                wr<uint32_t>(ram, ped + kState, 1u);
            if (r.probe == 1)
            {
                r.probe = 0;
                const float ledge = ledgeAnswer(ram, level);
                r.jumpUntil = now + 1500u;
                r.swimming = false;
                r.haveHeading = false;
                animsOff(ram, ped);
                wr<float>(ram, ped + kFallDist, 0.0f);
                const uint32_t hi = rd<uint32_t>(ram, ped + kPedFlagsHi);
                wr<uint32_t>(ram, ped + kPedFlagsHi, (hi & ~kStanding) | kInTheAir); // lands as from any jump
                if (std::isfinite(ledge))
                {
                    r.climbing = true;
                    r.ledgeZ = ledge;
                    wr<float>(ram, ped + kSpeed + 8, kClimbSpeed);
                    wr<float>(ram, ped + kSpeed, 0.0f);
                    wr<float>(ram, ped + kSpeed + 4, 0.0f);
                    if (g_log)
                        std::fprintf(stderr, "[lcs-swim] ledge %.2f m above the water at (%.1f, %.1f): climbing\n",
                                     ledge - level, x, y);
                }
                else
                {
                    wr<float>(ram, ped + kSpeed + 8, kHopSpeed);
                    wr<float>(ram, ped + kSpeed, fx * kHopForward);
                    wr<float>(ram, ped + kSpeed + 4, fy * kHopForward);
                    if (g_log)
                        std::fprintf(stderr, "[lcs-swim] no ledge in reach at (%.1f, %.1f): a hop\n", x, y);
                }
                return true;
            }
            if (pressed && (r.jumpUntil == 0 || now >= r.jumpUntil + 500u || now + 10000u < r.jumpUntil) &&
                z >= target - 0.3f)
            {
                probeLedge(ram, x, y, level, fx, fy, tail);
                r.probe = 1;
                if (state == kStateJump)
                    wr<uint32_t>(ram, ped + kState, 1u);
            }
            else if (pressed && state == kStateJump)
                wr<uint32_t>(ram, ped + kState, 1u); // back in the water: swimming (still) again

            // turning: slower than on foot (the control's wanted heading, +0x4E4, held to kTurnRate from the current)
            {
                const float wantH = rd<float>(ram, ped + kHeadingWant);
                if (std::isfinite(wantH) && std::isfinite(heading))
                {
                    const float d = wrapPi(wantH - heading), lim = kTurnRate * step / 50.0f;
                    if (std::fabs(d) > lim)
                        wr<float>(ram, ped + kHeadingWant, wrapPi(heading + (d > 0.0f ? lim : -lim)));
                }
            }

            // the move: the game's player control sets the move state (+0x354: 1 still, 2 walk, 4 run, 5 sprint) and
            // the heading; in water nothing moves him (no ground), so the speed is set here along his heading - with
            // the water's drag: the speed follows the heading slowly, so a turn drifts
            const uint32_t move = g_testMove ? g_testMove : rd<uint32_t>(ram, ped + kMoveState);
            float speed = (move == 2u ? kSwimWalk : move == 4u ? kSwimRun : move == 5u ? kSwimSprint : 0.0f) / 50.0f;
            if (g_pack.built && strokeOf(move) != kIdle)
                speed = g_pack.travel[strokeOf(move)] * paceOf(move) / 50.0f; // as fast as the stroke pulls him
            const float kh = 1.0f - std::pow(kWaterGrip, step);
            float vx = rd<float>(ram, ped + kSpeed), vy = rd<float>(ram, ped + kSpeed + 4);
            vx += (fx * speed - vx) * kh;
            vy += (fy * speed - vy) * kh;
            wr<float>(ram, ped + kSpeed, vx);
            wr<float>(ram, ped + kSpeed + 4, vy);

            // held at the surface: a spring towards the swimming depth, quick but not a snap (a fall into the water
            // stops within about a metre)
            const float err = target - z;
            float want = err * 0.3f;
            want = want < -0.12f ? -0.12f : want > 0.06f ? 0.06f : want;
            const float k = 1.0f - std::pow(0.5f, step);
            vz += (want - vz) * k;
            wr<float>(ram, ped + kSpeed + 8, vz);
            if (z < level - 6.0f) // never left deep under: straight back up
                wr<float>(ram, ped + kPos + 8, target);

            // not standing: a standing ped's speed is his animation's move delta (CPed::UpdatePosition 0x1D5E58, later
            // in ProcessControl: speed xy = +0x4D8 / +0x4DC), which stays 0 in water - he would not move. Not falling
            // (the game sets bIsInTheAir again when it finds no ground; CPed::InTheAir 0x1E34C0 then only adds to the
            // fall distance, kept at 0 here, so no fall animation and no fall damage), not drowning, touching water
            // (the wake). The enter-vehicle key (Triangle: F) is refused to a ped in the air: on the frame it is pressed
            // (and while the game walks him to a vehicle's door) he stands, so a boat next to him can be boarded.
            const uint32_t hi = rd<uint32_t>(ram, ped + kPedFlagsHi);
            if (enterPressed)
                r.boardUntil = now + 3000u;
            const bool board = now < r.boardUntil && r.boardUntil - now < 10000u && (enterPressed || state != 1u);
            wr<uint32_t>(ram, ped + kPedFlagsHi, board ? ((hi | kStanding) & ~kInTheAir) : (hi & ~(kStanding | kInTheAir)));
            wr<uint32_t>(ram, ped + kDrownFlags, rd<uint32_t>(ram, ped + kDrownFlags) & ~kDrowning);
            wr<uint32_t>(ram, ped + kPhysFlags, rd<uint32_t>(ram, ped + kPhysFlags) | 8u);
            wr<float>(ram, ped + kFallDist, 0.0f);

            if (!board)
                animFrame(ram, ped, move, tail);

            if (g_log && now >= r.nextLog)
            {
                r.nextLog = now + 1000u;
                std::fprintf(stderr, "[lcs-swim] swimming at (%.1f, %.1f, %.2f) water %.2f speed %.2f m/s move %u state %u "
                             "hp %.0f heading %.2f anim %d blends %u\n", x, y, z, level, std::sqrt(vx * vx + vy * vy) * 50.0f,
                             move, state, rd<float>(ram, ped + kHealth), heading, r.playing, r.blends);
            }
            return true;
        }

        bool isPlayer(const uint8_t *rdram, uint32_t ped)
        {
            return ped && ped == rd<uint32_t>(rdram, game::addrs().world.playerPed);
        }

        PS2_GUEST_CC void pedBuoyancy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = GPR_U32(ctx, 4);
            bool swim = false;
            Tail tail;
            if (enabled() && isPlayer(rdram, ped))
            {
                g_runtime = runtime;
                swim = swimFrame(rdram, ped, tail);
                g_swimming.store(swim, std::memory_order_relaxed);
            }
            if (swim)
            {
                if (tail.fn)
                {
                    // a tail call: the function runs in place of the rest of this one and returns to ProcessControl
                    for (int i = 0; i < 8; ++i)
                        SET_GPR_U32(ctx, 4 + i, tail.a[i]);
                    ctx->f[12] = tail.f12;
                    ctx->pc = tail.fn;
                    return;
                }
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_orig(rdram, ctx, runtime);
        }

        // CPed::SetInTheAir while he swims: nothing (no glide pose, and he isn't "in the air": InTheAir stays off)
        PS2_GUEST_CC void pedSetInTheAir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_swimming.load(std::memory_order_relaxed) && enabled() && isPlayer(rdram, GPR_U32(ctx, 4)))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origSetInTheAir(rdram, ctx, runtime);
        }

        // CPlayerPed::ProcessPlayerWeapon while he swims: nothing (no aiming, no shooting from the water)
        PS2_GUEST_CC void playerWeapon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_swimming.load(std::memory_order_relaxed) && enabled() && isPlayer(rdram, GPR_U32(ctx, 4)))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origWeapon(rdram, ctx, runtime);
        }
    }

    bool enabled() { return g_installed.load(std::memory_order_relaxed) && (g_forced || g_setting); }
    bool swimming() { return g_swimming.load(std::memory_order_relaxed); }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "enhance.swimming";
        s.page = "Enhancements";
        s.section = "Gameplay";
        s.label = "Swimming";
        s.help = "Toni swims instead of drowning: he floats in deep water and moves with the normal on-foot controls "
                 "(walk, run, sprint), walks out up a beach, climbs onto a low quay or deck with jump and boards a boat "
                 "with enter-vehicle. With the mod \"Swim animations (from your VCS disc)\" (Mods: made from your own "
                 "Vice City Stories disc) he swims with Vic's animations; without it a stand-in pose. Off: as the "
                 "original game (and the islands stay out of reach until the story opens them).";
        s.type = settings::Type::Toggle;
        s.defaultValue = 0;
        s.env = "LCS_SWIM";
        s.onChange = [](int v) { g_setting.store(v != 0, std::memory_order_relaxed); };
        if (const char *d = std::getenv("STORIES_DUALWORLD"); d && *d && *d != '0')
        {
            s.locked = [] { return true; };
            s.lockedValue = 1;
            s.lockedNote = "On while both cities are shown (STORIES_DUALWORLD).";
        }
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("LCS_SWIM_LOG");
        g_log = e && e[0] == '1';
        if (const char *m = std::getenv("LCS_SWIM_TEST_MOVE"))
            g_testMove = static_cast<uint32_t>(std::atoi(m));
        if (const char *d = std::getenv("STORIES_DUALWORLD"); d && *d && *d != '0')
            g_forced.store(true, std::memory_order_relaxed);
        if (const char *h = std::getenv("LCS_SWIM_HOOK"); h && h[0] == '0')
        {
            std::fprintf(stderr, "[lcs-swim] off (LCS_SWIM_HOOK=0): the player drowns as in the original\n");
            return;
        }
        if (!(g_orig = runtime.lookupFunction(kPedBuoyancy)) || !(g_origSetInTheAir = runtime.lookupFunction(kPedSetInTheAir)) ||
            !(g_origWeapon = runtime.lookupFunction(kPlayerWeapon)))
        {
            game::logUnavailable("swimming (lcs_swim: no CPed::ProcessBuoyancy / SetInTheAir / ProcessPlayerWeapon)");
            return;
        }
        runtime.replaceFunction(kPedBuoyancy, &pedBuoyancy);
        runtime.replaceFunction(kPedSetInTheAir, &pedSetInTheAir);
        runtime.replaceFunction(kPlayerWeapon, &playerWeapon);
        g_runtime = &runtime;
        g_installed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[lcs-swim] CPed::ProcessBuoyancy, SetInTheAir, ProcessPlayerWeapon hooked (swimming %s)\n",
                     g_forced ? "forced on: STORIES_DUALWORLD" : "by the Swimming setting");
    }
}
