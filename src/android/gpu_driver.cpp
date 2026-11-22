// [android] Custom GPU drivers through libadrenotools (gpu_driver.h; docs/ANDROID.md "Custom GPU drivers").
// Built into vcs_rhi when VCS_GPU_DRIVERS is on (Android, not a both-cities guest).
#include "android/gpu_driver.h"

#include <adrenotools/driver.h>

#include <cerrno>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gpudriver
{
    namespace
    {
        std::once_flag g_once;
        void *g_proc = nullptr;
        std::string g_name;

        // a folder / file name the launcher made: no path parts, nothing hidden
        bool plainName(const std::string &s)
        {
            if (s.empty() || s.size() > 128 || s[0] == '.')
                return false;
            for (char c : s)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '-' || c == '+'))
                    return false;
            return true;
        }

        bool makeDir(const std::string &dir)
        {
            return ::mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST;
        }

        void open()
        {
            if (!selected())
            {
                std::fprintf(stderr, "[gpu-driver] system driver%s\n",
                             std::getenv("VCS_GPU_DRIVER") && *std::getenv("VCS_GPU_DRIVER") ? " (VCS_GPU_DRIVERS=0)" : "");
                return;
            }
            const std::string name = std::getenv("VCS_GPU_DRIVER");
            const char *libEnv = std::getenv("VCS_GPU_DRIVER_LIB");
            const char *appEnv = std::getenv("VCS_APP_DIR");
            const char *hookEnv = std::getenv("VCS_NATIVE_LIB_DIR");
            const std::string lib = libEnv ? libEnv : "";
            if (!plainName(name) || !plainName(lib) || !appEnv || !*appEnv || !hookEnv || !*hookEnv)
            {
                std::fprintf(stderr, "[gpu-driver] failed: bad selection (driver '%s', library '%s', app dir %s, native "
                                     "lib dir %s): using the system driver\n",
                             name.c_str(), lib.c_str(), appEnv ? "set" : "unset", hookEnv ? "set" : "unset");
                return;
            }
            const std::string root = std::string(appEnv) + "/gpu_drivers/";
            const std::string dir = root + name + "/"; // adrenotools appends the library's name to it as it is
            const std::string tmp = root + "tmp/";     // only used below API 29 (memfd otherwise): created anyway
            const std::string redirect = root + "redirect/";
            makeDir(root);
            makeDir(tmp);
            makeDir(redirect);
            struct stat st{};
            if (::stat((dir + lib).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            {
                std::fprintf(stderr, "[gpu-driver] failed: %s%s is missing: using the system driver\n", dir.c_str(), lib.c_str());
                return;
            }
            std::fprintf(stderr, "[gpu-driver] loading '%s' (%s, %lld KB) through adrenotools, hooks from %s\n", name.c_str(),
                         lib.c_str(), static_cast<long long>(st.st_size / 1024), hookEnv);
            std::fflush(stderr);
            // The file redirect sends the driver's own file writes (shader caches, debug dumps) to our private folder
            void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM | ADRENOTOOLS_DRIVER_FILE_REDIRECT,
                                                      tmp.c_str(), hookEnv, dir.c_str(), lib.c_str(), redirect.c_str(), nullptr);
            if (!handle)
            {
                const char *why = dlerror();
                std::fprintf(stderr, "[gpu-driver] failed: adrenotools_open_libvulkan returned nothing (%s): using the system "
                                     "driver\n",
                             why ? why : "no dlerror; see the hook_impl / linkernsbypass lines in logcat");
                return;
            }
            void *proc = dlsym(handle, "vkGetInstanceProcAddr");
            if (!proc)
            {
                std::fprintf(stderr, "[gpu-driver] failed: no vkGetInstanceProcAddr in the hooked libvulkan.so: using the system "
                                     "driver\n");
                return;
            }
            g_proc = proc;
            g_name = name;
            std::fprintf(stderr, "[gpu-driver] loaded '%s': the Vulkan backend uses it (the [rhi] gpu: lines below show the "
                                 "driver the device reports)\n",
                         name.c_str());
        }
    }

    void *load()
    {
        std::call_once(g_once, open);
        return g_proc;
    }

    const char *activeName()
    {
        return g_name.c_str();
    }
}
