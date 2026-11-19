// [lcs] See lcs_hud.h. VCS's HUD logic (game/vcs_hud.cpp: capture, anchoring, classifier, native HUD layer, the
// tall bars, the scope mask's side bars) driven from LCS's addresses through its [lcs] seams (vcs::hudEnter / hudIm2D /
// hudRenderState / hudTallBar / hudMaskSideBar / onFrameKick). LCS's 2D is GTA3's: Render2dStuff (0x1F6338) sets the
// render states, draws the cutscene letterbox, the scope's off-screen bars, then CHud::Draw, the messages and finally
// CFont::DrawFonts (text is buffered and drawn there) - all inside the HUD context, which runs from CHud::Draw's entry
// to the frame kick as in VCS. Im2D vertices are VCS's layout (u, v, RGBA, x / y s16 in 640x448, z u32), from
// CSprite2d (rects in the PSP's 480x272 scaled by 640 / 480, 448 / 272).
//
// As VCS (vcs_hud.cpp), with LCS's own ways:
// - Menus: the frontend (CMenuManager's draw from the game loop, its screens and message boxes) and the loading screens
//   mark their 2D as a menu - kept in the middle, never at the screen edges, and the frame shown 4:3 at once.
// - The radar is known by who draws it: CRadar::DrawMap's entry to CHud::Draw's next element (its calls after the
//   radar's part of CHud::Draw), so text or icons near it never group with it.
// - Text: GTA3's CFont buffers every PrintString and draws them all in one Im2D call (the sprite batch's flush): a run
//   of glyphs per buffered line, each its own HUD primitive (hudIm2D's runs); a line centred across the screen (the
//   centre flag with a wide centre size around the middle: subtitles, mission text, big messages) stays in the middle,
//   text printed by the radar goes with the radar.
// - The sniper scope's side bars (two rect fills in CHud::Draw before the scope's four quarters) reach the screen edges.
// VCS_HUD_RW=0 leaves LCS's 2D alone (the renderer's far-plane heuristic only); VCS_HUD_NATIVE=0 draws the HUD into the
// frame buffer (squeezed) instead of the native HUD layer; VCS_MENU_NATIVE=0 / VCS_LOADING_NATIVE=0 leave menus / loading
// screens unmarked; VCS_SCOPE_FILL=0 leaves the scope's side bars. The trainer's Hide HUD skips CHud::Draw (hudDraw).
#include "game/lcs_hud.h"

#include "game/game_table.h"
#include "game/lcs_trainer.h"
#include "game/vcs_hud.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

