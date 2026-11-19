// VCS 2D at the RenderWare level (vcs_hud.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;
struct R5900Context; // [lcs]

namespace vcs
{
    void installHudHooks(PS2Runtime &runtime);
    void onFrameKick(); // the frame's display list goes out: the HUD context ends
    // Widescreen, HUD "Screen edges": each HUD element squeezed towards the screen edge it sits near (any thread)
    void setHudAnchoring(bool on);
    // Widescreen: the HUD's squeeze across ((4:3) / aspect; 1 = none, HUD "Stretch"), for the scope mask's side bars;
    // Vert+ (narrower than 4:3): its squeeze up and down (aspect / (4:3); 1 = none), for the mask's top / bottom bars
    // and the cutscene bars (any thread)
    void setHudSqueeze(float across, float down = 1.0f);
    uint32_t hudDrawCount(); // CHud::Draw calls drawn so far (the trainer's Hide HUD skips them)
    // [android] The player's HUD layout (the on-screen layout editor): part 0 the radar, 1 the top-right cluster, moved
    // by (dx, dy) in the picture's 640x448 units and scaled about its corner (any thread; 0, 0, 1: as the game draws it)
    void setHudPart(int part, float dx, float dy, float scale);

    // [lcs] Another game's 2D hooks (game/lcs_hud.cpp) drive this file's HUD context, anchoring (HUD "Screen edges")
    // and native HUD layer at that game's addresses; game thread unless noted. hudEnable once at start-up (in place of
    // installHudHooks); onFrameKick above at that game's frame kick.
    enum class HudContext
    {
        Hud,     // CHud::Draw's entry: everything to the frame kick is HUD
        Menu,    // the frontend's draw: laid out as a whole, kept in the middle
        Loading, // the loading screen: like a menu
        Radar,   // the radar's draw starts (anchored to the left edge)
        NoRadar, // the radar's draw is over
    };
    void hudEnter(HudContext c);
    // What a run of an Im2D call's vertices is, for a game that batches several things into one call (LCS's font
    // draws all buffered text lines in one call): Other is placed by where its element sits
    enum class HudRunPart : uint8_t
    {
        Other,
        Radar,       // drawn by the radar: the left edge
        CentredText, // a line centred across the screen: the middle
    };
    struct HudRun
    {
        uint32_t firstVertex; // the run goes to the next run's first vertex (or the call's last)
        HudRunPart part;
    };
    // before RwIm2DRenderPrimitive / ..Indexed; runs (in vertex order; nullptr: the call is one primitive) split it
    void hudIm2D(uint8_t *rdram, R5900Context *ctx, bool indexed, const HudRun *runs = nullptr, uint32_t runCount = 0);
    void hudNextPart(HudRunPart part); // the next Im2D call (a primitive of its own) is that part
    // a line centred across the screen moves to the screen's middle: HUD "Screen edges", in the HUD (not a menu)
    bool hudCentreText();
    void hudRenderState(uint32_t state, uint32_t value);           // before RwRenderStateSet
    void hudTallBar(uint8_t *rdram, R5900Context *ctx);            // before a top / bottom black bar's rect fill (Vert+)
    // before a scope mask's side bar's rect fill (rect a0 in 480x272): widened to the screen edge on its side
    void hudMaskSideBar(uint8_t *rdram, R5900Context *ctx, bool left);
    void hudEnable(bool nativeLayer); // the native HUD layer's default (VCS_HUD_NATIVE=0 / 1 decides when set)
}
