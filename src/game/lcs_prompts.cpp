// [lcs] See lcs_prompts.h. How LCS shows a prompt:
//
// 1. Help text names controls by action: `~w~Press~h~ ~k~ ~VEEE~ ~w~to enter or exit a vehicle.` The message code
//    (0x2F91A0, called by the help box, the mission text, the pager...) expands each `~k~` in place: it calls the key
//    expander 0x2F8F58 (src after "~k~ ", out), which reads the controller configuration (CPad 0 +0x7E: the pads are
//    0x408C60 + n x 0xBC, FUN_00170be8), looks the GXT key `C<configuration><ACTION>` up in TheText (CText* 0x3D89B8)
//    and copies its text to out, turning ~T~ ~S~ ~X~ ~O~ into the font's characters 0xE0 0xE1 0xE2 0xE3 (the PS2's
//    face-button symbols), ~L~ ~N~ ~R~ into 'M' 'O' 'S' and dropping any other ~?~. It returns 1 + the action's
//    length; the caller skips the action and its closing '~'. The text is words for everything but the face buttons
//    ("R3 button", "left analog stick, or left and right directional buttons", "L2 button + R2 button").
// 2. The font is GTA III's CFont (PrintString 0x1658F8): the expanded string is measured, wrapped and centred like any
//    other text, character by character.
//
// So the simplest robust port is VCS's expander without its glyphs: with Button prompts other than Original the
// expander is replaced, reads the same GXT text (host-side lookup, CText's layout below), turns each phrase into the
// control it names (the same phrase list as vcs_prompts.cpp) and writes that control's name for the device in use:
// the bound keys ("F", "W/S", "Left mouse"), an Xbox pad's buttons ("A", "LB", "Left stick"), a PlayStation pad's (its
// face buttons as the font's own symbols, the rest "L1", "R3", "Left stick"). The font then lays it out as text, so
// wrapping, centring and the widescreen HUD placement are the game's own. The name is chosen when the text is
// expanded (the help box keeps it while it is up), not each frame as VCS's glyphs are.
// Original, no text table yet, or a call from outside the message expansion: the game's own expander runs.
//
// The buffer (why the names are bounded). The expander's only caller is the message expansion 0x2F91A0 (msg), which
// never checks a length: it builds the result in a 256-character stack buffer W (sp+0x40), expanding each `~k~` into
// a second one O (sp+0x240, zeroed before each call, passed as `out`), appends O to W, copies the rest of the message
// to W as is, then writes W back over msg (and zeroes msg up to 256 characters: every caller's buffer is >= 256). At
// the call (return address 0x2F9270) its registers say exactly where it is: s0 = characters already in W, s1 = the
// source index the call reads from (a0 = s2 + 2 x s1), s2 = msg, s3 = W, s4 = s5 = O = W + 0x200, s6 = msg's length
// when it started (the loop runs while the source index < s6). After the call the source index becomes s1 + ret + 1,
// so the message still to come is rem = s6 - (s1 + ret + 1) characters, each copied one for one (a later `~k~` is
// bounded again by its own call). The expansion n therefore keeps s0 + n + rem + 1 (the terminator) <= 256: the full
// name if it fits, else the first binding of each control only, else the game's own result (the PS2 glyphs / words),
// cut to what fits. This holds by induction over the message: s0 + (s6 - index) + 1 <= 256 before the first `~k~`
// for any message the game itself would not overflow, and every expansion keeps it. A call from anywhere else (no
// such frame) runs the game's code. A GXT key that is not found makes the game write nothing (its lookup returns
// an empty string, 0x332058 with format "" at 0x3D89C0); so does this.
#include "game/lcs_prompts.h"

