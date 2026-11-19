// [lcs] Liberty City Stories' sound and movies (game/lcs_audio.cpp, game/lcs_movie.cpp): VCS's host-side players
// (game/vcs_sound.h, game/vcs_movie.h) bound to LCS's code. Installed from applyLcs (game/lcs_overrides.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs
{
    // The boot movies (RSNY, RSLEEDS, RSNORTH, DM.PSS) through VCS's FFmpeg player instead of being skipped.
    void installMovies(PS2Runtime &runtime);
    // The Audio settings' Music / Sound effects volume kept in step with LCS's own levels both ways
    // (game/lcs_volume.cpp): volumeDefaults in an LCS run after the settings are registered, before they are loaded
    // (LCS's own effects default); volumeFrame once a game frame on the EE thread (the native sound driver's voice
    // block); volumeHostFrame on the host frame (a pause-menu step into the setting).
    void volumeDefaults();
    void installVolume(PS2Runtime &runtime);
    void volumeFrame(uint8_t *rdram);
    void volumeHostFrame();
    // Everything the game's sound needs on this side, movies included. False when the sound is the emulated IOP's
    // (VCS_NATIVE_SOUND=0, or the disc's module is not the expected build).
    bool installAudio(PS2Runtime &runtime);
}
