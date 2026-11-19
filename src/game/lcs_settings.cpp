// [lcs] See lcs_settings.h. Settings that are being ported by their own LCS work (frame rate and timing, sound and
// movies, the trainer and free camera, free aim, saves) are left as they are, not on this list.
#include "game/lcs_settings.h"

#include "game/lcs_drawdist.h"
#include "game/lcs_swim.h"

#include "settings/settings.h"

#include "runtime/gs/gs_hw.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace lcs::settings_menu
{
    namespace
    {
        struct VcsOnly
        {
            const char *key;
            const char *why; // after "Vice City Stories only for now: "
        };
        const VcsOnly kVcsOnly[] = {
            // button prompts: ported as text (lcs_prompts.cpp: LCS's key expander writes the controls' names)
            // traffic / pedestrian density: ported (lcs_population.cpp)
            // vcs_driveby.cpp paces VCS's NPC drive-by (FUN_00216768 / FUN_002168E8), which calls CWeapon::Fire once a
            // frame. LCS has no such path: its CWeapon::Fire (0x2A27A0) is called only by CPed::Attack (0x1BC4A0, ped
            // state 16, on foot: paced by the attack animation) and the player's 0x349DC8 / 0x34A178; FireFromCar
            // (0x2A30F0) only by the player's three drive-bys (pad reads); FireInstantHit (0x2A4BF8) also by the script
            // command 0x546 (a car's gun, 150 ms apart) and a vehicle's own gun. Nothing fires once a frame to pace
            {"enhance.driveby_rate", "LCS's people in cars don't fire once a frame, nothing to change."},
        };

        // LCS's colour filter is the trails pass (game-internals.md 6p): each frame is downsampled into a 512x448 buffer
        // (FBP 384), blended back at FIX 32 (25%) and added back tinted at FIX 128 (the timecycle's post colour, ~7-36%
        // per channel) - so a third or more of every pixel comes from a 512-wide copy, and above 1x the picture looks
        // soft and washed next to VCS, whose trails blend at FIX ~8 with a faint tint. Internal resolution (default)
        // runs that buffer at the frame's resolution (gsHwSetPostFullResolution): the same tint and blend, the copy as
        // sharp as the picture. Original keeps the PS2's 512-wide buffer. LCS_POST_RES=0|1 for one run.
        void registerColourFilter()
        {
            settings::Spec s;
            s.key = "display.lcs_filter_res";
            s.page = "Graphics";
            s.section = "Image quality";
            s.label = "Colour filter resolution";
            s.help = "Liberty City Stories' colour filter blends a copy of the picture back over it. Internal resolution "
                     "draws that copy as sharp as the picture; Original (PS2) draws it at 512x448, softer above 1x.";
            s.type = settings::Type::Choice;
            s.choices = {{"Original (PS2)", 0}, {"Internal resolution", 1}};
            s.defaultValue = 1;
            s.env = "LCS_POST_RES";
            s.onChange = [](int v) { gsHwSetPostFullResolution(v != 0); };
            settings::add(s);
        }
    }

    void lockVcsOnly()
    {
        for (const VcsOnly &v : kVcsOnly)
            settings::lockForRun(v.key, std::string("Vice City Stories only for now: ") + v.why);
        std::fprintf(stderr, "[lcs] %zu Vice City Stories settings greyed out\n", sizeof(kVcsOnly) / sizeof(kVcsOnly[0]));
        lcs::drawdist::registerSettings(); // the draw distance's longer range, fog / far plane, model streaming
        registerColourFilter();
        lcs::swim::registerSettings(); // Enhancements > Gameplay > Swimming (game/lcs_swim.h)
        // lcs_framerate.cpp: CTimer's minimum step lowered to 0.2 (240 fps) by the host Update; 0.5 (100) without it
        if (const char *v = std::getenv("LCS_HOST_TIMER"); v && v[0] == '0')
            settings::noteForRun("display.fps", "Liberty City Stories: at most 100 (its shortest time step).");
    }
}
