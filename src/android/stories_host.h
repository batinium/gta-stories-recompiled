// [stories] Both cities on Android: the one-process host in libmain.so (stories_host.cpp; docs/ANDROID.md "Both cities
// on Android")
#pragma once

namespace vcs_android
{
    // VCS_STORIES=1 (VCSActivity, a --stories build's "Both cities" row)
    bool storiesRequested();
    // argv[1] Liberty City's disc (in front), argv[2] Vice City's (behind); returns when the game in front ends
    int storiesMain(int argc, char *argv[]);
}
