// [lcs] See lcs_script_timing.h. LCS's twin of game/vcs_script_timing.cpp (docs/game-internals.md 11f): a mission
// script runs once a frame, so a `WAIT 0` loop that adds a constant to a meter fills it with the frame rate.
//
// LCS's script VM (GTA III's layout, VCS's argument encoding): ProcessOneCommand 0x10A7A8 reads the opcode at the
// script's IP (+0x18, relative to ScriptSpace *0x3CF0F4), moves the IP past it and calls the handler of its hundred -
// ProcessCommands0To99 0x10A9A0 (a1 the opcode, a0 the script; switch through the table at 0x3A6320). Opcodes
// 0x08 / 0x09 add an int / float constant to a global, 0x0A / 0x0B to a local, 0x0C - 0x0F subtract (GTA III's
// numbers): GetPointerToScriptVariable 0x109A28 (type byte: < 0x0C the timers +0x200 / +0x204, < 0x6C local n - 0x0C
// from the frame base +0x208, < 0xCC a local array, < 0xE6 a global ((t - 0xCC) << 8 | next byte) x 4 into
// ScriptSpace, else a global array), then CollectParameters 0x1094C8 for the constant (type 1 / 2 zero, 3 / 4 / 5 the
// top 1 / 2 / 3 bytes of a float, 6 / 9 four bytes, 7 / 8 a signed byte / short, >= 0x0A a variable's value).
//
// The listed sites (kSites: the script's name and the opcode's IP) are done here with the constant scaled by the
// frame's step over the PS2's (clamped 0..2; integers carry the fraction to the next pass), only while the frame is
// not the PS2's; anything else goes to the game's handler. LCS_SCRIPT_TIMING=0 leaves them all per frame.
//
// ROTATE_OBJECT (0x0352) and SLIDE_OBJECT (0x0353) move an object by the step they are given on every call, and the
// missions call them once a frame until the object arrives: the steps are scaled the same way (a hook on the object
// pool's GetAt, which both call after reading their arguments into ScriptParams 0x3DF320).
//
// LCS_SCRIPT_TIMING_LOG=1 (the survey): every 20 s of game time, each add / subtract of a constant that ran on most
// frames - script, IP, the instruction, its runs per frame - so a mission's per-frame meters show up while it plays;
// also the rotate / slide calls and listed sites that were scaled.
#include "game/lcs_script_timing.h"

#include "game/game_table.h"
#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

namespace lcs::script_timing
{
    namespace
    {
        constexpr uint32_t kCommands0To99 = 0x0010A9A0u; // ProcessCommands0To99(a0 script, a1 opcode)
        constexpr uint32_t kScriptSpace = 0x003CF0F4u;   // -> ScriptSpace (gp-0x7DFC; game_addrs script.space)
        constexpr uint32_t kTimeStep = 0x003D9B8Cu, kTimeMs = 0x003D9B70u, kFrameCounter = 0x003D9B6Cu;
        constexpr uint32_t kIp = 0x18u, kName = 0x10u, kLocals = 0x60u, kFrameBase = 0x208u;
        constexpr float kPs2Step = 50.0f / 30.0f;

