// [android] Custom GPU drivers (docs/ANDROID.md "Custom GPU drivers"): a Vulkan driver the player imported in the
// launcher (e.g. Mesa Turnip for Adreno), loaded through libadrenotools instead of the phone's own.
//
// The launcher's GpuDriver.java extracts a driver package to files/gpu_drivers/<name>/ and VCSActivity sets, before the
// native code starts (smoke test and game alike):
//   VCS_GPU_DRIVER      the folder's name under files/gpu_drivers (empty / unset: the system driver)
//   VCS_GPU_DRIVER_LIB  the driver's library in it (meta.json's libraryName, e.g. libvulkan_freedreno.so)
//   VCS_NATIVE_LIB_DIR  the app's nativeLibraryDir (adrenotools' hook libraries: libmain_hook.so, libhook_impl.so, ...)
// VCS_GPU_DRIVERS=0 (files/env.txt) is the kill switch: the system driver whatever is selected.
//
// With a driver selected, the Vulkan backend takes vkGetInstanceProcAddr from the libvulkan.so copy adrenotools opens
// (volkInitializeCustom) and makes the window's surface itself (vkCreateAndroidSurfaceKHR through that copy): SDL's
// SDL_Vulkan_LoadLibrary only takes a path, and its loader copy would not know an instance of the custom one. The
// platform layer then makes the window without SDL_WINDOW_VULKAN, so SDL never loads the system loader (nor the stock
// driver behind it) in this process.
#pragma once

#include <cstdlib>
#include <cstring>

namespace gpudriver
{
    /** True when the launcher selected a custom driver for this run (cheap: the environment only) */
    inline bool selected()
    {
        const char *kill = std::getenv("VCS_GPU_DRIVERS");
        if (kill && std::strcmp(kill, "0") == 0)
            return false;
        const char *name = std::getenv("VCS_GPU_DRIVER");
        return name && *name;
    }

#if defined(VCS_GPU_DRIVERS)
    /** The selected driver's vkGetInstanceProcAddr (as void *), loaded on the first call; nullptr when none is selected or
     *  it could not be loaded (then the system driver is used, and the log says why). Thread-safe, loads once. */
    void *load();

    /** The selected driver's folder name, "" when the system driver is used (after load()) */
    const char *activeName();
#endif
}
