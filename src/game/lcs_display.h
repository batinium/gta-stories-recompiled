// [lcs] Liberty City Stories' side of the display settings (game/vcs_display.h): Hor+ / Vert+ through its camera's
// view window, the entity culling planes widened to match, its own (anamorphic) widescreen preference kept off and its
// cutscene letterbox following Display > Cutscene black bars. The settings themselves (display.aspect, display.hud,
// display.cutscene_bars) and the presenter / HUD squeeze are main.cpp's and the renderer's, shared with VCS.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs::display
{
    // At start-up (applyLcs): wraps LCS's view-window setter, sphere test, aspect choice and letterbox draw; installs
    // the HUD's side too (game/lcs_hud.h).
    void install(PS2Runtime &runtime);
}