        // Per-frame adds in `WAIT 0` loops (one WAIT a pass), from the MAIN.SCM survey (docs/LCS.md); the offset is the
        // opcode's from the mission's start (the IP minus *kMissionBase). All three feed ROTATE_OBJECT / SLIDE_OBJECT
        // steps or an object's angles, so each frame's move is right only with both scaled.
        struct Site
        {
            const char *script; // its SCRIPT_NAME as the game stores it (lower case)
            uint32_t rel;
            const char *what;
        };
        constexpr Site kSites[] = {
            // m68 JDT6: loop +0x397E WAIT 0 .. +0x3ED1 GOTO; state 10 (G4053), after TIMERA > 1000: while G4054 > 4,
            // G4054 -= 0.05 and ROTATE_OBJECT G1243 / G1244 to 90 at step G4054 (from 12: 5.3 s on the PS2)
            {"jdt6", 0x3C0Du, "G4054 -= 0.05 (rotate step)"},
            // m112 DONH5: loop +0x3C23 (WAIT 0 at +0x3C30) .. +0x3CA3 GOTO: while L12 > 0.02, L12 -= 0.02 and
            // ROTATE_OBJECT G3982 / G3983 to 0 at step L12
            {"donh5", 0x3C6Fu, "L12 -= 0.02 (rotate step)"},
            // m113 DONH6: loop +0x1AFD (while L55 < 165; WAIT 0 at +0x1B0A) .. +0x1BB4 GOTO: SLIDE_OBJECT G4008 at steps
            // L46 / L47 = L46 x 0.35 / L48, the steps growing, SET_OBJECT_ROTATION (0x0661) G4008 L49 L50 L51, and the
            // pass count L55 (165 passes: 5.5 s on the PS2, 80 / 100 / 145 its stages)
            {"donh6", 0x1B4Au, "L46 += 0.02 (slide step)"},
            {"donh6", 0x1B6Au, "L48 += 0.005 (slide step)"},
            {"donh6", 0x1B85u, "L49 += 0.2 (angle)"},
            {"donh6", 0x1B8Du, "L50 += 0.2 (angle)"},
            {"donh6", 0x1BAFu, "L55 += 1 (pass count)"},
        };
        constexpr uint32_t kMissionBase = 0x003D9C24u; // gp+0x2D34: the IP a mission is loaded at (0x42946, the
                                                       // main part's size; LOAD_AND_LAUNCH 0x268144 starts it there)

        // ROTATE_OBJECT (0x0352: object, target heading, step in degrees, collision) and SLIDE_OBJECT (0x0353:
        // object, x, y, z, steps x / y / z, collision) move the object by the step on every call - a `WAIT 0` loop calls
        // them once a frame until they report the target reached: the steps are scaled by the frame's share of a PS2 frame.
        // Handlers 0x2265C8 / 0x2269FC in ProcessCommands 0x225570's table (CollectParameters 4 / 8, then the object).
        constexpr uint32_t kGetObject = 0x00112030u; // CPool<CObject>::GetAt(a0 pool, a1 handle)
        constexpr uint32_t kRotateGetAt = 0x002265F4u; // its return address in ROTATE_OBJECT (jal at 0x2265EC)
        constexpr uint32_t kSlideGetAt = 0x00226A2Cu;  // and in SLIDE_OBJECT (jal at 0x226A24)
        constexpr uint32_t kScriptParams = 0x003DF320u; // ScriptParams: CollectParameters' out (lui 0x3E; -0xCE0)

        PS2Runtime::RecompiledFunction g_orig = nullptr;
        uint64_t g_rotates = 0, g_slides = 0, g_scaled = 0; // survey log: scaled calls