namespace lcs::hud
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Range
        {
            uint32_t begin, end;
            bool has(uint32_t a) const { return a >= begin && a < end; }
        };

        struct Addrs
        {
            uint32_t im2d;        // RwIm2DRenderPrimitive(prim a0, vertices a1, count a2)
            uint32_t im2dIndexed; // RwIm2DRenderIndexedPrimitive(prim, vertices, count, indices, indexCount)
            uint32_t renderState; // RwRenderStateSet(state a0, value a1); 6 = ZTESTENABLE
            uint32_t hudDraw;     // CHud::Draw
            uint32_t frameKick;   // the frame's display-list kick
            uint32_t rectFill;    // CSprite2d::DrawRect(rect a0: left, bottom, right, top in 480x272; colour a1; a2)
            uint32_t bars[6];     // return addresses of rect fills that are top / bottom black bars
            uint32_t maskLeft, maskRight; // return addresses of the scope mask's side bars' rect fills
            uint32_t uvSprite;    // CSprite2d::Draw(rect, colour, u / v ...): the scope's quarters
            uint32_t scopeQuarters[4]; // its return addresses for them (a primitive each: never grouped, the middle)
            // menus and loading screens
            uint32_t menuDraw;    // CMenuManager's draw / process from the game loop
            uint32_t menuScreen;  // the frontend's screen draw (also the start-up screens' loop)
            uint32_t menuMessage; // the frontend's message box (memory card, start-up)
            uint32_t loading;     // LoadingScreen(title, text, splash)
            uint32_t loadingIsland; // LoadingIslandScreen
            // the radar
            uint32_t radarDraw;   // CRadar::DrawMap: the radar's first part in CHud::Draw
            Range hudBody;        // CHud::Draw's code
            Range radarPart;      // the return addresses of CHud::Draw's calls that draw the radar (map, ring, blips)
            uint32_t afterRadar[6]; // calls after the radar: CHud::Draw's next (3D markers), Render2dStuff's after CHud::Draw
            uint32_t sprite[2];   // CSprite2d draws (rect, colour..): with a return address in CHud::Draw outside the
                                  // radar's part, the radar is over
            uint32_t rectCtor;    // CRect(l, b, r, t): likewise
            // text (GTA3's CFont)
            uint32_t printString; // CFont::PrintString(x f12, y f13, string a0, ...)
            uint32_t printLine;   // the buffered line: (x f12, y f13, ...) -> the font buffer
            Range printStringBody;
            Range printLineBody;
            uint32_t printChar;   // CFont::PrintChar(x, y, char): one glyph into the sprite batch
            uint32_t drawFonts;   // CFont::DrawFonts: renders the buffer
            uint32_t glyphFlush;  // the return address of the sprite batch's Im2D call
            uint32_t batchCount;  // s32 the sprite batch's vertex count
            uint32_t centreOn;    // u8 CFont details: centred
            uint32_t centreSize;  // f32 CFont details: the centred line's width
            uint32_t lineX, lineY; // f32 the line being rendered: its buffered x, y
        };
        constexpr Addrs kLcs{
            0x002B1A70u, // exact opcode-shape twin of VCS's 0x2FBE08 (LCS address report: kIm2DRender)
            0x002B1AD0u, // exact twin of VCS's 0x2FBE68 (kIm2DRenderIndexed)
            0x00144B58u, // twin of VCS's 0x175198 (best.py 0.98); the rect fill below calls it with 1, 5, 4, 6, 0xA
            0x0023F438u, // Render2dStuff 0x1F6338 calls, in GTA3's order: DrawBordersForWideScreen 0x28CF08, the
                         // scope bars, DisplayRadioStationName 0x16A670, then this (0x1F655C), OnscnTimer, CMessages,
                         // ..., CFont::DrawFonts 0x166A30. 17 KB, 19 PrintString / 49 CRGBA / 6 DrawRect calls; saves
                         // the widescreen byte (0x241E58) and restores it (0x241F6C) around one element
            0x00212810u, // exact twin of VCS's 0x2723E8 (ratio 1.0, 33 instructions): the VIF1 display-list kick; the
                         // frame's list is queued (0x212680) and kicked from the flip 0x2D9640 (0x212610)
            0x003219C0u, // exact twin of VCS's 0x3E0B10 (ratio 1.0): SetVertices 0x321068, render states, Im2D(5, 4)
            {
                0x0028CFA4u, 0x0028D004u, // DrawBordersForWideScreen 0x28CF08: the cutscene letterbox, 0..h and
                                          // 272 - h..272 (VCS 0x280560 / 0x2805AC)
                0x001F64B0u, 0x001F64E0u, // Render2dStuff, weapon 0x10 in a scope camera mode (7, 8, 0x27, 0x28,
                                          // 0x2E): bars -44..0 and 272..306, just off the screen (VCS 0x21F538 ...)
                0x001F6520u, 0x001F6550u, // the same for weapons 0x1C / 0x1D: -74..0 and 272..346
            },
            0x0023FB7Cu, // CHud::Draw, scope camera modes (7, 8, 0x22, 0x2D, 0x2E) with a scope weapon but not the
            0x0023FBC8u, // camera (0x24): rect(-1, 0, 30, 272) then rect(450, 0, 480, 272) in black, then the scope's
                         // four quarters around (240, 136), 280 (210 with the widescreen byte) x 210 (VCS 0x21F6F0 /
                         // 0x21F728, drawn by its 2D pass)
            0x00320E60u, // (u, v x 8 in f12.., the sprite, rect, colour) -> Im2D 0x2B1A70: called by CHud::Draw for
                         // the binoculars' / camera's crosshair (0x23F96C) and the scope's quarters below
            {0x0023FC74u, 0x0023FCF4u, 0x0023FD60u, 0x0023FDCCu}, // the four calls after the side bars, rects from
                                                                   // (240 - w, 136 - h) to (240 + w, 136 + h)
            0x00339200u, // called only by RenderMenus 0x1F65C8 (from the game loop 0x1F5BC0 after Render2dStuff)
                         // when FrontEndMenuManager (0x634610) +0x13D m_bMenuActive is set; then a fade rect
            0x00340B80u, // 4.3 KB, called with 0x634610 from the start-up loop 0x33EAE0 and 0x33F3A8 (the frontend)
            0x00343228u, // called with 0x634610 and a text key from 0x33EAE0 (memory card check), 0x3430A8 (its
                         // frames from the load-game loop 0x2FB700) and others
            0x001F5270u, // GTA3's LoadingScreen: own frame (0x1F3F98 .. 0x1F4C88), splash 0x320E00, three PrintString
                         // texts by a mode, the progress bar 0x1F50D0, DrawFonts; callers 0x1F37B0, 0x2FAE40 (the
                         // game's load), the game loop's start 0x1F5FA8
            0x001F57B0u, // GTA3's LoadingIslandScreen: own frame, a black rect, the splash, DrawFonts (from 0x18B728)
            0x00115AE0u, // CRadar::DrawMap: GTA3's (vehicle speed -> zoom 96..280, then DrawRadarMap 0x11B1E0 unless
                         // the menu getter 0x3462C0 says 1); called only from CHud::Draw 0x241E64
            {0x0023F438u, 0x002437C8u}, // functions.csv
            {0x00241E64u, 0x00241F78u}, // 0x241E64 jal DrawMap .. the radar ring (0x245C80.. radar position getters,
                                        // CRect 0x25B300, sprite 0x320EB0 with 0x433DF8) .. 0x241F70 jal DrawBlips
                                        // 0x117C08 (GTA3's order: DrawMap, the disc, DrawBlips)
            {
                0x00118380u, // CHud::Draw's next call after DrawBlips (0x241FA8): the blips' 3D markers
                0x002464C8u, 0x002E4330u, 0x002F7F38u, 0x0023B060u, 0x00199A70u, // Render2dStuff's calls after
                                                                                 // CHud::Draw (0x1F6574 .. 0x1F6598)
            },
            {0x00320E00u, 0x00320EB0u}, // CSprite2d::Draw(rect, colour) / (rect, four colours): CHud's sprites
            0x0025B300u, // CRect ctor (l f12, b f13, r f14, t f15, this a0): before every element CHud::Draw draws
            0x001658F8u, // 36 callers; x - width * 0.5 when 0x4083C9 is 1, wraps at 0x4083E0 when centred; calls the
                         // line routine below for each line
            0x00165418u, // writes x, y, details to the font buffer (0x3D96E4 the write pointer), recursing first for the
                         // drop shadow (x + d * 0.75, y + d * 0.607); RenderFontBuffer copies them to 0x40841C / 0x408420
            {0x001658F8u, 0x00165D5Cu}, // functions.csv
            {0x00165418u, 0x0016583Cu}, // functions.csv
            0x00164C50u, // RenderFontBuffer 0x164FB8's per-character call (x f12, y f13, char a0): CRect 0x25B300 then
                         // the batch add 0x320BB8
            0x00166A30u, // jumps to RenderFontBuffer 0x164FB8; from Render2dStuff's end, CHud::Draw, the menus
            0x00320CF8u, // 0x320CB0 (RenderFontBuffer's end, the batch add when full): Im2DRenderIndexed(3, 0x6239E0,
                         // [0x3DA1A8], 0x6279E0, [0x3DA1AC]) at 0x320CF0, then both counts cleared
            0x003DA1A8u, // the vertex count passed above (the buffer at 0x6239E0, 16-byte vertices)
            0x004083C9u, // PrintString: centred (x - width / 2), else 0x4083CA right-justified
            0x004083E0u, // PrintString: the wrap width when 0x4083C9 is set (SetCentreSize; 400 / 454 in CHud::Draw)
            0x0040841Cu, // RenderFontBuffer: the current line's x, y as the line routine buffered them
            0x00408420u,
        };

        PS2Runtime::RecompiledFunction g_im2d = nullptr, g_im2dIndexed = nullptr, g_renderState = nullptr,
                                       g_hudDraw = nullptr, g_frameKick = nullptr, g_rectFill = nullptr;
        bool g_scopeFill = true;

        // game thread
        bool g_radar = false; // the radar is being drawn
        void radarOff()
        {
            if (!g_radar)
                return;
            g_radar = false;
            vcs::hudEnter(vcs::HudContext::NoRadar);
        }

        // Text: the parts of the buffered lines (by their buffered x, y), the runs of the sprite batch's vertices
        struct Line
        {
            float x, y;
            vcs::HudRunPart part;
        };
        std::vector<Line> g_lines;
        std::vector<vcs::HudRun> g_runs;
        vcs::HudRunPart g_printPart = vcs::HudRunPart::Other; // the latest PrintString's
        float g_runX = 0.0f, g_runY = 0.0f;                    // the line of the last run
        vcs::HudRunPart g_runPart = vcs::HudRunPart::Other;

        float rdF32(uint8_t *rdram, uint32_t a)
        {
            float f;
            std::memcpy(&f, rdram + (a & kRamMask), 4);
            return f;
        }

        vcs::HudRunPart linePart(float x, float y)
        {
            for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it)
                if (it->x == x && it->y == y)
                    return it->part;
            // (the drop shadow's line, buffered by a call that may not come through the hook): the nearest
            for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it)
                if (std::fabs(it->x - x) <= 3.0f && std::fabs(it->y - y) <= 3.0f)
                    return it->part;
            return vcs::HudRunPart::Other;
        }

        // ---- hooks: work before the original (docs/UPSTREAM_AGENTS.md rule 1), then call what was registered before ----
        PS2_GUEST_CC void im2d(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            vcs::hudIm2D(rdram, ctx, false);
            g_im2d(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void im2dIndexed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kLcs.glyphFlush && !g_runs.empty())
            {
                vcs::hudIm2D(rdram, ctx, true, g_runs.data(), static_cast<uint32_t>(g_runs.size()));
                // the batch starts again (the glyph that filled it goes first into the new one)
                g_runs.assign(1, vcs::HudRun{0u, g_runPart});
            }
            else
                vcs::hudIm2D(rdram, ctx, true);
            g_im2dIndexed(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void renderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            vcs::hudRenderState(GPR_U32(ctx, 4), GPR_U32(ctx, 5));
            g_renderState(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void hudDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_radar = false;
            // the trainer's Hide HUD: none of CHud::Draw this frame (radar, money, weapon, stars, clock; as VCS's
            // vcs_hud.cpp). Subtitles / help text come from CMessages after it (Render2dStuff) and stay.
            if (ctx->pc == kLcs.hudDraw && lcs::trainer::toggle(vcs::trainer::HideHud))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            vcs::hudEnter(vcs::HudContext::Hud);
            g_hudDraw(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_radar = false;
            g_lines.clear();
            vcs::onFrameKick();
            g_frameKick(rdram, ctx, runtime);
        }

        // in CHud::Draw past the radar's part: the radar is over
        void afterRadarCall(R5900Context *ctx)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_radar && kLcs.hudBody.has(ra) && !kLcs.radarPart.has(ra))
                radarOff();
        }

        PS2_GUEST_CC void rectFill(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (std::find(std::begin(kLcs.bars), std::end(kLcs.bars), ra) != std::end(kLcs.bars))
                vcs::hudTallBar(rdram, ctx);
            else if (g_scopeFill && (ra == kLcs.maskLeft || ra == kLcs.maskRight))
                vcs::hudMaskSideBar(rdram, ctx, ra == kLcs.maskLeft);
            afterRadarCall(ctx);
            g_rectFill(rdram, ctx, runtime);
        }

        // the simple "before" hooks: a table of (address, what to do, the original)
        struct Hook
        {
            uint32_t address;
            void (*before)(uint8_t *rdram, R5900Context *ctx);
            PS2Runtime::RecompiledFunction original;
        };

        void menuEnter(uint8_t *, R5900Context *) { vcs::hudEnter(vcs::HudContext::Menu); }
        void loadingEnter(uint8_t *, R5900Context *) { vcs::hudEnter(vcs::HudContext::Loading); }
        void radarEnter(uint8_t *, R5900Context *)
        {
            g_radar = true;
            vcs::hudEnter(vcs::HudContext::Radar);
        }
        void radarOver(uint8_t *, R5900Context *) { radarOff(); }
        void afterRadarHook(uint8_t *, R5900Context *ctx) { afterRadarCall(ctx); }

        void printString(uint8_t *rdram, R5900Context *ctx)
        {
            afterRadarCall(ctx);
            const float x = ctx->f[12];
            const bool centred = rdram[kLcs.centreOn & kRamMask] == 1u && rdF32(rdram, kLcs.centreSize) >= 240.0f &&
                                 std::fabs(x - 240.0f) <= 64.0f;
            g_printPart = g_radar ? vcs::HudRunPart::Radar : centred ? vcs::HudRunPart::CentredText : vcs::HudRunPart::Other;
            // as VCS's: a line centred right of the middle to clear the 4:3 radar (the mission text, x 284) goes to the
            // middle when the radar is at the screen edge
            if (g_printPart == vcs::HudRunPart::CentredText && std::fabs(x - 240.0f) >= 0.5f && vcs::hudCentreText())
                ctx->f[12] = 240.0f;
        }

        void printLine(uint8_t *, R5900Context *ctx)
        {
            // from PrintString (or its own drop-shadow recursion): that call's part; from elsewhere, by the radar only
            const uint32_t ra = GPR_U32(ctx, 31);
            const vcs::HudRunPart part = kLcs.printStringBody.has(ra) || kLcs.printLineBody.has(ra)
                                             ? g_printPart
                                             : g_radar ? vcs::HudRunPart::Radar
                                                       : vcs::HudRunPart::Other;
            if (g_lines.size() >= 1024u)
                g_lines.erase(g_lines.begin(), g_lines.begin() + 512);
            g_lines.push_back({ctx->f[12], ctx->f[13], part});
        }

        void printChar(uint8_t *rdram, R5900Context *)
        {
            const float x = rdF32(rdram, kLcs.lineX), y = rdF32(rdram, kLcs.lineY);
            int32_t count;
            std::memcpy(&count, rdram + (kLcs.batchCount & kRamMask), 4);
            const uint32_t v = count > 0 ? static_cast<uint32_t>(count) : 0u;
            if (!g_runs.empty() && v < g_runs.back().firstVertex) // the batch was drawn by someone else meanwhile
                g_runs.clear();
            if (g_runs.empty() || x != g_runX || y != g_runY)
            {
                g_runX = x;
                g_runY = y;
                g_runPart = linePart(x, y);
                if (!g_runs.empty() && g_runs.back().firstVertex == v)
                    g_runs.back().part = g_runPart;
                else if (g_runs.size() < 4096u)
                    g_runs.push_back({v, g_runPart});
            }
        }

        void drawFonts(uint8_t *, R5900Context *) { radarOff(); }

        void uvSprite(uint8_t *, R5900Context *ctx)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (std::find(std::begin(kLcs.scopeQuarters), std::end(kLcs.scopeQuarters), ra) != std::end(kLcs.scopeQuarters))
                vcs::hudNextPart(vcs::HudRunPart::CentredText);
            afterRadarCall(ctx);
        }

        Hook g_hooks[] = {
            {kLcs.menuDraw, &menuEnter, nullptr},        {kLcs.menuScreen, &menuEnter, nullptr},
            {kLcs.menuMessage, &menuEnter, nullptr},     {kLcs.loading, &loadingEnter, nullptr},
            {kLcs.loadingIsland, &loadingEnter, nullptr}, {kLcs.radarDraw, &radarEnter, nullptr},
            {kLcs.afterRadar[0], &radarOver, nullptr},   {kLcs.afterRadar[1], &radarOver, nullptr},
            {kLcs.afterRadar[2], &radarOver, nullptr},   {kLcs.afterRadar[3], &radarOver, nullptr},
            {kLcs.afterRadar[4], &radarOver, nullptr},   {kLcs.afterRadar[5], &radarOver, nullptr},
            {kLcs.sprite[0], &afterRadarHook, nullptr},
            {kLcs.sprite[1], &afterRadarHook, nullptr},  {kLcs.rectCtor, &afterRadarHook, nullptr},
            {kLcs.printString, &printString, nullptr},   {kLcs.printLine, &printLine, nullptr},
            {kLcs.printChar, &printChar, nullptr},       {kLcs.drawFonts, &drawFonts, nullptr},
            {kLcs.uvSprite, &uvSprite, nullptr},
        };
        constexpr int kMenuHooks = 3, kLoadingHooks = 2; // the first entries of g_hooks

        template <int I>
        PS2_GUEST_CC void before(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_hooks[I].before(rdram, ctx);
            g_hooks[I].original(rdram, ctx, runtime);
        }

        template <int... I>
        constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> befores(std::integer_sequence<int, I...>)
        {
            return {&before<I>...};
        }

        bool envOff(const char *name)
        {
            const char *e = std::getenv(name);
            return e && e[0] == '0';
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (envOff("VCS_HUD_RW"))
            return;
        const Addrs &A = kLcs;
        // the functions as registered now (another feature's hook of the same address, if it went in first, chains)
        g_im2d = runtime.lookupFunction(A.im2d);
        g_im2dIndexed = runtime.lookupFunction(A.im2dIndexed);
        g_renderState = runtime.lookupFunction(A.renderState);
        g_hudDraw = runtime.lookupFunction(A.hudDraw);
        g_frameKick = runtime.lookupFunction(A.frameKick);
        g_rectFill = runtime.lookupFunction(A.rectFill);
        if (!g_im2d || !g_im2dIndexed || !g_renderState || !g_hudDraw || !g_frameKick || !g_rectFill)
        {
            game::logUnavailable("widescreen HUD (lcs_hud: a function is missing from the map)");
            return;
        }
        runtime.replaceFunction(A.im2d, &im2d);
        runtime.replaceFunction(A.im2dIndexed, &im2dIndexed);
        runtime.replaceFunction(A.renderState, &renderState);
        runtime.replaceFunction(A.hudDraw, &hudDraw);
        runtime.replaceFunction(A.frameKick, &frameKick);
        runtime.replaceFunction(A.rectFill, &rectFill);
        g_scopeFill = !envOff("VCS_SCOPE_FILL");

        constexpr int n = static_cast<int>(std::size(g_hooks));
        static constexpr auto trampolines = befores(std::make_integer_sequence<int, n>{});
        const bool menus = !envOff("VCS_MENU_NATIVE"), loading = !envOff("VCS_LOADING_NATIVE");
        int hooked = 0;
        for (int i = 0; i < n; ++i)
        {
            if ((i < kMenuHooks && !menus) || (i >= kMenuHooks && i < kMenuHooks + kLoadingHooks && !loading))
                continue;
            g_hooks[i].original = runtime.lookupFunction(g_hooks[i].address);
            if (!g_hooks[i].original)
            {
                std::fprintf(stderr, "[lcs-hud] %08x is missing from the map: not hooked\n", g_hooks[i].address);
                continue;
            }
            runtime.replaceFunction(g_hooks[i].address, trampolines[i]);
            ++hooked;
        }
        g_lines.reserve(1024);
        g_runs.reserve(512);
        // The native HUD layer: the HUD drawn over the picture at the window's size (VCS_HUD_NATIVE=0: into the frame
        // buffer, squeezed)
        vcs::hudEnable(true);
        std::fprintf(stderr, "[lcs-hud] HUD context, anchoring, native HUD layer; menus %d, loading screens %d, radar / text "
                             "by who draws them, scope side bars %d (%d hooks)\n",
                     menus ? 1 : 0, loading ? 1 : 0, g_scopeFill ? 1 : 0, hooked);
    }
}
