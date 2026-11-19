// [lcs] Liberty City Stories' 2D at the RenderWare level for the widescreen HUD (game/vcs_hud.h does it for VCS): the
// HUD context (CHud::Draw to the frame kick), its primitives captured and anchored for HUD "Screen edges" and the native
// HUD layer, and the black bars that must reach the screen's top / bottom edge narrower than 4:3.
#pragma once

class PS2Runtime;

namespace lcs::hud
{
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
}