        template <typename T>
        T rd(const uint8_t *ram, uint32_t a)
        {
            T v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
            return v;
        }
        template <typename T>
        void wr(uint8_t *ram, uint32_t a, T v)
        {
            std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T));
        }

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return !(v && v[0] == '0');
        }
        bool logOn()
        {
            static const bool on = std::getenv("LCS_SCRIPT_TIMING_LOG") != nullptr;
            return on;
        }

        // GetPointerToScriptVariable 0x1098D8; `ip` is ScriptSpace-relative and is moved past the argument
        uint32_t varAddress(const uint8_t *ram, uint32_t script, uint32_t &ip, std::string *label)
        {
            const uint32_t space = rd<uint32_t>(ram, kScriptSpace);
            const uint8_t t = rd<uint8_t>(ram, space + ip++);
            const uint32_t base = rd<uint32_t>(ram, script + kFrameBase);
            char buf[48];
            if (t < 0x0Cu)
            {
                std::snprintf(buf, sizeof(buf), "timer%u", t);
                if (label)
                    *label = buf;
                return script + kLocals + 4u * (t + 0x5Eu);
            }
            if (t < 0x6Cu)
            {
                std::snprintf(buf, sizeof(buf), "L%u", t - 0x0Cu);
                if (label)
                    *label = buf;
                return script + kLocals + 4u * (t - 0x0Cu + base);
            }
            if (t < 0xCCu) // a local array: index variable (a local), size
            {
                const uint8_t idx = rd<uint8_t>(ram, space + ip), size = rd<uint8_t>(ram, space + ip + 1);
                ip += 2;
                int32_t i = rd<int32_t>(ram, script + kLocals + 4u * (idx + base));
                if (i >= size)
                    i = size - 1;
                std::snprintf(buf, sizeof(buf), "L%u[L%u]", t - 0x6Cu, idx);
                if (label)
                    *label = buf;
                return script + kLocals + 4u * (t - 0x6Cu + base + i);
            }
            if (t < 0xE6u)
            {
                const uint32_t n = ((t - 0xCCu) << 8) | rd<uint8_t>(ram, space + ip++);
                std::snprintf(buf, sizeof(buf), "G%u", n);
                if (label)
                    *label = buf;
                return space + 4u * n;
            }
            const uint32_t lo = rd<uint8_t>(ram, space + ip), idx = rd<uint8_t>(ram, space + ip + 1),
                           size = rd<uint8_t>(ram, space + ip + 2);
            ip += 3;
            int32_t i = rd<int32_t>(ram, script + kLocals + 4u * (idx + base));
            if (i >= static_cast<int32_t>(size))
                i = static_cast<int32_t>(size) - 1;
            const uint32_t n = ((t - 0xE6u) << 8) + lo;
            std::snprintf(buf, sizeof(buf), "G%u[L%u]", n, idx);
            if (label)
                *label = buf;
            return space + 4u * (n + static_cast<uint32_t>(i));
        }

        // CollectParameters' decoding of one value (its bits)
        uint32_t constant(const uint8_t *ram, uint32_t script, uint32_t &ip, bool &isVar)
        {
            const uint32_t space = rd<uint32_t>(ram, kScriptSpace);
            const uint8_t t = rd<uint8_t>(ram, space + ip);
            isVar = false;
            switch (t)
            {
            case 1:
            case 2: ip += 1; return 0u;
            case 3: ip += 2; return static_cast<uint32_t>(rd<uint8_t>(ram, space + ip - 1)) << 24;
            case 4: ip += 3; return static_cast<uint32_t>(rd<uint16_t>(ram, space + ip - 2)) << 16;
            case 5:
                ip += 4;
                return (static_cast<uint32_t>(rd<uint8_t>(ram, space + ip - 3)) << 8) |
                       (static_cast<uint32_t>(rd<uint16_t>(ram, space + ip - 2)) << 16);
            case 6:
            case 9: ip += 5; return rd<uint32_t>(ram, space + ip - 4);
            case 7: ip += 2; return static_cast<uint32_t>(static_cast<int32_t>(rd<int8_t>(ram, space + ip - 1)));
            case 8: ip += 3; return static_cast<uint32_t>(static_cast<int32_t>(rd<int16_t>(ram, space + ip - 2)));
            default: isVar = true; return rd<uint32_t>(ram, varAddress(ram, script, ip, nullptr));
            }
        }

        std::string scriptName(const uint8_t *ram, uint32_t script)
        {
            char n[9] = {};
            std::memcpy(n, ram + ((script + kName) & 0x01FFFFFFu), 8);
            return n;
        }

        // ---- the survey -------------------------------------------------------------------------------------------
        struct Seen
        {
            uint64_t runs = 0;
            uint32_t lastFrame = ~0u, frames = 0;
            std::string text;
        };
        std::map<std::pair<std::string, uint32_t>, Seen> g_seen;
        uint32_t g_logStartMs = 0, g_logStartFrame = 0;

        void survey(const uint8_t *ram, uint32_t script, uint32_t opcode)
        {
            const uint32_t frame = rd<uint32_t>(ram, kFrameCounter), now = rd<uint32_t>(ram, kTimeMs);
            if (!g_logStartMs || now < g_logStartMs)
                g_logStartMs = now, g_logStartFrame = frame;
            const uint32_t ip0 = rd<uint32_t>(ram, script + kIp);
            Seen &s = g_seen[{scriptName(ram, script), ip0 - 2u}];
            ++s.runs;
            if (s.lastFrame != frame)
                s.lastFrame = frame, ++s.frames;
            if (s.text.empty())
            {
                uint32_t ip = ip0;
                std::string var;
                varAddress(ram, script, ip, &var);
                bool isVar = false;
                const uint32_t bits = constant(ram, script, ip, isVar);
                const bool isFloat = opcode & 1u;
                const bool sub = opcode >= 0x0Cu;
                char buf[96];
                float f;
                std::memcpy(&f, &bits, 4);
                if (isVar)
                    std::snprintf(buf, sizeof(buf), "%s %s= (a variable)", var.c_str(), sub ? "-" : "+");
                else if (isFloat)
                    std::snprintf(buf, sizeof(buf), "%s %s= %g", var.c_str(), sub ? "-" : "+", f);
                else
                    std::snprintf(buf, sizeof(buf), "%s %s= %d", var.c_str(), sub ? "-" : "+", static_cast<int32_t>(bits));
                s.text = buf;
            }
            if (now - g_logStartMs >= 20000u)
            {
                const uint32_t frames = frame - g_logStartFrame;
                std::fprintf(stderr, "[lcs-script-timing] %.1f game s, %u frames: adds that ran on most frames:\n",
                             (now - g_logStartMs) / 1000.0, frames);
                for (const auto &[key, v] : g_seen)
                    if (frames && v.frames >= frames * 6u / 10u)
                        std::fprintf(stderr, "[lcs-script-timing]   %s ip 0x%x (mission +0x%x): %s, %.2f a frame on %u%% of frames\n",
                                     key.first.c_str(), key.second, key.second - rd<uint32_t>(ram, kMissionBase),
                                     v.text.c_str(), static_cast<double>(v.runs) / frames, v.frames * 100u / frames);
                std::fprintf(stderr, "[lcs-script-timing]   scaled: %llu listed adds, %llu ROTATE_OBJECT, %llu SLIDE_OBJECT calls\n",
                             static_cast<unsigned long long>(g_scaled), static_cast<unsigned long long>(g_rotates),
                             static_cast<unsigned long long>(g_slides));
                g_scaled = g_rotates = g_slides = 0;
                g_seen.clear();
                g_logStartMs = now, g_logStartFrame = frame;
            }
        }

        // ---- the fix ----------------------------------------------------------------------------------------------
        float g_carry[sizeof(kSites) / sizeof(kSites[0])] = {};
        uint64_t g_siteRuns[sizeof(kSites) / sizeof(kSites[0])] = {}; // (a debugger's count)

        int listed(const uint8_t *ram, uint32_t script, uint32_t ip0)
        {
            const uint32_t rel = ip0 - rd<uint32_t>(ram, kMissionBase);
            std::string name;
            for (size_t i = 0; i < sizeof(kSites) / sizeof(kSites[0]); ++i)
                if (kSites[i].rel == rel)
                {
                    if (name.empty())
                    {
                        name = scriptName(ram, script);
                        for (char &c : name)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    if (name == kSites[i].script)
                        return static_cast<int>(i);
                }
            return -1;
        }

        bool active() { return vcs::timing() == vcs::Timing::Independent || vcs::frameCap() != 30; }

        float frameScale(const uint8_t *ram)
        {
            return std::clamp(rd<float>(ram, kTimeStep) / kPs2Step, 0.0f, 2.0f);
        }

        // ---- ROTATE_OBJECT / SLIDE_OBJECT steps -------------------------------------------------------------------
        // Both handlers read their arguments into ScriptParams (CollectParameters) and then look the object up
        // (CPool<CObject>::GetAt 0x112030, from the pool *0x3D5AF8) before they use them: the hook on GetAt scales the
        // steps in ScriptParams when it is called from those two places.
        PS2Runtime::RecompiledFunction g_origGetAt = nullptr;

        PS2_GUEST_CC void objectGetAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool on = envOn("LCS_SCRIPT_TIMING");
            const uint32_t ra = GPR_U32(ctx, 31);
            if ((ra == kRotateGetAt || ra == kSlideGetAt) && on && active())
            {
                const float scale = frameScale(rdram);
                if (scale < 1.0f)
                {
                    auto scaleParam = [&](uint32_t i) {
                        wr<float>(rdram, kScriptParams + 4u * i, rd<float>(rdram, kScriptParams + 4u * i) * scale);
                    };
                    if (ra == kRotateGetAt)
                        scaleParam(2), ++g_rotates;
                    else
                        scaleParam(4), scaleParam(5), scaleParam(6), ++g_slides;
                }
            }
            g_origGetAt(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void commands0To99(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool on = envOn("LCS_SCRIPT_TIMING");
            const uint32_t script = GPR_U32(ctx, 4), opcode = GPR_U32(ctx, 5);
            if (opcode >= 0x08u && opcode <= 0x0Fu)
            {
                if (logOn())
                    survey(rdram, script, opcode);
                const uint32_t ip0 = rd<uint32_t>(rdram, script + kIp);
                const int site = on && active() ? listed(rdram, script, ip0 - 2u) : -1;
                if (site >= 0)
                {
                    const float scale = frameScale(rdram);
                    ++g_siteRuns[site], ++g_scaled;
                    uint32_t ip = ip0;
                    const uint32_t var = varAddress(rdram, script, ip, nullptr);
                    bool isVar = false;
                    const uint32_t bits = constant(rdram, script, ip, isVar);
                    const bool sub = opcode >= 0x0Cu;
                    if (opcode & 1u)
                    {
                        float c;
                        std::memcpy(&c, &bits, 4);
                        const float v = rd<float>(rdram, var);
                        wr<float>(rdram, var, sub ? v - c * scale : v + c * scale);
                    }
                    else
                    {
                        float &carry = g_carry[site];
                        const float want = static_cast<float>(static_cast<int32_t>(bits)) * scale + carry;
                        const float whole = std::trunc(want);
                        carry = want - whole;
                        const int32_t v = rd<int32_t>(rdram, var), d = static_cast<int32_t>(whole);
                        wr<int32_t>(rdram, var, sub ? v - d : v + d);
                    }
                    wr<uint32_t>(rdram, script + kIp, ip);
                    SET_GPR_U32(ctx, 2, 0u); // the handler's "go on"
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
            }
            g_orig(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!envOn("LCS_SCRIPT_TIMING") && !logOn())
        {
            std::fprintf(stderr, "[lcs-script-timing] off (LCS_SCRIPT_TIMING=0): mission meters once a frame\n");
            return;
        }
        g_orig = runtime.lookupFunction(kCommands0To99);
        if (!g_orig)
        {
            game::logUnavailable("mission meters on the time step (lcs_script_timing: no ProcessCommands0To99)");
            return;
        }
        runtime.replaceFunction(kCommands0To99, &commands0To99);
        if ((g_origGetAt = runtime.lookupFunction(kGetObject)))
            runtime.replaceFunction(kGetObject, &objectGetAt);
        else
            game::logUnavailable("object rotate / slide steps on the time step (lcs_script_timing: no CPool<CObject>::GetAt)");
        std::fprintf(stderr, "[lcs-script-timing] %zu meter sites, ROTATE_OBJECT / SLIDE_OBJECT steps%s\n",
                     sizeof(kSites) / sizeof(kSites[0]), logOn() ? " (survey log on)" : "");
    }
}
