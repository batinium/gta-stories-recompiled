// [android] stdout / stderr -> logcat (android_log.h).
#include "android/android_log.h"

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace vcs_android
{
    namespace
    {
        const char *g_tag = "vcs";

        void pump(int fd)
        {
            char buffer[4096];
            size_t used = 0;
            for (;;)
            {
                const ssize_t got = read(fd, buffer + used, sizeof(buffer) - 1 - used);
                if (got <= 0)
                    break;
                used += static_cast<size_t>(got);
                size_t start = 0;
                for (size_t i = 0; i < used; ++i)
                {
                    if (buffer[i] != '\n')
                        continue;
                    buffer[i] = '\0';
                    __android_log_write(ANDROID_LOG_INFO, g_tag, buffer + start);
                    start = i + 1;
                }
                if (start == 0 && used == sizeof(buffer) - 1) // one line longer than the buffer: send what there is
                {
                    buffer[used] = '\0';
                    __android_log_write(ANDROID_LOG_INFO, g_tag, buffer);
                    used = 0;
                }
                else if (start > 0)
                {
                    std::memmove(buffer, buffer + start, used - start);
                    used -= start;
                }
            }
        }
    }

    void redirectStdioToLogcat(const char *tag)
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        g_tag = tag;
        int fds[2];
        if (pipe(fds) != 0)
            return;
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::setvbuf(stderr, nullptr, _IOLBF, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        std::thread(pump, fds[0]).detach();
    }
}
