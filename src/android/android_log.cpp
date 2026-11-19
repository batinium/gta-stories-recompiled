// [android] stdout / stderr -> logcat (android_log.h), and a copy in files/logs/game.log for crash reports.
#include "android/android_log.h"

#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace vcs_android
{
    namespace
    {
        const char *g_tag = "vcs";
        int g_readFd = -1;
        std::FILE *g_file = nullptr;  // files/logs/game.log, or null
        size_t g_fileBytes = 0;
        constexpr size_t FILE_LIMIT = 16u << 20; // a flood of lines stops the copy, never the game
        std::atomic<bool> g_busy{false};     // the pump is between a read and its writes

        void toFile(const char *line)
        {
            if (!g_file)
                return;
            if (g_fileBytes > FILE_LIMIT)
            {
                std::fputs("[log] the log file is full: the rest is in logcat only\n", g_file);
                std::fclose(g_file);
                g_file = nullptr;
                return;
            }
            const size_t n = std::strlen(line);
            std::fwrite(line, 1, n, g_file);
            std::fputc('\n', g_file);
            std::fflush(g_file); // a crash may end the process at any line
            g_fileBytes += n + 1;
        }

        void emit(const char *line)
        {
            __android_log_write(ANDROID_LOG_INFO, g_tag, line);
            toFile(line);
        }

        void pump(int fd)
        {
            char buffer[4096];
            size_t used = 0;
            for (;;)
            {
                const ssize_t got = read(fd, buffer + used, sizeof(buffer) - 1 - used);
                if (got <= 0)
                    break;
                g_busy = true;
                used += static_cast<size_t>(got);
                size_t start = 0;
                for (size_t i = 0; i < used; ++i)
                {
                    if (buffer[i] != '\n')
                        continue;
                    buffer[i] = '\0';
                    emit(buffer + start);
                    start = i + 1;
                }
                if (start == 0 && used == sizeof(buffer) - 1) // one line longer than the buffer: send what there is
                {
                    buffer[used] = '\0';
                    emit(buffer);
                    used = 0;
                }
                else if (start > 0)
                {
                    std::memmove(buffer, buffer + start, used - start);
                    used -= start;
                }
                g_busy = false;
            }
        }

        // files/logs/game.log (VCS_APP_DIR, set by VCSActivity); the run before is kept as game.prev.log
        void openFile()
        {
            const char *app = std::getenv("VCS_APP_DIR");
            if (!app || !*app)
                return;
            const std::string dir = std::string(app) + "/logs";
            mkdir(dir.c_str(), 0700);
            const std::string path = dir + "/game.log";
            std::rename(path.c_str(), (dir + "/game.prev.log").c_str());
            g_file = std::fopen(path.c_str(), "w");
            if (!g_file)
                return;
            char when[64];
            const std::time_t now = std::time(nullptr);
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            std::fprintf(g_file, "[log] game process %d started %s\n", static_cast<int>(getpid()), when);
            std::fflush(g_file);
        }
    }

    void redirectStdioToLogcat(const char *tag)
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        g_tag = tag;
        openFile();
        int fds[2];
        if (pipe(fds) != 0)
            return;
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        std::setvbuf(stderr, nullptr, _IOLBF, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        g_readFd = fds[0];
        std::thread(pump, fds[0]).detach();
    }
}

// The crash handler (src/debug/crash.cpp) calls this before the process ends, so its report reaches logcat and the
// file: waits (at most ~1 s) until the pump has read and written everything in the pipe.
extern "C" void vcs_android_log_drain()
{
    using vcs_android::g_busy;
    using vcs_android::g_readFd;
    if (g_readFd < 0)
        return;
    std::fflush(stdout);
    std::fflush(stderr);
    int idle = 0; // twice idle in a row: the pump may be between its read and setting g_busy
    for (int i = 0; i < 100 && idle < 2; ++i)
    {
        int pending = 0;
        if (ioctl(g_readFd, FIONREAD, &pending) != 0)
            return;
        idle = pending == 0 && !g_busy ? idle + 1 : 0;
        usleep(10000);
    }
}
