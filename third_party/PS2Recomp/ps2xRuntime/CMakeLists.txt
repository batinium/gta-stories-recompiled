cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" ON)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    endif()

    # [gta-recomp] No raylib: the host platform layer (window, input, audio, Dear ImGui) comes from the parent
    # project as PS2X_HOST_PLATFORM_TARGET (gta-recomp: src/platform, SDL3). Dear ImGui is its `imgui` target.
    if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID AND NOT TARGET imgui)
        message(FATAL_ERROR "PS2X_ENABLE_DEBUG_UI needs an `imgui` target from the host platform layer")
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    if(NOT PS2X_HOST_PLATFORM_TARGET)
        message(FATAL_ERROR "Set PS2X_HOST_PLATFORM_TARGET to the host platform library (window, input, audio)")
    endif()
    target_link_libraries(ps2_host_backend INTERFACE ${PS2X_HOST_PLATFORM_TARGET})

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

# [gta-recomp] [android] FFmpeg on Android too (built from source below; it was off: the movies were stub frames)
set(PS2X_ENABLE_FFMPEG_DEFAULT ON)

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

# [gta-recomp] [android] Android: no system FFmpeg to take. A POSIX sh runs FFmpeg's configure (Windows hosts: Git for
# Windows' usr/bin, which also has the sed / awk / grep it calls; never WSL's bash.exe) and the NDK's own make builds it.
# Without a sh the build goes on without FFmpeg (stub frames), with a warning.
if(PS2X_ENABLE_FFMPEG AND PS2X_IS_ANDROID)
    if(CMAKE_HOST_WIN32)
        find_package(Git QUIET)
        set(_ffmpeg_sh_hints)
        if(GIT_EXECUTABLE)
            get_filename_component(_git_bin "${GIT_EXECUTABLE}" DIRECTORY) # <Git>/cmd or <Git>/mingw64/bin
            list(APPEND _ffmpeg_sh_hints "${_git_bin}/../usr/bin" "${_git_bin}/../../usr/bin")
        endif()
        find_program(PS2X_FFMPEG_SH sh.exe HINTS ${_ffmpeg_sh_hints} "C:/Program Files/Git/usr/bin" NO_DEFAULT_PATH)
    else()
        find_program(PS2X_FFMPEG_SH sh)
    endif()
    file(GLOB _ffmpeg_ndk_make "${CMAKE_ANDROID_NDK}/prebuilt/*/bin/make${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    find_program(PS2X_FFMPEG_MAKE NAMES make gmake HINTS ${_ffmpeg_ndk_make})
    if(_ffmpeg_ndk_make)
        list(GET _ffmpeg_ndk_make 0 PS2X_FFMPEG_MAKE)
    endif()
    if(NOT PS2X_FFMPEG_SH OR NOT PS2X_FFMPEG_MAKE)
        message(WARNING "FFmpeg for Android needs a POSIX sh (Windows: Git for Windows) and make "
                        "(sh: '${PS2X_FFMPEG_SH}', make: '${PS2X_FFMPEG_MAKE}'): building without it, the movies are stub frames")
        set(PS2X_ENABLE_FFMPEG OFF)
    endif()
endif()

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(PS2X_IS_ANDROID)
    # [gta-recomp] [android] (the parent's docs/ANDROID.md "Movies"): the macOS build's FFmpeg below - the same pinned
    # source release and the same minimal LGPL configuration (the MPEG-2 video decoder, its parser, swscale; static, no
    # network / hwaccel / autodetected libraries) - cross-built with the NDK's clang. Built once into
    # PS2X_FFMPEG_CACHE_DIR, keyed by version, ABI and options: the build trees that point there (Gradle's, tools/
    # android.py's) reuse it; the release tarball is kept there too. Minutes on a Windows host (configure's many forks).
    set(FFMPEG_SOURCE_VERSION "7.1.2")
    if(NOT FFMPEG_SOURCE_URL)
        set(FFMPEG_SOURCE_URL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_SOURCE_VERSION}.tar.xz")
    endif()
    set(PS2X_FFMPEG_CACHE_DIR "${CMAKE_BINARY_DIR}/ThirdParty" CACHE PATH "Where Android's FFmpeg is built once and kept")
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        set(_ffmpeg_arch aarch64)
        set(_ffmpeg_arch_options)
    elseif(CMAKE_ANDROID_ARCH_ABI STREQUAL "x86_64")
        set(_ffmpeg_arch x86_64)
        set(_ffmpeg_arch_options --disable-x86asm) # no nasm in the NDK
    else()
        message(FATAL_ERROR "FFmpeg for Android: no recipe for ABI ${CMAKE_ANDROID_ARCH_ABI} (-DPS2X_ENABLE_FFMPEG=OFF)")
    endif()
    set(_ffmpeg_target "${CMAKE_C_COMPILER_TARGET}")
    if(NOT _ffmpeg_target)
        set(_ffmpeg_target "${CMAKE_ANDROID_ARCH_TRIPLE}${CMAKE_SYSTEM_VERSION}")
    endif()
    set(_ffmpeg_options
        --target-os=android --arch=${_ffmpeg_arch} --enable-cross-compile ${_ffmpeg_arch_options}
        --enable-static --disable-shared --enable-pic
        --disable-everything --enable-decoder=mpeg2video --enable-parser=mpegvideo --enable-swscale
        --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample
        --disable-programs --disable-doc --disable-network --disable-autodetect --disable-debug
        --extra-cflags=--target=${_ffmpeg_target} --extra-ldflags=--target=${_ffmpeg_target}
        # configure insists on a working host C compiler; nothing in this configuration builds a host tool
        --host-cflags=--target=${_ffmpeg_target} --host-ldflags=--target=${_ffmpeg_target})
    string(SHA256 _ffmpeg_key "${FFMPEG_SOURCE_VERSION};${_ffmpeg_options}")
    string(SUBSTRING "${_ffmpeg_key}" 0 10 _ffmpeg_key)
    set(FFMPEG_INSTALL_DIR "${PS2X_FFMPEG_CACHE_DIR}/ffmpeg-${FFMPEG_SOURCE_VERSION}-${CMAKE_ANDROID_ARCH_ABI}-${_ffmpeg_key}")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_INSTALL_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_INSTALL_DIR}/lib")
    set(_ffmpeg_done "${FFMPEG_INSTALL_DIR}/.complete")
    file(MAKE_DIRECTORY "${FFMPEG_INCLUDE_DIR}")
    if(EXISTS "${_ffmpeg_done}")
        message(STATUS "FFmpeg ${FFMPEG_SOURCE_VERSION} for ${CMAKE_ANDROID_ARCH_ABI}: ${FFMPEG_INSTALL_DIR} (built before)")
    else()
        message(STATUS "FFmpeg ${FFMPEG_SOURCE_VERSION} for ${CMAKE_ANDROID_ARCH_ABI}: built into ${FFMPEG_INSTALL_DIR}")
        include(ExternalProject)
        include(ProcessorCount)
        ProcessorCount(_ffmpeg_jobs)
        if(_ffmpeg_jobs EQUAL 0)
            set(_ffmpeg_jobs 4)
        endif()
        get_filename_component(_ffmpeg_sh_dir "${PS2X_FFMPEG_SH}" DIRECTORY)
        # the sh's folder first on PATH: make (the NDK's, a native program on Windows) runs its recipes through it
        set(_ffmpeg_env ${CMAKE_COMMAND} -E env --modify "PATH=path_list_prepend:${_ffmpeg_sh_dir}" --)
        ExternalProject_Add(ffmpeg_external
            URL "${FFMPEG_SOURCE_URL}"
            URL_HASH SHA256=089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304
            DOWNLOAD_DIR "${PS2X_FFMPEG_CACHE_DIR}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            PREFIX "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix"
            # in the source tree: configure then names it "." (out of tree it writes the sh's own spelling of the path,
            # /e/... on Windows, which the native make cannot open)
            BUILD_IN_SOURCE TRUE
            CONFIGURE_COMMAND ${_ffmpeg_env} "${PS2X_FFMPEG_SH}" ./configure --prefix=${FFMPEG_INSTALL_DIR}
                --cc=${CMAKE_C_COMPILER} --host-cc=${CMAKE_C_COMPILER} --ar=${CMAKE_AR} --nm=${CMAKE_NM}
                --ranlib=${CMAKE_RANLIB} ${_ffmpeg_options}
            BUILD_COMMAND ${_ffmpeg_env} "${PS2X_FFMPEG_MAKE}" -j${_ffmpeg_jobs}
            INSTALL_COMMAND ${_ffmpeg_env} "${PS2X_FFMPEG_MAKE}" install
                COMMAND ${CMAKE_COMMAND} -E touch "${_ffmpeg_done}"
            BUILD_BYPRODUCTS
                "${FFMPEG_LIB_DIR}/libavcodec.a"
                "${FFMPEG_LIB_DIR}/libavutil.a"
                "${FFMPEG_LIB_DIR}/libswscale.a"
            LOG_CONFIGURE TRUE
            LOG_BUILD TRUE
            LOG_INSTALL TRUE
            LOG_OUTPUT_ON_FAILURE TRUE
        )
    endif()
    add_library(ffmpeg INTERFACE)
    foreach(_lib avcodec swscale avutil)  # in link order
        add_library(ffmpeg_${_lib} STATIC IMPORTED GLOBAL)
        set_target_properties(ffmpeg_${_lib} PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/lib${_lib}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
        if(TARGET ffmpeg_external)
            add_dependencies(ffmpeg_${_lib} ffmpeg_external)
        endif()
        target_link_libraries(ffmpeg INTERFACE ffmpeg_${_lib})
    endforeach()
    target_link_libraries(ffmpeg INTERFACE m)
elseif(APPLE AND NOT VCS_FFMPEG STREQUAL "system")
    # [gta-recomp] macOS (docs/MACOS.md): FFmpeg built from a pinned source release with only what the movies use
    # (the MPEG-2 video decoder, its parser, swscale) - static, LGPL, no Homebrew. A minute on an M1, once per build
    # tree. -DFFMPEG_SOURCE_URL=<local .tar.xz>: a release's bundled copy (deps/, tools/deps.py). VCS_FFMPEG=system
    # takes pkg-config's (e.g. Homebrew's) instead.
    include(ExternalProject)
    include(ProcessorCount)
    ProcessorCount(_ffmpeg_jobs)
    if(_ffmpeg_jobs EQUAL 0)
        set(_ffmpeg_jobs 4)
    endif()
    set(FFMPEG_SOURCE_VERSION "7.1.2")
    if(NOT FFMPEG_SOURCE_URL)
        set(FFMPEG_SOURCE_URL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_SOURCE_VERSION}.tar.xz")
    endif()
    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_INSTALL_DIR "${FFMPEG_PREFIX_DIR}/install")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_INSTALL_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_INSTALL_DIR}/lib")
    file(MAKE_DIRECTORY "${FFMPEG_INCLUDE_DIR}")
    # the compiler by full path (not xcrun's shim) needs the SDK spelled out (CMake 4 leaves CMAKE_OSX_SYSROOT empty);
    # SDKROOT for FFmpeg's host-compiler checks too
    set(_ffmpeg_sdk "${CMAKE_OSX_SYSROOT}")
    if(NOT _ffmpeg_sdk)
        execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE _ffmpeg_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    set(_ffmpeg_flags "-arch arm64 -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET} -isysroot ${_ffmpeg_sdk}")
    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_SOURCE_URL}"
        URL_HASH SHA256=089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env SDKROOT=${_ffmpeg_sdk} <SOURCE_DIR>/configure --prefix=${FFMPEG_INSTALL_DIR} --cc=${CMAKE_C_COMPILER}
            --arch=aarch64 --enable-static --disable-shared --enable-pic
            --disable-everything --enable-decoder=mpeg2video --enable-parser=mpegvideo --enable-swscale
            --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample
            --disable-programs --disable-doc --disable-network --disable-autodetect --disable-debug
            --extra-cflags=${_ffmpeg_flags} --extra-ldflags=${_ffmpeg_flags}
        BUILD_COMMAND ${CMAKE_COMMAND} -E env SDKROOT=${_ffmpeg_sdk} make -j${_ffmpeg_jobs}
        INSTALL_COMMAND ${CMAKE_COMMAND} -E env SDKROOT=${_ffmpeg_sdk} make install
        BUILD_BYPRODUCTS
            "${FFMPEG_LIB_DIR}/libavcodec.a"
            "${FFMPEG_LIB_DIR}/libavutil.a"
            "${FFMPEG_LIB_DIR}/libswscale.a"
        LOG_CONFIGURE TRUE
        LOG_BUILD TRUE
        LOG_INSTALL TRUE
        LOG_OUTPUT_ON_FAILURE TRUE
    )
    add_library(ffmpeg INTERFACE)
    foreach(_lib avcodec swscale avutil)  # in link order
        add_library(ffmpeg_${_lib} STATIC IMPORTED GLOBAL)
        set_target_properties(ffmpeg_${_lib} PROPERTIES
            IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/lib${_lib}.a"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
        add_dependencies(ffmpeg_${_lib} ffmpeg_external)
        target_link_libraries(ffmpeg INTERFACE ffmpeg_${_lib})
    endforeach()
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    # [gta-recomp] -DFFMPEG_PREBUILT_URL=<local zip>: a release's bundled copy (deps/, tools/deps.py), no download
    if(NOT FFMPEG_PREBUILT_URL)
        set(FFMPEG_PREBUILT_URL
            "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")
    endif()

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_disc_image.cpp
    src/lib/ps2_audio_out.cpp
    src/lib/ps2_perf.cpp
    src/lib/ps2_control.cpp
    src/lib/ps2_hle_survey.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/ps2_render_worker.cpp
    src/lib/ps2_write_watch.cpp
    src/lib/ps2_host_std.cpp
    src/lib/ps2_iop_native.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu_code_log.cpp
    src/lib/games_database.cpp
)