#include "game/vcs_input.h"
#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace lcs::prompts
{
    namespace
    {
        struct Addrs
        {
            // the key expander (src, out) -> 1 + action length: FUN_002f8f58, called by the in-place `~k~` expansion
            // FUN_002f91a0 (`~` `k` `~` test, then `uVar8 += 4`, the call, `uVar8 += ret + 1`); builds 'C' + '0'..'9'
            // (switch on CPad+0x7E) + the action, looks it up through FUN_00332e68 and maps ~T~ ~S~ ~X~ ~O~ ~L~ ~N~ ~R~
            uint32_t expandKey = 0x002F8F58u;
            // TheText: CText* made on first use by every reader (`if (DAT_003d89b8 == 0) new(0x994) CText`, e.g.
            // FUN_002f8f58, FUN_001f5270); its lookup FUN_00332e68 -> FUN_00332058 -> FUN_00332100: binary search over
            // {u32 entries, u16 count} at +0, entries 12 bytes {u32 text, char key[8]} compared with strcmp; the mission
            // table {entries, count} at +0x10 when +0x21 and +0x22 are set
            uint32_t theText = 0x003D89B8u;
            // CPad 0: FUN_00170be8(n) = 0x408C60 + n x 0xBC; the expander's configuration digit is its u16 at +0x7E
            uint32_t pad0Mode = 0x00408C60u + 0x7Eu;
            // the message expansion's call of the expander (FUN_002f91a0 at 0x2F9268, delay slot 0x2F926C): its return
            // address, the only caller in the game
            uint32_t expandReturn = 0x002F9270u;
        };
        constexpr Addrs kLcs{};

        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        // the message expansion's buffers: W (result) and O (one expansion), u16 characters each, O = W + 0x200
        constexpr uint32_t kMsgChars = 256;
        // the font's face-button characters (the expander's own mapping, FUN_002f8f58's switch)
        constexpr uint16_t kGlyphTriangle = 0xE0, kGlyphSquare = 0xE1, kGlyphCross = 0xE2, kGlyphCircle = 0xE3;

        enum Ctl : int
        {
            Cross, Circle, Triangle, Square, L1, R1, L2, R2, L3, R3, Select, Start,
            DUp, DDown, DLeft, DRight, DPad, DPadUD, DPadLR,
            LS, LSUp, LSDown, LSLeft, LSRight, LSUD, LSLR,
            RS, RSUp, RSDown, RSLeft, RSRight, RSUD, RSLR,
            kCtlCount
        };
        enum Ctx : int
        {
            Foot,
            Vehicle,
        };
        constexpr const char *kCtlNames[kCtlCount] = {
            "X", "O", "T", "S", "L1", "R1", "L2", "R2", "L3", "R3", "SELECT", "START",
            "DU", "DD", "DL", "DR", "DPAD", "DPADUD", "DPADLR",
            "LS", "LSU", "LSD", "LSL", "LSR", "LSUD", "LSLR",
            "RS", "RSU", "RSD", "RSL", "RSR", "RSUD", "RSLR"};
        // the bindings' ids (vcs_input.cpp kControls) of the button controls, and their PS2 default on a gamepad
        constexpr const char *kButtonIds[DPad] = {"cross", "circle", "triangle", "square", "l1", "r1", "l2", "r2",
                                                  "l3", "r3", "select", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right"};
        constexpr const char *kPadDefault[DPad] = {"south", "east", "north", "west", "lb", "rb", "lt", "rt",
                                                   "ls", "rs", "back", "start", "dpad_u", "dpad_d", "dpad_l", "dpad_r"};

        // the setting's values (vcs_prompts.cpp registerSettings)
        enum Set : int
        {
            Original = 0,
            Automatic = 1,
            PlayStation = 2,
            Xbox = 3,
            Keyboard = 4,
        };

        uint8_t *g_rdram = nullptr;
        bool g_installed = false;
        PS2Runtime::RecompiledFunction g_origExpand = nullptr;
        std::atomic<uint32_t> g_expanded{0}, g_passed{0};
        std::atomic<int> g_lastSet{-1};
        // the shorter form (each control's first binding only), while it is being made: the emulation thread's
        thread_local bool t_firstOnly = false;
        // tests: each control's name repeated this many times (`lcsprompts stress N`), to drive the bound
        std::atomic<int> g_stress{1};

        // the bound's bookkeeping (`lcsprompts last`)
        enum Form : int
        {
            FormFull,
            FormFirst,
            FormGame,
            FormCut,
        };
        struct Last
        {
            uint32_t msg = 0, w = 0, used = 0, rem = 0, budget = 0, wanted = 0, written = 0;
            int form = -1;
            std::string action;
        };
        std::mutex g_lastMutex;
        Last g_last;
        std::atomic<uint32_t> g_forms[4]{}, g_foreign{0}, g_maxTotal{0};

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 2);
            return v;
        }
        void wr16(uint32_t a, uint16_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 2); }
        bool inRam(uint32_t a, uint32_t n = 4) { return a >= 0x00100000u && a + n <= 0x02000000u; }

        // ---- names: what each control is called on each device --------------------------------------------------------
        int setting(const std::string &key) { return settings::value(key); }

        int boundKey(const char *id, bool preferMouse)
        {
            const int a = setting(std::string("keys.") + id), b = setting(std::string("keys.") + id + "2");
            if (preferMouse && b >= platform::KEY_MOUSE_LEFT)
                return b;
            return a ? a : b;
        }

        std::string keysName(std::initializer_list<int> keys)
        {
            std::string s;
            for (int k : keys)
                if (k > 0 && !(t_firstOnly && !s.empty()))
                {
                    const char *n = platform::keyName(k);
                    if (!s.empty())
                        s += "/";
                    s += n;
                }
            return s.empty() ? "(unbound)" : s;
        }

        std::string keyboardName(int ctx, int ctl)
        {
            auto k = [](const char *id) { return boundKey(id, false); };
            if (ctl < DPad)
            {
                if (ctx == Vehicle)
                {
                    const char *drive = ctl == Cross ? "drive_accelerate" : ctl == Square ? "drive_brake" : ctl == R1 ? "drive_handbrake" : nullptr;
                    if (drive)
                        if (const int key = boundKey(drive, false))
                            return keysName({key});
                }
                // the mouse where it is bound (attack, target): how a PC player knows those
                return keysName({boundKey(kButtonIds[ctl], ctl == Circle || ctl == R1)});
            }
            switch (ctl)
            {
            case DPad: return keysName({k("dpad_up"), k("dpad_left"), k("dpad_down"), k("dpad_right")});
            case DPadUD: return keysName({k("dpad_up"), k("dpad_down")});
            case DPadLR: return keysName({k("dpad_left"), k("dpad_right")});
            default: break;
            }
            if (ctl >= RS)
            {
                if (setting("mouse.camera"))
                    return "the mouse";
                switch (ctl)
                {
                case RSUp: return keysName({k("cam_up")});
                case RSDown: return keysName({k("cam_down")});
                case RSLeft: return keysName({k("cam_left")});
                case RSRight: return keysName({k("cam_right")});
                case RSUD: return keysName({k("cam_up"), k("cam_down")});
                case RSLR: return keysName({k("cam_left"), k("cam_right")});
                default: return keysName({k("cam_up"), k("cam_left"), k("cam_down"), k("cam_right")});
                }
            }
            if (ctx == Vehicle) // the left stick steers (left / right) and leans (up / down: the Driving keys)
                switch (ctl)
                {
                case LSUp: return keysName({k("drive_lean_forward")});
                case LSDown: return keysName({k("drive_lean_back")});
                case LSUD: return keysName({k("drive_lean_forward"), k("drive_lean_back")});
                case LS:
                case LSLR: return keysName({k("move_left"), k("move_right")});
                default: break;
                }
            switch (ctl)
            {
            case LSUp: return keysName({k("move_up")});
            case LSDown: return keysName({k("move_down")});
            case LSLeft: return keysName({k("move_left")});
            case LSRight: return keysName({k("move_right")});
            case LSUD: return keysName({k("move_up"), k("move_down")});
            case LSLR: return keysName({k("move_left"), k("move_right")});
            default: return keysName({k("move_up"), k("move_left"), k("move_down"), k("move_right")});
            }
        }

        const char *padSourceName(int source) // the Gamepad page's sources (vcs_input.cpp)
        {
            switch (source)
            {
            case 0: return "south";
            case 1: return "east";
            case 2: return "west";
            case 3: return "north";
            case 4: return "back";
            case 5: return "start";
            case 6: return "ls";
            case 7: return "rs";
            case 8: return "lb";
            case 9: return "rb";
            case 10: return "dpad_u";
            case 11: return "dpad_d";
            case 12: return "dpad_l";
            case 13: return "dpad_r";
            case 20: return "lt";
            case 21: return "rt";
            default: return nullptr;
            }
        }

        // a physical pad button's name: PlayStation's face buttons are the font's symbols (one character)
        std::u16string padButtonName(bool ps, const std::string &b)
        {
            auto s = [](const char *t) { return std::u16string(t, t + std::strlen(t)); };
            if (ps)
            {
                if (b == "south") return std::u16string(1, char16_t(kGlyphCross));
                if (b == "east") return std::u16string(1, char16_t(kGlyphCircle));
                if (b == "north") return std::u16string(1, char16_t(kGlyphTriangle));
                if (b == "west") return std::u16string(1, char16_t(kGlyphSquare));
                if (b == "lb") return s("L1");
                if (b == "rb") return s("R1");
                if (b == "lt") return s("L2");
                if (b == "rt") return s("R2");
                if (b == "ls") return s("L3");
                if (b == "rs") return s("R3");
                if (b == "back") return s("SELECT");
                if (b == "start") return s("START");
            }
            else
            {
                if (b == "south") return s("A");
                if (b == "east") return s("B");
                if (b == "north") return s("Y");
                if (b == "west") return s("X");
                if (b == "lb") return s("LB");
                if (b == "rb") return s("RB");
                if (b == "lt") return s("LT");
                if (b == "rt") return s("RT");
                if (b == "ls") return s("LS");
                if (b == "rs") return s("RS");
                if (b == "back") return s("Back");
                if (b == "start") return s("Start");
            }
            if (b == "dpad_u") return s("D-pad up");
            if (b == "dpad_d") return s("D-pad down");
            if (b == "dpad_l") return s("D-pad left");
            if (b == "dpad_r") return s("D-pad right");
            return s("?");
        }

        std::u16string padName(bool ps, int ctl)
        {
            auto s = [](const std::string &t) { return std::u16string(t.begin(), t.end()); };
            if (ctl < DPad) // the physical button the player bound (Gamepad page)
            {
                const char *bound = padSourceName(setting(std::string("pad.") + kButtonIds[ctl]));
                return padButtonName(ps, bound ? bound : kPadDefault[ctl]);
            }
            switch (ctl)
            {
            case DPad: return s("D-pad");
            case DPadUD: return s("D-pad up/down");
            case DPadLR: return s("D-pad left/right");
            default: break;
            }
            bool left = ctl < RS;
            if (setting("pad.swap_sticks"))
                left = !left;
            const int d = ctl - (ctl < RS ? LS : RS);
            static const char *const suffix[] = {"", " up", " down", " left", " right", " up/down", " left/right"};
            return s(std::string(left ? "Left stick" : "Right stick") + suffix[d]);
        }

        // the set in use: Automatic follows the device used last; a gamepad shows its maker's buttons (Xbox for others)
        int effectiveSet(int s)
        {
            if (s != Automatic)
                return s;
            if (vcs::input::lastDevice() == vcs::input::Device::Keyboard)
                return Keyboard;
            return platform::gamepadFamily() == platform::GamepadFamily::PlayStation ? PlayStation : Xbox;
        }

        std::u16string controlName1(int set, int ctx, int ctl)
        {
            if (set == PlayStation || set == Xbox)
                return padName(set == PlayStation, ctl);
            const std::string n = keyboardName(ctx, ctl);
            return std::u16string(n.begin(), n.end());
        }

        std::u16string controlName(int set, int ctx, int ctl)
        {
            const std::u16string one = controlName1(set, ctx, ctl);
            std::u16string s = one;
            for (int i = 1; i < std::clamp(g_stress.load(std::memory_order_relaxed), 1, 64); ++i)
                s += u" " + one;
            return s;
        }

        // ---- the GXT phrases: which control the text names (vcs_prompts.cpp's list) -----------------------------------
        struct Phrase
        {
            std::string pattern, replacement;
            bool exact; // case matters (the ~X~ tokens; ~t~ is a colour)
        };
        const std::vector<Phrase> &phrases()
        {
            static const std::vector<Phrase> p = [] {
                std::vector<Phrase> v = {
                    {"left analog stick, or left and right directional buttons", "{LSLR} or {DPADLR}", false},
                    {"left analog stick up and down, or up and down directional buttons", "{LSUD} or {DPADUD}", false},
                    {"left analog stick, or directional buttons", "{LS} or {DPAD}", false},
                    {"left analog stick left and right", "{LSLR}", false},
                    {"left analog stick up and down", "{LSUD}", false},
                    {"left analog stick up", "{LSU}", false},
                    {"left analog stick down", "{LSD}", false},
                    {"left analog stick", "{LS}", false},
                    {"right analog stick", "{RS}", false},
                    {"analog stick up and analog stick down", "{LSU} and {LSD}", false},
                    {"analog stick left and analog stick right", "{LSL} and {LSR}", false},
                    {"analog stick up", "{LSU}", false},
                    {"analog stick down", "{LSD}", false},
                    {"analog stick left", "{LSL}", false},
                    {"analog stick right", "{LSR}", false},
                    {"analog stick", "{LS}", false},
                    {"up and down directional buttons", "{DPADUD}", false},
                    {"up directional button", "{DU}", false},
                    {"down directional button", "{DD}", false},
                    {"left directional button", "{DL}", false},
                    {"right directional button", "{DR}", false},
                    {"left and right directional buttons", "{DPADLR}", false},
                    {"directional buttons", "{DPAD}", false},
                    {"up button and down button", "{DU} and {DD}", false},
                    {"left button and right button", "{DL} and {DR}", false},
                    {"the down button", "{DD}", false},
                    {"up button", "{DU}", false},
                    {"down button", "{DD}", false},
                    {"left button", "{DL}", false},
                    {"right button", "{DR}", false},
                    {"L1 and R1 buttons", "{L1} and {R1}", false},
                    {"L2 and R2 buttons", "{L2} and {R2}", false},
                    {"L2 button and R2 button", "{L2} and {R2}", false},
                    {"L2 button + R2 button", "{L2} + {R2}", false},
                    {"L1 button", "{L1}", false},
                    {"R1 button", "{R1}", false},
                    {"L2 button", "{L2}", false},
                    {"R2 button", "{R2}", false},
                    {"L3 button", "{L3}", false},
                    {"R3 button", "{R3}", false},
                    {"SELECT button", "{SELECT}", false},
                    {"START button", "{START}", false},
                    // the PSP's names left in the PS2's text (L / R: the PS2's L1 / R1)
                    {"L button", "{L1}", false},
                    {"R button", "{R1}", false},
                    {"~X~", "{X}", true},
                    {"~O~", "{O}", true},
                    {"~T~", "{T}", true},
                    {"~S~", "{S}", true},
                };
                std::stable_sort(v.begin(), v.end(), [](const Phrase &a, const Phrase &b) { return a.pattern.size() > b.pattern.size(); });
                return v;
            }();
            return p;
        }

        int ctlByName(const std::string &name)
        {
            for (int i = 0; i < kCtlCount; ++i)
                if (name == kCtlNames[i])
                    return i;
            return -1;
        }

        // the action's context: driving (VE*: LCS's vehicle actions VEACC, VEBRK, VEEE, VESTR...), the camera view, the
        // fire truck's hose
        int actionContext(const std::string &action)
        {
            if (action.rfind("VE", 0) == 0 || action.rfind("FL", 0) == 0 || action.rfind("HE", 0) == 0 || action == "CVEIW" ||
                action == "FIREH")
                return Vehicle;
            return Foot;
        }

        // the GXT text with each phrase replaced by the control's name; other ~?~ tokens as the game's expander does
        // them (~L~ ~N~ ~R~ -> 'M' 'O' 'S', the rest dropped)
        std::u16string translate(const std::u16string &text, int set, int ctx)
        {
            std::u16string out;
            auto lower = [](char16_t c) { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + 32) : c; };
            auto letter = [](char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); };
            for (size_t i = 0; i < text.size();)
            {
                const Phrase *hit = nullptr;
                const bool wordStart = i == 0 || !letter(text[i - 1]); // "pickup button" is no "up button"
                for (const Phrase &p : phrases())
                {
                    if (i + p.pattern.size() > text.size() || (!p.exact && !wordStart))
                        continue;
                    bool same = true;
                    for (size_t k = 0; k < p.pattern.size() && same; ++k)
                    {
                        const char16_t a = text[i + k], b = static_cast<char16_t>(p.pattern[k]);
                        same = p.exact ? a == b : lower(a) == lower(b);
                    }
                    if (same)
                    {
                        hit = &p;
                        break;
                    }
                }
                if (hit)
                {
                    i += hit->pattern.size();
                    const std::string &r = hit->replacement;
                    for (size_t k = 0; k < r.size();)
                    {
                        if (r[k] == '{')
                        {
                            const size_t end = r.find('}', k);
                            const int ctl = ctlByName(r.substr(k + 1, end - k - 1));
                            out += controlName(set, ctx, ctl < 0 ? Cross : ctl);
                            k = end + 1;
                        }
                        else
                            out.push_back(static_cast<char16_t>(r[k++]));
                    }
                    continue;
                }
                if (text[i] == u'~' && i + 2 < text.size() && text[i + 2] == u'~')
                {
                    const char16_t t = text[i + 1];
                    if (t == u'L')
                        out.push_back(u'M');
                    else if (t == u'N')
                        out.push_back(u'O');
                    else if (t == u'R')
                        out.push_back(u'S');
                    i += 3;
                    continue;
                }
                out.push_back(text[i++]);
            }
            return out;
        }

        // ---- TheText ------------------------------------------------------------------------------------------------
        uint32_t lookupIn(uint32_t entries, uint32_t count, const char *want)
        {
            if (!inRam(entries) || count == 0 || count > 30000 || !inRam(entries, count * 12u))
                return 0;
            int lo = 0, hi = static_cast<int>(count) - 1;
            while (lo <= hi)
            {
                const int mid = (lo + hi) / 2;
                const int c = std::strncmp(want, reinterpret_cast<const char *>(g_rdram + entries + mid * 12u + 4u), 8);
                if (c == 0)
                    return rd32(entries + mid * 12u);
                if (c < 0)
                    hi = mid - 1;
                else
                    lo = mid + 1;
            }
            return 0;
        }

        // the text of a GXT key ("" when TheText has none), as FUN_00332e68: the main table, then the mission's
        std::u16string gameText(const std::string &key)
        {
            const uint32_t text = rd32(kLcs.theText);
            if (!inRam(text, 0x30))
                return {};
            char want[9] = {};
            std::memcpy(want, key.data(), std::min<size_t>(key.size(), 8));
            uint32_t p = lookupIn(rd32(text), rd16(text + 4), want);
            if (!p && g_rdram[(text + 0x21) & kRamMask] && g_rdram[(text + 0x22) & kRamMask])
                p = lookupIn(rd32(text + 0x10), rd16(text + 0x14), want);
            std::u16string s;
            for (; p && inRam(p, 2) && s.size() < 255; p += 2)
            {
                const uint16_t ch = rd16(p);
                if (!ch)
                    break;
                s.push_back(static_cast<char16_t>(ch));
            }
            return s;
        }

        // the action after "~k~ " at p ("~ACTION~"): its name, and where its opening '~' was found (0: none)
        std::string actionAt(uint32_t p)
        {
            for (int n = 0; n < 64 && inRam(p, 2) && rd16(p) != '~'; ++n)
                p += 2;
            std::string action;
            if (inRam(p, 2) && rd16(p) == '~')
                for (p += 2; inRam(p, 2) && rd16(p) != '~' && action.size() < 16; p += 2)
                    action.push_back(static_cast<char>(rd16(p)));
            return action;
        }

        // the GXT text the game's expander would look up for the action ("" when none: it then writes nothing)
        std::u16string keyText(const std::string &action)
        {
            const uint16_t mode = rd16(kLcs.pad0Mode);
            if (mode > 9 || action.empty())
                return {};
            return gameText("C" + std::to_string(mode) + action);
        }

        std::u16string expandFor(const std::string &action, int set)
        {
            const std::u16string text = keyText(action);
            return text.empty() ? text : translate(text, set, actionContext(action));
        }

        // what the game's own expander writes for the text (FUN_002f8f58's loop): ~T~ ~S~ ~X~ ~O~ -> the font's symbols,
        // ~L~ ~N~ ~R~ -> 'M' 'O' 'S', any other ~?~ dropped
        std::u16string gameExpansion(const std::u16string &text)
        {
            std::u16string out;
            for (size_t i = 0; i < text.size();)
            {
                if (text[i] != u'~')
                {
                    out.push_back(text[i++]);
                    continue;
                }
                switch (i + 1 < text.size() ? text[i + 1] : 0)
                {
                case u'L': out.push_back(u'M'); break;
                case u'N': out.push_back(u'O'); break;
                case u'O': out.push_back(char16_t(kGlyphCircle)); break;
                case u'R': out.push_back(u'S'); break;
                case u'S': out.push_back(char16_t(kGlyphSquare)); break;
                case u'T': out.push_back(char16_t(kGlyphTriangle)); break;
                case u'X': out.push_back(char16_t(kGlyphCross)); break;
                default: break;
                }
                i += 3;
            }
            return out;
        }

        // the message expansion's frame at this call (see the top): how many characters the expansion may write so
        // that W never overflows. False when the caller is not that frame (then the game's code runs).
        bool expansionRoom(R5900Context *ctx, uint32_t src, uint32_t out, uint32_t ret, Last &l)
        {
            const uint32_t s0 = GPR_U32(ctx, 16), s1 = GPR_U32(ctx, 17), s2 = GPR_U32(ctx, 18) & kRamMask,
                           s3 = GPR_U32(ctx, 19) & kRamMask, s4 = GPR_U32(ctx, 20) & kRamMask, s6 = GPR_U32(ctx, 22);
            if (GPR_U32(ctx, 31) != kLcs.expandReturn || s4 != out || s3 + kMsgChars * 2u != out || s1 > 0xFFFFu ||
                s2 + s1 * 2u != src || s0 >= kMsgChars || s6 > 0xFFFFu || !inRam(s3, kMsgChars * 4u) ||
                !inRam(s2, kMsgChars * 2u))
                return false;
            l.msg = s2;
            l.w = s3;
            l.used = s0;
            l.rem = s6 > s1 + ret + 1 ? s6 - (s1 + ret + 1) : 0; // the message after the action's closing '~'
            const uint32_t taken = s0 + l.rem + 1;                  // + the terminator
            l.budget = taken < kMsgChars ? kMsgChars - taken : 0;
            return true;
        }

        // FUN_002f8f58(src, out) -> 1 + the action's length. Replaced while a set other than Original is chosen; the
        // game's own code otherwise, and when called from anywhere but the message expansion (no bound known).
        PS2_GUEST_CC void expandKey(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int chosen = settings::value("enhance.prompts");
            const uint32_t src = GPR_U32(ctx, 4) & kRamMask, out = GPR_U32(ctx, 5) & kRamMask;
            Last l;
            l.action = chosen == Original ? std::string() : actionAt(src);
            const uint32_t ret = static_cast<uint32_t>(1 + l.action.size());
            // no text table yet: the game makes it (empty tables: it writes nothing either)
            if (chosen == Original || rd32(kLcs.theText) == 0 || !expansionRoom(ctx, src, out, ret, l))
            {
                if (chosen != Original)
                    g_foreign.fetch_add(1, std::memory_order_relaxed);
                g_passed.fetch_add(1, std::memory_order_relaxed);
                return g_origExpand(rdram, ctx, runtime);
            }
            const int set = effectiveSet(chosen);
            const std::u16string text = keyText(l.action);
            const int vctx = actionContext(l.action);
            // the full name, else each control's first binding, else the game's own words / glyphs, cut to fit
            std::u16string t = text.empty() ? text : translate(text, set, vctx);
            l.wanted = static_cast<uint32_t>(t.size());
            l.form = FormFull;
            if (t.size() > l.budget)
            {
                t_firstOnly = true;
                t = translate(text, set, vctx);
                t_firstOnly = false;
                l.form = FormFirst;
            }
            if (t.size() > l.budget)
            {
                t = gameExpansion(text);
                l.form = FormGame;
            }
            if (t.size() > l.budget)
            {
                t.resize(l.budget);
                l.form = FormCut;
            }
            for (size_t i = 0; i < t.size(); ++i)
                wr16(out + static_cast<uint32_t>(i) * 2u, static_cast<uint16_t>(t[i]));
            wr16(out + static_cast<uint32_t>(t.size()) * 2u, 0);
            l.written = static_cast<uint32_t>(t.size());
            const uint32_t total = l.used + l.written + l.rem + 1;
            for (uint32_t m = g_maxTotal.load(); total > m && !g_maxTotal.compare_exchange_weak(m, total);)
            {
            }
            g_forms[l.form].fetch_add(1, std::memory_order_relaxed);
            g_expanded.fetch_add(1, std::memory_order_relaxed);
            g_lastSet.store(set, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(g_lastMutex);
                g_last = l;
            }
            SET_GPR_U32(ctx, 2, ret);
            ctx->pc = GPR_U32(ctx, 31);
        }

        std::string narrow(const std::u16string &s)
        {
            std::string o;
            for (char16_t c : s)
            {
                if (c >= 0x20 && c < 0x7F)
                    o.push_back(static_cast<char>(c));
                else
                {
                    char b[8];
                    std::snprintf(b, sizeof(b), "<%02X>", static_cast<unsigned>(c));
                    o += b;
                }
            }
            return o;
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("VCS_PROMPTS_HOOKS"); e && e[0] == '0')
            return;
        g_rdram = runtime.memory().getRDRAM();
        g_origExpand = runtime.lookupFunction(kLcs.expandKey);
        if (!g_origExpand)
        {
            std::fprintf(stderr, "[lcs-prompts] key expander 0x%x not found: Button prompts does nothing\n", kLcs.expandKey);
            return;
        }
        runtime.replaceFunction(kLcs.expandKey, &expandKey);
        g_installed = true;
        std::fprintf(stderr, "[lcs-prompts] button prompts: the key expander writes the player's controls\n");

        // lcsprompts -> ok setting set mode expanded passed | lcsprompts text <KEY> -> the GXT text (raw) |
        // lcsprompts expand <ACTION> [set] -> what the expander would write now (set: 1 auto 2 ps 3 xbox 4 keyboard) |
        // lcsprompts last -> the last bounded expansion and its message now | lcsprompts stress <N> -> tests: each
        // control's name repeated N times (1 = off), to drive the bound
        ps2control::addCommand("lcsprompts", [](const std::string &args, ps2control::Reply reply) {
            char verb[16] = {}, arg[32] = {};
            int set = -1;
            const int n = std::sscanf(args.c_str(), " %15s %31s %d", verb, arg, &set);
            if (n >= 2 && std::strcmp(verb, "text") == 0)
                return reply("ok " + narrow(gameText(arg)));
            if (n >= 2 && std::strcmp(verb, "expand") == 0)
            {
                const int s = effectiveSet(set >= 1 ? set : std::max(1, settings::value("enhance.prompts")));
                return reply("ok set=" + std::to_string(s) + " " + narrow(expandFor(arg, s)));
            }
            if (n >= 2 && std::strcmp(verb, "stress") == 0)
            {
                g_stress.store(std::clamp(std::atoi(arg), 1, 64));
                return reply("ok stress=" + std::to_string(g_stress.load()));
            }
            if (n >= 1 && std::strcmp(verb, "last") == 0)
            {
                // the last bounded expansion, and its message as it is now (expanded; its length <= 255)
                Last l;
                {
                    std::lock_guard<std::mutex> lock(g_lastMutex);
                    l = g_last;
                }
                if (!l.msg)
                    return reply("ok none");
                std::u16string m;
                uint32_t len = 0;
                while (len < kMsgChars + 32 && rd16(l.msg + len * 2u))
                    m.push_back(static_cast<char16_t>(rd16(l.msg + 2u * len++)));
                char text[320];
                std::snprintf(text, sizeof(text),
                              "ok action=%s msg=0x%x w=0x%x used=%u rem=%u budget=%u wanted=%u written=%u form=%d total=%u "
                              "msglen=%u | ",
                              l.action.c_str(), l.msg, l.w, l.used, l.rem, l.budget, l.wanted, l.written, l.form,
                              l.used + l.written + l.rem + 1, len);
                return reply(text + narrow(m));
            }
            char text[320];
            std::snprintf(text, sizeof(text),
                          "ok installed=%d setting=%d set=%d mode=%u expanded=%u passed=%u foreign=%u full=%u first=%u "
                          "game=%u cut=%u max_total=%u stress=%d",
                          g_installed ? 1 : 0, settings::value("enhance.prompts"),
                          effectiveSet(std::max(1, settings::value("enhance.prompts"))), rd16(kLcs.pad0Mode),
                          g_expanded.load(), g_passed.load(), g_foreign.load(), g_forms[FormFull].load(),
                          g_forms[FormFirst].load(), g_forms[FormGame].load(), g_forms[FormCut].load(), g_maxTotal.load(),
                          g_stress.load());
            reply(text);
        });
    }
}
