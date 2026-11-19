// [lcs] See lcs_frontend.h. The keys' handling is vcs_frontend.cpp's (Enter = Cross, Escape = Triangle while held,
// pressed in the menu; owned until let go); the menu's state is LCS's.
//
// LCS's pause menu is GTA III's CMenuManager: FrontEndMenuManager at 0x634610. CMenuManager::SwitchMenuOnAndOff
// (0x3426B0, called with it from 0x339200 / 0x33F3A8) flips +0x13D (m_bMenuActive) and calls CTimer::StartUserPause
// (0x2B50B8: m_UserPause = 1) when it turns on; the close (0x342A48, also called with 0x634610 from 0x342B20) clears
// +0x13D and calls CTimer::EndUserPause (0x2B50C8). +0x588 is the screen shown (set to 7 on opening).
//
// Where things are (the menu draws in the PSP's 480x272, scaled to the whole frame; read from the draw 0x339FE0 and
// the input 0x340B80, Ghidra):
// - The tab bar (map, brief, game, stats / controls, audio, display): manager +0 the tab (1..7), the screen follows it
//   (the draw 0x339200 maps it: 1 -> 7 map, 2 -> 2, 3 -> 1, 4 -> 0, 5 -> 3, 6 -> 4, 7 -> 5) while +0x5B4 is 0 (the tab
//   bar has the focus; 1: the page has it). Drawn only then, two centred rows (y 232: tabs 1-4, y 249: 5-7) by
//   0x345E60 (a0 the manager, a1 the tab's row {tab, label key, up, down, left, right}, a2 x, a3 y, t0 1 = measure
//   only), which returns the label's width + 12 (the next tab's x). The input steps the tab by the D-pad through
//   the neighbours at 0x3CA750 + tab * 0x18 (+0 up, +4 down, +8 left, +0xC right; 0x3CA810 when 0x3D8B94 == 4).
// - A page's entries: aScreens at 0x393A30, 0x116 a screen: its name (8), then 15 entries of 0x12: +0 action (s16;
//   1 = a title, not selectable), +2 the text key (8), +0xA type, +0xC x, +0xE y (s16, 480x272: the draw fills in
//   zeros as 320,100 / the previous + 30), +0x10 alignment. +0x58C is the entry selected (the draw highlights it from
//   y to y + 23).
// So the pointer: over a tab (tab bar shown) the D-pad steps toward it, one press a frame then a frame up (as a hand
// on the pad: the input wants the pad let go between steps); over a page's entry (the page focused) that entry is
// selected (+0x58C, as VC's PC menu does with the mouse); a click is Cross on what is under the pointer (once a tab
// is reached), else Cross as before; a click on an entry while the tab bar has the focus enters the page and clicks it.
// The pointer counts when it moves, so the keys keep working with it still.
// - The map page (screen 7, updated in the frontend's process 0x33F3A8): manager +0x488 its zoom, +0x48C / +0x490 where
//   it is drawn, +0x494 / +0x498 its cursor from (241, 112). The pointer puts the cursor under it, a drag moves the map
//   (within the D-pad's own limits), the wheel holds R1 / L1 (the game's zoom, about the cursor), a click is Cross.
// - Stats (screen 0, 0x33D0E0) and Brief (screen 2, 0x33E4C0) scroll by themselves, wrapping (offsets 0x3D8E58 /
//   0x3D8B74, speed 0x3D8B68 / 0x3D8B6C, way 0x3D8B70 / 0x3D8B71: the D-pad sets these): the wheel moves the offset.
#include "game/lcs_frontend.h"

