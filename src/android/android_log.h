// [android] Android has no console: stdout and stderr go nowhere. The port logs with std::fprintf(stderr, ...)
// everywhere (runtime, RHI, game), so both are piped into logcat under one tag instead (adb logcat -s vcs).
#pragma once

namespace vcs_android
{
    // Once, at the start of main (SDL_main). Lines are sent as they end; a line longer than the buffer is split.
    void redirectStdioToLogcat(const char *tag);
}