# [gta-recomp] GS on the graphics card: paraLLEl-GS (Vulkan compute, LGPLv3+). Integration ported from
# BigDaddyJRich/PS2Recomp (hctp branch, GPL-3.0). Without PS2X_PARALLEL_GS_DIR only the CPU renderer exists.
set(PS2X_PARALLEL_GS_DIR "" CACHE PATH "paraLLEl-GS source tree (optional Vulkan GS renderer)")
target_sources(ps2_runtime PRIVATE src/lib/gs/gs_gpu.cpp src/lib/gs/gs_hw.cpp)
if(PS2X_PARALLEL_GS_DIR AND EXISTS "${PS2X_PARALLEL_GS_DIR}/gs/gs_interface.hpp" AND WIN32)
    # paraLLEl-GS picks the static C runtime when none is set; everything else uses the DLL one
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
    add_subdirectory("${PS2X_PARALLEL_GS_DIR}" "${CMAKE_BINARY_DIR}/parallel-gs" EXCLUDE_FROM_ALL)
    target_sources(ps2_runtime PRIVATE src/lib/gs/gs_gpu_parallel.cpp)
    set_source_files_properties(src/lib/gs/gs_gpu_parallel.cpp PROPERTIES
        COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN;_SILENCE_CXX20_IS_POD_DEPRECATION_WARNING")
    target_link_libraries(ps2_runtime PRIVATE parallel-gs)
    target_compile_definitions(ps2_runtime PRIVATE PS2X_HAS_PARALLEL_GS=1)
    message(STATUS "paraLLEl-GS: ${PS2X_PARALLEL_GS_DIR}")
else()
    target_sources(ps2_runtime PRIVATE src/lib/gs/gs_gpu_none.cpp)
endif()

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)

