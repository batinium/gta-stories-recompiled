// Dual world (prototype, off by default): Liberty City Stories and Vice City Stories running at the same time in two
// processes, shown as one world. The front game (the one being played) publishes its camera and clock through shared
// memory each frame; the background game puts its own camera there plus an offset (the other city's place across
// the sea), renders its world without the HUD and hands the picture (with its sky as alpha 0) back; the front game
// draws it where its own depth buffer stayed clear - its sky and beyond its far plane (runtime/gs/gs_dualworld.h).
//
// Switches (environment, both processes):
//   STORIES_DUALWORLD=front|back   the role (unset: nothing of this runs)
//   STORIES_DW_NAME=<name>         the shared memory's name (default GTAStoriesDualWorld): one pair per name
//   STORIES_DW_OFFSET=dx,dy,dz     background-game coordinates = front-game coordinates + offset (default -3800,0,6:
//                                  front = LCS, Vice City east of Portland, its sea at the same height)
//                                  (the z given is used as it is; with none the seas are lined up: z 6, see below)
//   STORIES_DW_SEAMATCH=0          no vertical offset (z 0): Vice City's sea (water z 6) then shows above Liberty City's
//                                  (water z 0) in the far picture; the default lifts the offset by 6 so they meet
//   STORIES_DW_HOLD=N|0            the GPU hand-over (one process): a frame whose picture is being written / held by the
//                                  other game draws the last good one again, up to N frames old (default 30; 0: off)
//   STORIES_DW_FRAMELOG=<file>     (tests) one line per composite in <file>.<time>: tag, picture seq, its camera's frame tag
//   STORIES_DW_SIZE=WxH            the background picture's size (default 960x672)
//   STORIES_DW_CLOCK=0             the background game's clock is not set from the front game's
//   STORIES_DW_REPROJECT=0         the background picture is drawn as it is, not reprojected into the front's newer
//                                  camera (it then trails the camera by a frame or two when it turns)
//   STORIES_DW_OVERSCAN=k          the background renders its view this much wider (tan; default 1.05) for reprojection
//   STORIES_DW_MISSIONFAIL=0       a mission running when the player crosses the border is not failed
// Control command `dualworld` (both roles): state, frame counters and costs.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace dualworld
{
    enum class Role
    {
        Off,
        Front,
        Back
    };
    Role role();

    // From applyLcs / applyVcs, before the run: reads the switches, opens the shared memory, sets the renderer seams.
    void install(PS2Runtime &runtime);
    // Game thread, once per game frame (the frame kick): front publishes its camera and clock; back takes the clock.
    void onGameFrame(uint8_t *rdram);
    // [vcs] Game thread, from the free camera's height-routine hook on the active CCam (vcs_freecam.cpp): in the
    // background role the camera is set to the front game's plus the offset; true = done (skip the game's routine).
    bool backCamera(uint8_t *rdram, uint32_t cam);
    // Main thread, each host frame (vcs_input.cpp): the window shown in front / hidden in the background, the front's
    // window place published for the next one shown.
    void onHostFrame();
    // The offset in use: background-game coordinates = front-game coordinates + offset (default -3800,0,0, so
    // LCS x = VCS x + 3800). Valid (the default) even when the role is Off.
    void offset(float out[3]);
}
