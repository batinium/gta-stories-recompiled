// [lcs] Liberty City Stories' movies through VCS's player (game/vcs_movie.cpp): FFmpeg video, PCM sound into the audio
// output, Start / Cross skip where the game allows it, Display > Skip intro movies honoured.
#include "game/lcs_audio.h"
#include "game/vcs_movie.h"

#include <cstdint>

namespace lcs
{
    namespace
    {
        struct Addrs
        {
            uint32_t moviePlay; // CMoviePlayer::Play(path, 0, frames before a skip is allowed)
        };
        // Evidence: 0x1F3728 (called once at boot) calls the player's init 0x359F40, then 0x35A020 four times with
        // a0 = cdrom0:\MOVIES\RSNY.PSS;1 / RSLEEDS / RSNORTH / DM.PSS (strings 0x3B5A58..0x3B5AB8), a1 = 0, a2 = 0 / 10 /
        // 10 / 10, then the shutdown 0x35A000 - the shape of VCS's FUN_0021CA70 (init 0x40D7D8, Play 0x40D8B8 (path, 0,
        // n), shutdown 0x40D898). The PSS files have VCS's layout: MPEG-2 video in stream 0xE0, sound in private stream
        // 1 sub-stream 0xFF as SShd type 1 (16-bit PCM, 48 kHz, stereo, 512-byte interleave) - read from RSNY / DM.PSS.
        constexpr Addrs kLcs{0x0035A020u};
    }

    void installMovies(PS2Runtime &runtime) { vcs::installMovieHooks(runtime, kLcs.moviePlay); }
}