# [gta-recomp] [android] The runtime / game split (the parent's docs/ANDROID.md): ps2EntryRunner (libmain.so) holds the
# runtime without the generated code, which is a shared library of its own (ps2GameLibrary, PS2X_GAME_LIBRARY_SOURCES)
# loaded at start; the runtime reaches its function table through ps2xAttachGameTable (ps2_runtime.h)
option(PS2X_SPLIT_RUNTIME "The runtime without the generated code (a game library loaded at start)" OFF)
set(PS2X_GAME_LIBRARY_SOURCES "" CACHE STRING "The game library's sources (PS2X_SPLIT_RUNTIME): the generated code")
set(PS2X_GAME_LIBRARY_SOURCE_DIR "" CACHE PATH "The generated code's folder (its headers), for the game library")
if(PS2X_SPLIT_RUNTIME)
    target_compile_definitions(ps2_runtime PRIVATE PS2X_SPLIT_RUNTIME=1)
endif()

# [gta-recomp] control channel sockets (src/lib/ps2_control.cpp)
if(WIN32)
    target_link_libraries(ps2_runtime PRIVATE ws2_32)
endif()

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

# [gta-recomp] Let a parent project supply the generated code, its own main()
# and extra sources instead of copying them into src/runner.
set(PS2X_RUNNER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/runner" CACHE PATH "Directory with generated runner sources")
set(PS2X_RUNNER_EXTRA_SOURCES "" CACHE STRING "Extra sources compiled into ps2EntryRunner")
set(PS2X_RUNNER_MAIN "" CACHE FILEPATH "Replacement main() source for ps2EntryRunner")

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${PS2X_RUNNER_SOURCE_DIR}/*.cpp"
)
list(APPEND RUNNER_SRC_FILES ${PS2X_RUNNER_EXTRA_SOURCES})

set(RUNNER_MAIN_CPP "${PS2X_RUNNER_SOURCE_DIR}/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(PS2X_RUNNER_MAIN)
    list(APPEND RUNNER_SRC_FILES "${PS2X_RUNNER_MAIN}")
elseif(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

# [gta-recomp] [stories] PS2X_RUNNER_SHARED: the runner as a shared library on the desktop too (the parent's
# one-process host loads two games' DLLs side by side; docs/PS2RECOMP_PATCHES.md)
option(PS2X_RUNNER_SHARED "Build ps2EntryRunner as a shared library (the parent project supplies the entry)" OFF)
if(PS2X_IS_ANDROID OR PS2X_RUNNER_SHARED)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    # [gta-recomp] [android] no NativeActivity: SDL3's Java activity loads the library and calls SDL_main (the parent
    # project's src/android/game_main.cpp); upstream's -Wl,--undefined=ANativeActivity_onCreate removed
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h"
        "${PS2X_RUNNER_SOURCE_DIR}/ps2_recompiled_functions.h"
        "${PS2X_RUNNER_SOURCE_DIR}/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)
target_include_directories(ps2EntryRunner PRIVATE "${PS2X_RUNNER_SOURCE_DIR}")

# [gta-recomp] [android] The split build's two libraries. libmain.so exports the whole runtime (the game library's code
# calls into it: every object of the runtime's archives, all exported), and the game library links against it.
if(PS2X_SPLIT_RUNTIME)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_SPLIT_RUNTIME=1)
    target_link_options(ps2EntryRunner PRIVATE "LINKER:--whole-archive" "$<TARGET_FILE:ps2_runtime>"
                        "$<TARGET_FILE:ps2_iop>" "LINKER:--no-whole-archive" "LINKER:--export-dynamic"
                        # DF_1_GLOBAL: the game library's lookups find this library first. Its own copies of the
                        # runtime's inline globals (g_ps2ScratchpadHost, the fast-access limits, the watches) then
                        # bind to the runtime's: loaded by System.loadLibrary, libmain.so is otherwise not in the
                        # global group, and the game library's own copy came first (2026-11-17: a null scratchpad)
                        "LINKER:-z,global")
    if(PS2X_GAME_LIBRARY_SOURCES)
        add_library(ps2GameLibrary SHARED ${PS2X_GAME_LIBRARY_SOURCES})
        set_target_properties(ps2GameLibrary PROPERTIES OUTPUT_NAME "vcs_game")
        if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
            set_target_properties(ps2GameLibrary PROPERTIES
                UNITY_BUILD ON
                UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
            )
        endif()
        if(PS2X_ENABLE_RUNNER_PCH)
            set(PS2X_GAME_PCH_HEADERS
                "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h")
            foreach(PS2X_GAME_GENERATED_HEADER IN ITEMS
                "${PS2X_GAME_LIBRARY_SOURCE_DIR}/ps2_recompiled_functions.h"
                "${PS2X_GAME_LIBRARY_SOURCE_DIR}/ps2_recompiled_stubs.h")
                if(EXISTS "${PS2X_GAME_GENERATED_HEADER}")
                    list(APPEND PS2X_GAME_PCH_HEADERS "${PS2X_GAME_GENERATED_HEADER}")
                endif()
            endforeach()
            list(APPEND PS2X_GAME_PCH_HEADERS
                "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h")
            target_precompile_headers(ps2GameLibrary PRIVATE ${PS2X_GAME_PCH_HEADERS})
        endif()
        target_include_directories(ps2GameLibrary PRIVATE "${PS2X_GAME_LIBRARY_SOURCE_DIR}")
        # the runtime's headers and definitions, not its code: that is libmain.so's (DT_NEEDED, resolved at load)
        target_link_libraries(ps2GameLibrary PRIVATE ps2EntryRunner "$<COMPILE_ONLY:ps2_runtime>")
        if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            EnableFastReleaseMode(ps2GameLibrary)
        endif()
    endif()
endif()

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

# [gta-recomp] marks the start of the game's link for tools/build.py's progress: Ninja prints a step when it ends, and
# with ThinLTO the link is the longest step of a build (the whole game is optimised and compiled to machine code there)
add_custom_command(TARGET ps2EntryRunner PRE_LINK
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/game_link.started"
    VERBATIM
)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

# [gta-recomp] [android] upstream's ps2_android_runtime.cpp (an empty touch-overlay stub) is not built: the parent project
# has the Android host (src/android)

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