#include "game/game_addrs.h"
#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/presenter.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace lcs::frontend
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            uint32_t manager;    // FrontEndMenuManager
            uint32_t menuActive; // u8 CMenuManager::m_bMenuActive (manager +)
            uint32_t screen, option, pageFocus; // s32 screen, s32 entry selected, u8 the page has the focus (manager +)
            uint32_t tabDraw;    // a tab's label in the tab bar (see above)
            uint32_t tabLinks, tabLinksAlt, tabLinksWhich; // the tabs' neighbours (and the other layout's, and its flag)
            uint32_t screens;    // aScreens
            // the map page (screen 7; its update is in the frontend's process 0x33F3A8): manager + floats, 480x272 units
            uint32_t mapSize;    // the map's zoom (400 at first; R1 +8 a frame while (size + 8) * aspect < 450, L1 -8)
            uint32_t mapX, mapY; // where it is drawn: the D-pad moves them within centre -/+ (size - 130) (y: 114 -/+
                                 // (size * aspect - 130)) while the cursor is in the middle
            uint32_t mapCursorX, mapCursorY; // the cursor from (241, 112): -239..239, -111..112 (the D-pad moves it
                                             // once the map is at its edge; zoom is about it, Square a waypoint there)
            uint32_t mapCentre;  // s32: the map's x centre (240)
            uint32_t wide;       // u8: the widescreen option (aspect 1, else 0.75: 0x3464D0)
        };
        constexpr Addrs kLcs{
            0x00634610u, 0x13Du, 0x588u, 0x58Cu, 0x5B4u,
            0x00345E60u,
            0x003CA750u, 0x003CA810u, 0x003D8B94u,
            0x00393A30u,
            0x488u, 0x48Cu, 0x490u, 0x494u, 0x498u, 0x003D8ECCu, 0x003D8B8Au,
        };
        constexpr int32_t kScreenMap = 7;
        // Stats (screen 0, drawn by 0x33D0E0) and Brief (screen 2, 0x33E4C0) scroll by themselves, endlessly (the lines
        // wrap): each frame their offset moves 30 / speed one way (the D-pad held: speed 20 and that way, else 150).
        // Stats: offset 0x3D8E58, a line at 196 + i * 15 - offset; Brief: 0x3D8B74, a line at ... + offset.
        struct Scroller
        {
            int32_t screen;
            uint32_t offset;
            float up; // the offset's change that moves the text up one step (a wheel notch down)
        };
        constexpr Scroller kScrollers[] = {{0, 0x003D8E58u, 30.0f}, {2, 0x003D8B74u, -22.0f}};
        constexpr int kZoomFrames = 6;    // a wheel notch: R1 / L1 held this many frames (8 a frame: 48)
        constexpr uint32_t kScreenSize = 0x116u, kEntrySize = 0x12u, kEntries = 15u;
        constexpr int kTabs = 8;
        constexpr float kMenuW = 480.0f, kMenuH = 272.0f;
        constexpr float kTabHeight = 15.0f;
        constexpr float kEntryHeight = 22.0f; // at most (the highlight is 23; rows closer than that end at the next)

        // the pad word as game/vcs_input.cpp builds it (pressed = 1)
        constexpr uint16_t kTriangle = 0x1000, kCross = 0x4000, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080;
        constexpr uint16_t kL1 = 0x0400, kR1 = 0x0800;
        constexpr uint16_t kDirButton[4] = {kUp, kDown, kLeft, kRight}; // the neighbours' order

        using namespace platform;
        // Escape, Enter (twice), and the mouse: the left button selects (Cross), the right one goes back (Triangle), the
        // wheel steps up / down (the D-pad)
        constexpr int kMenuKeys[] = {KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER, KEY_MOUSE_LEFT, KEY_MOUSE_RIGHT, KEY_WHEEL_UP, KEY_WHEEL_DOWN};
        constexpr size_t kMenuKeyCount = std::size(kMenuKeys);

        std::atomic<uint8_t *> g_rdram{nullptr};
        PS2Runtime::RecompiledFunction g_origTabDraw = nullptr;
        bool g_mouseOn = true; // LCS_MENU_MOUSE=0: no pointer (the buttons and the wheel only, as before)

        // the cursor over the menu's 480x272 (main thread -> game thread)
        std::atomic<float> g_cursorX{0.0f}, g_cursorY{0.0f};
        std::atomic<bool> g_cursorValid{false};
        std::atomic<int> g_windowW{0}, g_windowH{0}; // drawable pixels at the last host frame ("lcsmenumouse window")
        enum Injected : int { kNotInjected, kInjectedMenu, kInjectedWindow };
        std::atomic<int> g_injected{kNotInjected};
        std::atomic<float> g_injectedX{0.0f}, g_injectedY{0.0f};

        // game thread
        struct TabBox
        {
            int tab = 0;
            float x = 0, y = 0;
        };
        std::vector<TabBox> g_tabsDrawing, g_tabsDrawn; // the last frame drawn's (whole at a pad read), the one before
        uint32_t g_tabFrame = 0;                       // the frame g_tabsDrawing is for

        struct
        {
            uint32_t frame = 0xFFFFFFFFu;
            uint16_t out = 0;
            bool open = false;
            std::array<bool, kMenuKeyCount> owned{}, was{};
            bool escArmed = false, enterArmed = false, leftArmed = false, rightArmed = false;
            // the pointer
            float lastX = -1.0f, lastY = -1.0f;
            int goalTab = 0;      // a tab the D-pad is stepping toward (0: none)
            int goalFrames = 0;   // frames left for it
            bool selectAtGoal = false; // a click on it: Cross once it is the tab
            bool stepped = false; // a step went out last frame (this one is let go)
            int hover = -1;       // what is under the pointer: a tab (1..7) or an entry (100 + i), the map 200, -1 nothing
            // the map page
            bool mapDrag = false; // the left button went down over the map: it drags it (a click if it never moved)
            float mapDragX = 0, mapDragY = 0, mapMoved = 0;
            bool mapTap = false;  // Cross for a click on the map (sent after the button's release)
            int mapZoom = 0;      // R1 (+) / L1 (-) frames still to hold for the wheel
            int mapZoomTail = 0;  // frames after a zoom the cursor is still put back under the pointer
            int entryGoal = -1;   // an entry clicked while the tab bar had the focus: selected and Cross once in the page
            int entryGoalFrames = 0;
        } g;

        uint8_t *ram() { return g_rdram.load(std::memory_order_relaxed); }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            if (uint8_t *r = ram())
                std::memcpy(&v, r + (a & kRamMask), 4);
            return v;
        }
        int16_t rd16(uint32_t a)
        {
            int16_t v = 0;
            if (uint8_t *r = ram())
                std::memcpy(&v, r + (a & kRamMask), 2);
            return v;
        }
        uint8_t rd8(uint32_t a)
        {
            const uint8_t *r = ram();
            return r ? r[a & kRamMask] : 0;
        }
        void wr32(uint32_t a, uint32_t v)
        {
            if (uint8_t *r = ram())
                std::memcpy(r + (a & kRamMask), &v, 4);
        }
        float rdf(uint32_t a)
        {
            float v = 0.0f;
            if (uint8_t *r = ram())
                std::memcpy(&v, r + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v)
        {
            if (uint8_t *r = ram())
                std::memcpy(r + (a & kRamMask), &v, 4);
        }
        uint32_t frameNow() { return game::addrs().timer.frameCounter ? rd32(game::addrs().timer.frameCounter) : 0; }

        bool menuUp()
        {
            const uint8_t *rdram = ram();
            return rdram && rdram[(kLcs.manager + kLcs.menuActive) & kRamMask] != 0;
        }

        bool cursor(float &x, float &y)
        {
            if (g_injected.load(std::memory_order_relaxed) == kInjectedMenu)
            {
                x = g_injectedX.load(std::memory_order_relaxed);
                y = g_injectedY.load(std::memory_order_relaxed);
                return true;
            }
            x = g_cursorX.load(std::memory_order_relaxed);
            y = g_cursorY.load(std::memory_order_relaxed);
            return g_cursorValid.load(std::memory_order_relaxed);
        }

        // ---- where things are --------------------------------------------------------------------------------------

        // 0x345E60 (a tab's label): its place as drawn (t0 0: drawing, not measuring)
        PS2_GUEST_CC void tabDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 8) == 0 && (GPR_U32(ctx, 4) & kRamMask) == (kLcs.manager & kRamMask))
            {
                const uint32_t now = frameNow();
                if (now != g_tabFrame)
                {
                    g_tabsDrawn.swap(g_tabsDrawing);
                    g_tabsDrawing.clear();
                    g_tabFrame = now;
                }
                const int tab = static_cast<int>(rd32(GPR_U32(ctx, 5)));
                if (tab >= 1 && tab < kTabs && g_tabsDrawing.size() < 16)
                    g_tabsDrawing.push_back({tab, static_cast<float>(static_cast<int32_t>(GPR_U32(ctx, 6))),
                                             static_cast<float>(static_cast<int32_t>(GPR_U32(ctx, 7)))});
            }
            g_origTabDraw(rdram, ctx, runtime);
        }

        // The tab bar as last drawn (only while it has the focus): the tabs' boxes, a row's tabs from one x to the
        // next, its last to where the centred row ends.
        struct Box
        {
            int tab;
            float x0, y0, x1, y1;
        };
        std::vector<Box> tabBoxes()
        {
            std::vector<Box> out;
            // read at the pad read, on the game thread: no tab bar is being drawn then, the last one drawn is whole
            if (frameNow() - g_tabFrame > 2u)
                return out;
            const std::vector<TabBox> &tabs = g_tabsDrawing;
            for (const TabBox &t : tabs)
            {
                float next = 1e9f, first = t.x;
                for (const TabBox &u : tabs)
                    if (u.y == t.y)
                    {
                        if (u.x > t.x)
                            next = std::min(next, u.x);
                        first = std::min(first, u.x);
                    }
                const float end = next < 1e9f ? next - 6.0f : kMenuW - first - 6.0f;
                out.push_back({t.tab, t.x - 4.0f, t.y - 2.0f, std::max(end, t.x + 8.0f), t.y + kTabHeight});
            }
            return out;
        }

        // The entry of the page shown under the menu point (x, y): its index, or -1.
        int entryAt(float x, float y)
        {
            const int32_t screen = static_cast<int32_t>(rd32(kLcs.manager + kLcs.screen));
            if (screen < 0 || screen > 63 || x < 8.0f || x > kMenuW - 8.0f)
                return -1;
            const uint32_t base = kLcs.screens + 8u + static_cast<uint32_t>(screen) * kScreenSize;
            struct Row
            {
                int i;
                float y;
            };
            Row rows[kEntries];
            int n = 0;
            for (uint32_t i = 0; i < kEntries; ++i)
            {
                const uint32_t e = base + i * kEntrySize;
                if (rd16(e) == 1 || rd8(e + 2) == 0) // a title, or no entry
                    continue;
                const float ey = rd16(e + 0xE);
                if (ey <= 0.0f || ey >= kMenuH)
                    continue;
                rows[n++] = {static_cast<int>(i), ey};
            }
            for (int k = 0; k < n; ++k)
            {
                float bottom = rows[k].y + kEntryHeight;
                for (int j = 0; j < n; ++j)
                    if (rows[j].y > rows[k].y)
                        bottom = std::min(bottom, rows[j].y);
                if (y >= rows[k].y - 3.0f && y < bottom - 1.0f)
                    return rows[k].i;
            }
            return -1;
        }

        // The first D-pad step from tab `from` to `to` through the neighbours (breadth first), or 0.
        uint16_t stepToward(int from, int to)
        {
            const uint32_t links = rd32(kLcs.tabLinksWhich) == 4u ? kLcs.tabLinksAlt : kLcs.tabLinks;
            auto next = [&](int tab, int d) {
                const int32_t t = static_cast<int32_t>(rd32(links + static_cast<uint32_t>(tab) * 0x18u + static_cast<uint32_t>(d) * 4u));
                return t >= 1 && t < kTabs ? t : 0;
            };
            if (from < 1 || from >= kTabs || to < 1 || to >= kTabs || from == to)
                return 0;
            int firstDir[kTabs];
            std::fill(std::begin(firstDir), std::end(firstDir), -1);
            int queue[kTabs], head = 0, tail = 0;
            queue[tail++] = from;
            firstDir[from] = 4;
            while (head < tail)
            {
                const int t = queue[head++];
                for (int d = 0; d < 4; ++d)
                {
                    const int u = next(t, d);
                    if (u && firstDir[u] < 0)
                    {
                        firstDir[u] = t == from ? d : firstDir[t];
                        if (u == to)
                            return kDirButton[firstDir[u]];
                        queue[tail++] = u;
                    }
                }
            }
            return 0;
        }

        // The pointer, once a frame (menu up, not blocked): what it is over, and the buttons it adds.
        uint16_t pointer(bool leftPress, bool &clickTaken)
        {
            clickTaken = false;
            float x = 0.0f, y = 0.0f;
            const bool valid = g_mouseOn && cursor(x, y);
            const bool moved = valid && (std::fabs(x - g.lastX) > 0.5f || std::fabs(y - g.lastY) > 0.5f);
            if (valid)
            {
                g.lastX = x;
                g.lastY = y;
            }
            const bool inPage = rd8(kLcs.manager + kLcs.pageFocus) != 0;
            const int tab = static_cast<int>(rd32(kLcs.manager));
            g.hover = -1;
            if (valid && !inPage)
                for (const Box &b : tabBoxes())
                    if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1)
                        g.hover = b.tab;
            int entry = -1;
            if (valid && inPage && (entry = entryAt(x, y)) >= 0)
                g.hover = 100 + entry;

            if (inPage)
                g.goalTab = 0;
            else if (g.hover >= 1 && g.hover < kTabs && (moved || leftPress))
            {
                g.goalTab = g.hover;
                g.goalFrames = 40;
                g.selectAtGoal = g.selectAtGoal || leftPress;
                clickTaken = leftPress;
            }
            if (entry >= 0 && (moved || leftPress))
            {
                if (static_cast<int>(rd32(kLcs.manager + kLcs.option)) != entry)
                    wr32(kLcs.manager + kLcs.option, static_cast<uint32_t>(entry));
            }

            // a click on an entry of the page while the tab bar has the focus: Cross to enter the page, then (a frame up
            // between) the entry selected and Cross on it, as a click on it in the page
            uint16_t out = 0;
            if (!inPage && leftPress && valid && !clickTaken && (entry = entryAt(x, y)) >= 0)
            {
                g.entryGoal = entry;
                g.entryGoalFrames = 30;
                g.goalTab = 0;
                clickTaken = true;
                g.stepped = true;
                return kCross;
            }
            if (g.entryGoal >= 0)
            {
                if (--g.entryGoalFrames <= 0)
                    g.entryGoal = -1;
                else if (inPage && !g.stepped)
                {
                    wr32(kLcs.manager + kLcs.option, static_cast<uint32_t>(g.entryGoal));
                    g.entryGoal = -1;
                    out = kCross;
                }
                g.stepped = out != 0;
                return out;
            }
            // a step toward the tab, a frame up between steps; Cross once there (a click)
            if (g.goalTab)
            {
                if (--g.goalFrames <= 0)
                    g.goalTab = 0, g.selectAtGoal = false;
                else if (tab == g.goalTab)
                {
                    if (g.selectAtGoal && !g.stepped)
                        out = kCross;
                    if (!g.selectAtGoal || out)
                        g.goalTab = 0, g.selectAtGoal = false;
                }
                else if (!g.stepped)
                    out = stepToward(tab, g.goalTab);
            }
            g.stepped = out != 0;
            return out;
        }

        bool mapPage()
        {
            return static_cast<int32_t>(rd32(kLcs.manager + kLcs.screen)) == kScreenMap && rd8(kLcs.manager + kLcs.pageFocus) != 0;
        }

        // The map page with the pointer (the PC way): the map's cursor follows it, the left button drags the map (in
        // the game's own limits), a click without a drag is Cross, the wheel zooms (R1 / L1 held a moment: about the
        // cursor, so about the pointer). Returns the buttons it adds.
        uint16_t mapPointer(bool left, bool leftPress, bool wheelUp, bool wheelDown)
        {
            const uint32_t m = kLcs.manager;
            float x = 0.0f, y = 0.0f;
            const bool valid = cursor(x, y) && x >= 0.0f && x < kMenuW && y >= 0.0f && y < kMenuH;
            const bool moved = valid && (std::fabs(x - g.lastX) > 0.5f || std::fabs(y - g.lastY) > 0.5f);
            if (valid)
            {
                g.lastX = x;
                g.lastY = y;
            }
            // the zoom moves the cursor (toward the middle): back under the pointer each frame of it, and a few after
            g.mapZoomTail = g.mapZoom != 0 || wheelUp || wheelDown ? 4 : std::max(0, g.mapZoomTail - 1);
            if (valid && (moved || leftPress || g.mapZoomTail > 0))
            {
                wrf(m + kLcs.mapCursorX, std::clamp(x - 241.0f, -239.0f, 239.0f));
                wrf(m + kLcs.mapCursorY, std::clamp(y - 112.0f, -111.0f, 112.0f));
            }
            if (leftPress && valid)
            {
                g.mapDrag = true;
                g.mapDragX = x;
                g.mapDragY = y;
                g.mapMoved = 0.0f;
            }
            uint16_t out = 0;
            if (g.mapDrag && left)
            {
                const float dx = valid ? x - g.mapDragX : 0.0f, dy = valid ? y - g.mapDragY : 0.0f;
                if (valid)
                {
                    g.mapDragX = x;
                    g.mapDragY = y;
                }
                g.mapMoved += std::fabs(dx) + std::fabs(dy);
                if (g.mapMoved > 3.0f && (dx != 0.0f || dy != 0.0f))
                {
                    // the map moves with the pointer, as far as the D-pad could take it
                    const float size = rdf(m + kLcs.mapSize);
                    const float aspect = rd8(kLcs.wide) ? 1.0f : 0.75f;
                    const float cx = static_cast<float>(static_cast<int32_t>(rd32(kLcs.mapCentre)));
                    const float rx = std::max(0.0f, size - 130.0f), ry = std::max(0.0f, size * aspect - 130.0f);
                    const float mx = rdf(m + kLcs.mapX), my = rdf(m + kLcs.mapY);
                    wrf(m + kLcs.mapX, std::clamp(mx + dx, std::min(mx, cx - rx), std::max(mx, cx + rx)));
                    wrf(m + kLcs.mapY, std::clamp(my + dy, std::min(my, 114.0f - ry), std::max(my, 114.0f + ry)));
                }
            }
            else if (g.mapDrag)
            {
                g.mapTap = g.mapMoved <= 3.0f; // a click: the game's Cross
                g.mapDrag = false;
            }
            else if (g.mapTap)
            {
                out |= kCross;
                g.mapTap = false;
            }
            if (wheelUp)
                g.mapZoom = std::min(g.mapZoom, 0) + kZoomFrames;
            if (wheelDown)
                g.mapZoom = std::max(g.mapZoom, 0) - kZoomFrames;
            if (g.mapZoom > 0)
                out |= kR1, --g.mapZoom;
            else if (g.mapZoom < 0)
                out |= kL1, ++g.mapZoom;
            g.hover = valid ? 200 : -1;
            return out;
        }

        void step(bool blocked)
        {
            const bool open = menuUp();
            std::array<bool, kMenuKeyCount> now{};
            for (size_t i = 0; i < kMenuKeyCount; ++i)
            {
                now[i] = ps2control::keyDown(kMenuKeys[i]);
                g.owned[i] = now[i] && ((open && !blocked) || g.owned[i]);
            }
            const bool alt = ps2control::keyDown(KEY_LEFT_ALT) || ps2control::keyDown(KEY_RIGHT_ALT); // Alt+Enter
            const bool esc = now[0], enter = (now[1] || now[2]) && !alt;
            const bool escPress = esc && !g.was[0], enterPress = enter && !(g.was[1] || g.was[2]);
            const bool left = now[3], right = now[4];
            const bool leftPress = left && !g.was[3], rightPress = right && !g.was[4];
            bool wheelUp = now[5] && !g.was[5], wheelDown = now[6] && !g.was[6]; // a notch: one step
            g.was = now;
            if (open != g.open)
            {
                g.open = open;
                g.escArmed = g.enterArmed = g.leftArmed = g.rightArmed = false;
                g.goalTab = 0;
                g.entryGoal = -1;
                g.selectAtGoal = g.stepped = false;
            }
            const bool onMap = g_mouseOn && open && !blocked && mapPage();
            if (!onMap)
                g.mapDrag = g.mapTap = false, g.mapZoom = 0;
            if (!open || blocked)
            {
                g.out = 0;
                g.escArmed = g.enterArmed = g.leftArmed = g.rightArmed = false;
                g.goalTab = 0;
                g.hover = -1;
                return;
            }
            // Escape still down from opening the menu is not a back
            g.escArmed = esc && (g.escArmed || escPress);
            g.enterArmed = enter && (g.enterArmed || enterPress);
            // a button still down from before the menu opened is not a press in it
            g.leftArmed = left && (g.leftArmed || leftPress);
            g.rightArmed = right && (g.rightArmed || rightPress);
            // Stats / Brief: a wheel notch scrolls the text a step (the D-pad would turn its own scrolling around)
            if (g_mouseOn && (wheelUp || wheelDown) && rd8(kLcs.manager + kLcs.pageFocus) != 0)
                for (const Scroller &sc : kScrollers)
                    if (static_cast<int32_t>(rd32(kLcs.manager + kLcs.screen)) == sc.screen)
                    {
                        wrf(sc.offset, rdf(sc.offset) + (wheelDown ? sc.up : -sc.up));
                        wheelUp = wheelDown = false;
                    }
            if (onMap)
            {
                // the map has the left button and the wheel (drag, click, zoom)
                g.out = static_cast<uint16_t>(((g.escArmed || g.rightArmed) ? kTriangle : 0) | (g.enterArmed ? kCross : 0) |
                                              mapPointer(g.leftArmed, g.leftArmed && leftPress, wheelUp, wheelDown));
                return;
            }
            bool clickTaken = false;
            const uint16_t pointed = pointer(g.leftArmed && leftPress, clickTaken);
            // a click on a tab other than the one up: its Cross comes when the tab is reached (pointer)
            if (clickTaken)
                g.leftArmed = false;
            g.out = static_cast<uint16_t>(((g.escArmed || g.rightArmed) ? kTriangle : 0) | ((g.enterArmed || g.leftArmed) ? kCross : 0) |
                                          (wheelUp ? kUp : 0) | (wheelDown ? kDown : 0) | pointed);
        }

        // ---- "lcsmenu" ---------------------------------------------------------------------------------------------
        std::mutex g_stateMutex;
        std::string g_state = "open=0";

        void publish()
        {
            char buf[160];
            std::string s;
            if (!menuUp())
                s = "open=0";
            else
            {
                float x = 0, y = 0;
                const bool on = cursor(x, y);
                std::snprintf(buf, sizeof(buf), "open=1 window=%dx%d tab=%d screen=%d page=%d option=%d hover=%d cursor=",
                              g_windowW.load(std::memory_order_relaxed), g_windowH.load(std::memory_order_relaxed), static_cast<int>(rd32(kLcs.manager)),
                              static_cast<int>(rd32(kLcs.manager + kLcs.screen)), rd8(kLcs.manager + kLcs.pageFocus),
                              static_cast<int>(rd32(kLcs.manager + kLcs.option)), g.hover);
                s = buf;
                if (on)
                    std::snprintf(buf, sizeof(buf), "%.1f,%.1f", x, y);
                s += on ? buf : "-";
                s += " tabs=";
                for (const Box &b : tabBoxes())
                {
                    std::snprintf(buf, sizeof(buf), "%d:%.0f,%.0f,%.0f,%.0f;", b.tab, b.x0, b.y0, b.x1, b.y1);
                    s += buf;
                }
                if (static_cast<int32_t>(rd32(kLcs.manager + kLcs.screen)) == kScreenMap)
                {
                    std::snprintf(buf, sizeof(buf), " map=%.1f,%.1f,%.1f,%.1f,%.1f", rdf(kLcs.manager + kLcs.mapSize),
                                  rdf(kLcs.manager + kLcs.mapX), rdf(kLcs.manager + kLcs.mapY),
                                  rdf(kLcs.manager + kLcs.mapCursorX), rdf(kLcs.manager + kLcs.mapCursorY));
                    s += buf;
                }
                s += " entries=";
                const int32_t screen = static_cast<int32_t>(rd32(kLcs.manager + kLcs.screen));
                if (screen >= 0 && screen <= 63)
                    for (uint32_t i = 0; i < kEntries; ++i)
                    {
                        const uint32_t e = kLcs.screens + 8u + static_cast<uint32_t>(screen) * kScreenSize + i * kEntrySize;
                        if (!rd8(e + 2))
                            continue;
                        char key[9] = {};
                        for (int k = 0; k < 8; ++k)
                            key[k] = static_cast<char>(rd8(e + 2 + k));
                        std::snprintf(buf, sizeof(buf), "%u:%s:%d:%d,%d;", i, key, rd16(e), rd16(e + 0xC), rd16(e + 0xE));
                        s += buf;
                    }
            }
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state = s;
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram.store(runtime.memory().getRDRAM(), std::memory_order_relaxed);
        if (const char *e = std::getenv("LCS_MENU_MOUSE"); e && e[0] == '0')
            g_mouseOn = false;
        if (g_mouseOn)
        {
            g_origTabDraw = runtime.lookupFunction(kLcs.tabDraw);
            if (g_origTabDraw)
                runtime.replaceFunction(kLcs.tabDraw, &tabDraw);
            else
                std::fprintf(stderr, "[lcs] pause menu: the tab bar's draw not found: no pointing at tabs\n");
        }
        // lcsmenumouse <x> <y> (the menu's 480x272) | lcsmenumouse window <x> <y> | lcsmenumouse off: the cursor set
        // from here (hidden test runs: no real cursor). lcsmenu -> the menu's state, tabs as drawn, the page's entries.
        ps2control::addCommand("lcsmenumouse", [](const std::string &args, ps2control::Reply reply) {
            char what[16] = {};
            float x = 0, y = 0;
            if (std::sscanf(args.c_str(), "%15s", what) == 1 && std::string(what) == "off")
            {
                g_injected = kNotInjected;
                return reply("ok");
            }
            if (std::sscanf(args.c_str(), "window %f %f", &x, &y) == 2)
            {
                g_injectedX = x;
                g_injectedY = y;
                g_injected = kInjectedWindow;
                return reply("ok");
            }
            if (std::sscanf(args.c_str(), "%f %f", &x, &y) == 2)
            {
                g_injectedX = x;
                g_injectedY = y;
                g_injected = kInjectedMenu;
                return reply("ok");
            }
            reply("error usage: lcsmenumouse <x> <y> | lcsmenumouse window <x> <y> | lcsmenumouse off");
        });
        ps2control::addCommand("lcsmenu", [](const std::string &, ps2control::Reply reply) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            reply("ok " + g_state);
        });
        std::fprintf(stderr, "[lcs] pause menu: Enter / left click select, Escape / right click go back, the wheel steps%s\n",
                     g_mouseOn && g_origTabDraw ? ", the pointer picks tabs and entries" : "");
    }

    bool open() { return menuUp(); }

    void onHostFrame()
    {
        int w = 0, h = 0;
        windowSize(w, h);
        g_windowW.store(w, std::memory_order_relaxed);
        g_windowH.store(h, std::memory_order_relaxed);
        float mx = 0.0f, my = 0.0f;
        bool over = false;
        if (g_injected.load(std::memory_order_relaxed) == kInjectedWindow)
        {
            mx = g_injectedX.load(std::memory_order_relaxed);
            my = g_injectedY.load(std::memory_order_relaxed);
            over = g_mouseOn && w > 0 && h > 0;
        }
        else
            over = g_mouseOn && ps2control::physicalInputOn() && mousePosition(mx, my) && w > 0 && h > 0;
        if (over)
        {
            // the menu is shown 4:3 in the game's frame (lcs_hud.cpp tags it a menu), its 480x272 drawing filling that
            // area (vcs_frontend.cpp's mapping)
            float x0 = 0.0f, y0 = 0.0f, fw = 0.0f, fh = 0.0f;
            present::fitRect(4.0f / 3.0f, present::displayAspect(true), static_cast<float>(w), static_cast<float>(h), x0, y0, fw, fh);
            if (fw > 0.0f && fh > 0.0f)
            {
                g_cursorX.store((mx - x0) / fw * kMenuW, std::memory_order_relaxed);
                g_cursorY.store((my - y0) / fh * kMenuH, std::memory_order_relaxed);
            }
            else
                over = false;
        }
        g_cursorValid.store(over, std::memory_order_relaxed);
    }

    uint16_t padButtons(uint16_t, bool blocked)
    {
        const uint8_t *rdram = ram();
        const uint32_t counter = game::addrs().timer.frameCounter;
        if (!rdram || !counter)
            return 0;
        uint32_t frame;
        std::memcpy(&frame, rdram + (counter & kRamMask), 4);
        if (frame != g.frame) // the game reads the pad several times a frame: one step per frame
        {
            g.frame = frame;
            step(blocked);
            publish();
        }
        return g.out;
    }

    bool ownsKey(int key)
    {
        for (size_t i = 0; i < kMenuKeyCount; ++i)
            if (kMenuKeys[i] == key)
                return g.owned[i] || g.open;
        return false;
    }
}
