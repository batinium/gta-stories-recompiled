// VCS movies (docs/NATIVE_PORT_ROADMAP.md milestone 5): STINGS.PSS and INTRO.PSS played natively.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    // Replaces CMoviePlayer::Play (0x40D8B8): the PSS file from the disc, demuxed here, MPEG-2 video decoded with
    // FFmpeg and shown by the presenter at 4:3, its PCM sound mixed into the audio output (the clock the video
    // follows). Start / Cross skip where the game allows it (the intro, not the logos).
    void installMovieHooks(PS2Runtime &runtime);
    // [lcs] The same player for another build of the engine (Liberty City Stories: game/lcs_movie.cpp): its Play,
    // called (path, 0, frames before a skip is allowed) as VCS's
    void installMovieHooks(PS2Runtime &runtime, uint32_t playAddress);

    // Display > Intro movies: skip both (the logos and the intro) when on. Any thread.
    void setSkipIntroMovies(bool skip);
}
