// [lcs] Liberty City Stories' sound without the IOP (game/lcs_sound.cpp): game/vcs_sound.h's driver for LCS's build of
// CDSTREAM.IRX.
#pragma once

class PS2Runtime;

namespace lcs
{
    // Serves LCS's CDSTREAM.IRX sound servers 0x65686579 (effects) / 0x65686578 (music) on the host and makes the native
    // SPU2 the audio output. False with VCS_NATIVE_SOUND=0 (the emulated IOP's, the reference) or when the disc's module
    // is not the expected build.
    bool installSound(PS2Runtime &runtime);
}
