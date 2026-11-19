// [lcs] Liberty City Stories' sound (see game/lcs_audio.h).
#include "game/lcs_audio.h"

#include "game/lcs_sound.h"

namespace lcs
{
    bool installAudio(PS2Runtime &runtime)
    {
        installMovies(runtime);
        if (!installSound(runtime))
            return false;
        installVolume(runtime); // its per-frame step comes from the native driver
        return true;
    }
}
