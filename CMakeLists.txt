cmake_minimum_required(VERSION 3.21)
project(vcs_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's own SLUS_215.90 (tools/recompile.py). Never committed.
set(VCS_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/ee" CACHE PATH "Recompiled EE code")
# The game the generated code is from: vcs, or lcs for Liberty City Stories' bring-up (tools/build.py --game lcs: its own
# build folder and generated code; main.cpp boots its ELF and none of VCS's game code runs)
set(VCS_GAME "vcs" CACHE STRING "The game: vcs, or lcs (bring-up)")
set_property(CACHE VCS_GAME PROPERTY STRINGS vcs lcs)

set(PS2X_LTO "THIN" CACHE STRING "Link-time optimisation for the game: THIN (default, the game to play), OFF (quick dev builds), FULL")
set(PS2X_PGO "" CACHE STRING "Profile-guided optimisation: empty (off), GEN (instrumented), or a .profdata path (tools/build.py --opt)")
set(PS2X_CPU "AVX2" CACHE STRING "The CPU the build runs on: AVX2 (Haswell / Zen and newer, the default), AVX (Sandy / Ivy Bridge, Bulldozer) or SSE4 (no AVX); tools/build.py picks it from this PC")
set_property(CACHE PS2X_CPU PROPERTY STRINGS AVX2 AVX SSE4)
option(VCS_BUILD_TOOLS "Build the PS2Recomp recompiler/analyzer" ON)
# [android] The recompiler and analyzer are host tools: an Android build (the NDK's toolchain, android/) never makes them
if(ANDROID)
    set(VCS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
endif()

set(PS2X_BUILD_RECOMP ${VCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER ${VCS_BUILD_TOOLS} CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

# Linux (docs/LINUX.md) and Windows' llvm-mingw (DECISIONS.md 2026-11-03): clang 19+ only - the same compiler as
# clang-cl, and PS2_GUEST_CC needs its preserve_none. Linked with lld (ThinLTO).
# macOS (docs/MACOS.md): Apple's clang (AppleClang 17+ is LLVM 19) and Apple's linker (ld64; Xcode has no lld).
if(APPLE)
    enable_language(OBJC OBJCXX) # the Metal backend (src/rhi/metal)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
        message(FATAL_ERROR "The game needs Apple clang 17 or newer (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}): "
                            "the Xcode Command Line Tools (xcode-select --install); python3 tools/build.py picks it up")
    endif()
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-Werror")
    check_cxx_source_compiles("__attribute__((preserve_none)) void f(); int main() { return 0; }" VCS_HAVE_PRESERVE_NONE)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT VCS_HAVE_PRESERVE_NONE)
        message(FATAL_ERROR "This clang has no preserve_none calling convention (PS2_GUEST_CC): update the Command Line Tools")
    endif()
elseif(NOT MSVC)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
        message(FATAL_ERROR "The game needs clang 19 or newer (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). "
                            "python tools/build.py picks it up (docs/LINUX.md; Windows: llvm-mingw, downloaded)")
    endif()
    add_link_options(-fuse-ld=lld)
endif()
# llvm-mingw: mingw-w64's headers default to Windows 7, the Windows SDK's to Windows 10 - the same APIs as clang-cl's
# build. Every program is linked -static: libc++, libunwind and winpthread inside, only the system's UCRT left.
if(MINGW)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
    add_link_options(-static)
endif()
# Linux and macOS: compile and ThinLTO jobs follow the memory - a unity batch of generated code takes up to ~2 GB to compile.
if(NOT WIN32)
    cmake_host_system_information(RESULT _vcs_mem_mb QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT _vcs_cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _vcs_jobs "${_vcs_mem_mb} / 2048")
    if(_vcs_jobs LESS 1)
        set(_vcs_jobs 1)
    endif()
    if(_vcs_jobs GREATER _vcs_cores)
        set(_vcs_jobs ${_vcs_cores})
    endif()
    set(VCS_COMPILE_JOBS ${_vcs_jobs} CACHE STRING "Parallel compile jobs (default: one per 2 GB of memory, at most one per core)")
    set(PS2X_LTO_JOBS ${_vcs_jobs} CACHE STRING "Parallel ThinLTO backend jobs at the game's link")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS vcs_compile=${VCS_COMPILE_JOBS})
    set(CMAKE_JOB_POOL_COMPILE vcs_compile)
    message(STATUS "${CMAKE_SYSTEM_NAME} build: ${VCS_COMPILE_JOBS} compile jobs, ${PS2X_LTO_JOBS} ThinLTO jobs (${_vcs_mem_mb} MB memory)")
endif()

set(VCS_HAVE_GENERATED OFF)
if(EXISTS "${VCS_GENERATED_DIR}/register_functions.cpp")
    set(VCS_HAVE_GENERATED ON)
endif()
# [android] The runtime / game split (docs/ANDROID.md): by default libmain.so (the APK) is SDL, the runtime and our code
# only, built with or without generated/, and the generated code is the player's own libvcs_game.so
# (VCS_ANDROID_GAME_LIBRARY, tools/android.py build-game), loaded at start. VCS_ANDROID_BUNDLE_GAME: the personal build,
# the game inside libmain.so as before (tools/android.py build --personal; never share that APK).
option(VCS_ANDROID_BUNDLE_GAME "Android: the generated code inside libmain.so (a personal APK, never shared)" OFF)
option(VCS_ANDROID_GAME_LIBRARY "Android: also build the game library libvcs_game.so from generated/" OFF)
# [stories] Both cities on Android (docs/ANDROID.md "Both cities on Android"): the game as libstories_<game>.so, a whole
# runtime with the generated code inside and everything local but its entry (src/stories/game_so.cpp / game_so.map), for
# the one-process host in libmain.so (src/android/stories_host.cpp). Built by tools/android.py build-game --stories in a
# tree of its own; a personal build (game code inside).
option(VCS_ANDROID_STORIES_GUEST "Android: the game as libstories_<game>.so for the both-cities host" OFF)
set(VCS_SPLIT OFF)
set(VCS_BUILD_GAME_RUNNER ${VCS_HAVE_GENERATED})
if(ANDROID AND VCS_ANDROID_STORIES_GUEST)
    if(NOT VCS_HAVE_GENERATED)
        message(FATAL_ERROR "VCS_ANDROID_STORIES_GUEST needs the generated code in ${VCS_GENERATED_DIR}")
    endif()
    if(VCS_ANDROID_BUNDLE_GAME OR VCS_ANDROID_GAME_LIBRARY)
        message(FATAL_ERROR "VCS_ANDROID_STORIES_GUEST is a build of its own (not with the personal build or a game library)")
    endif()
elseif(ANDROID)
    if(VCS_ANDROID_BUNDLE_GAME)
        if(NOT VCS_HAVE_GENERATED)
            message(FATAL_ERROR "VCS_ANDROID_BUNDLE_GAME (the personal build) needs the generated code in ${VCS_GENERATED_DIR}")
        endif()
        if(VCS_ANDROID_GAME_LIBRARY)
            message(FATAL_ERROR "VCS_ANDROID_GAME_LIBRARY is for the split build, not with VCS_ANDROID_BUNDLE_GAME")
        endif()
    else()
        set(VCS_SPLIT ON)
        set(VCS_BUILD_GAME_RUNNER ON)
        include("${CMAKE_SOURCE_DIR}/src/android/game_abi.cmake")
        if(VCS_ANDROID_GAME_LIBRARY AND NOT VCS_HAVE_GENERATED)
            message(FATAL_ERROR "VCS_ANDROID_GAME_LIBRARY: no generated code in ${VCS_GENERATED_DIR} "
                                "(python3 tools/build.py --recompile <your iso> first)")
        endif()
    endif()
endif()
# [stories] Both cities in one process (docs/STORIES.md "One process"; Windows only, opt-in): the game as a DLL
# (<game>_game.dll, entry src/stories/game_dll.cpp) instead of <game>.exe, for the host stories.exe (src/stories/host.cpp,
# built in the same tree). Off: nothing changes.
option(VCS_GAME_DLL "Windows: build the game as <game>_game.dll for the one-process host stories.exe" OFF)
if(VCS_GAME_DLL AND (NOT WIN32 OR ANDROID))
    message(FATAL_ERROR "VCS_GAME_DLL is for Windows desktop builds")
endif()
set(PS2X_RUNNER_SHARED ${VCS_GAME_DLL} CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ${VCS_BUILD_GAME_RUNNER} CACHE BOOL "" FORCE)
set(PS2X_SPLIT_RUNTIME ${VCS_SPLIT} CACHE BOOL "" FORCE)

file(GLOB_RECURSE VCS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM VCS_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")
# The platform layer, RHI and renderer are their own libraries (src/*/CMakeLists.txt), linked into the runtime.
list(FILTER VCS_SOURCES EXCLUDE REGEX "/src/(platform|rhi|render)/")
# [android] src/android: the Android-only pieces (logcat, the smoke test), added below for Android builds only
list(FILTER VCS_SOURCES EXCLUDE REGEX "/src/android/")
# [stories] src/stories: the one-process host (its own target) and the game DLL's entry, added below with VCS_GAME_DLL only
list(FILTER VCS_SOURCES EXCLUDE REGEX "/src/stories/")
# VU1 microprograms compiled to C++ by tools/vu1_recomp.py (from the user's ELF; see game/vcs/vu1_uploads.txt)
# (next to the EE code: generated/vu1 for generated/ee)
get_filename_component(VCS_GENERATED_ROOT "${VCS_GENERATED_DIR}" DIRECTORY)
set(VCS_VU1_COMPILED "${VCS_GENERATED_ROOT}/vu1/vu1_compiled.cpp")
if(EXISTS "${VCS_VU1_COMPILED}" AND NOT VCS_SPLIT)
    list(APPEND VCS_SOURCES "${VCS_VU1_COMPILED}")
endif()

if(VCS_SPLIT)
    # [android] libmain.so without the generated code: an empty folder in its place (nothing to glob, no header)
    set(_vcs_no_generated "${CMAKE_BINARY_DIR}/vcs_no_generated")
    file(MAKE_DIRECTORY "${_vcs_no_generated}")
    set(PS2X_RUNNER_SOURCE_DIR "${_vcs_no_generated}" CACHE PATH "" FORCE)
    # the game library: every generated file, the compiled VU1 code and its interface stamp
    set(_vcs_game_sources "")
    if(VCS_ANDROID_GAME_LIBRARY)
        file(GLOB _vcs_game_sources CONFIGURE_DEPENDS "${VCS_GENERATED_DIR}/*.cpp")
        if(EXISTS "${VCS_VU1_COMPILED}")
            list(APPEND _vcs_game_sources "${VCS_VU1_COMPILED}")
        endif()
        list(APPEND _vcs_game_sources "${CMAKE_SOURCE_DIR}/src/android/game_library_stamp.cpp")
    endif()
    set(PS2X_GAME_LIBRARY_SOURCES "${_vcs_game_sources}" CACHE STRING "" FORCE)
    set(PS2X_GAME_LIBRARY_SOURCE_DIR "${VCS_GENERATED_DIR}" CACHE PATH "" FORCE)
else()
    set(PS2X_RUNNER_SOURCE_DIR "${VCS_GENERATED_DIR}" CACHE PATH "" FORCE)
    set(PS2X_GAME_LIBRARY_SOURCES "" CACHE STRING "" FORCE)
endif()
set(PS2X_RUNNER_EXTRA_SOURCES "${VCS_SOURCES}" CACHE STRING "" FORCE)
set(PS2X_RUNNER_MAIN "${CMAKE_SOURCE_DIR}/src/main.cpp" CACHE FILEPATH "" FORCE)

set(PS2X_PARALLEL_GS_DIR "${CMAKE_SOURCE_DIR}/third_party/parallel-gs" CACHE PATH "" FORCE)
# [android] The APK's native libraries (docs/ANDROID.md), loaded one per run by SDL's Java activity (VCSActivity):
# libsmoke.so, the bring-up test on the platform layer, the Vulkan RHI and the presenter (always), and libmain.so, the
# game (below, once a disc is recompiled). SDL3 is linked whole into each: its JNI entry points are only reached from Java.
if(ANDROID)
    # [android] Custom GPU drivers (docs/ANDROID.md): libadrenotools, built into vcs_rhi (src/rhi/CMakeLists.txt). Not in
    # a both-cities guest (its host's libmain.so loads Vulkan). The player brings the driver; none is built or shipped.
    if(NOT VCS_ANDROID_STORIES_GUEST)
        option(VCS_GPU_DRIVERS "Android: custom Vulkan drivers through libadrenotools (Adreno)" ON)
    else()
        set(VCS_GPU_DRIVERS OFF)
    endif()
    if(VCS_GPU_DRIVERS)
        include(src/android/adrenotools.cmake)
    endif()
    add_subdirectory(src/platform)
    add_subdirectory(src/rhi)
    add_subdirectory(src/render)
    add_library(smoke SHARED src/android/smoke_main.cpp src/android/android_log.cpp)
    target_include_directories(smoke PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(smoke PRIVATE vcs_render vcs_platform "$<LINK_LIBRARY:WHOLE_ARCHIVE,SDL3::SDL3-static>" android log)
endif()
if(VCS_BUILD_GAME_RUNNER)
    if(NOT ANDROID) # [android] added above
        add_subdirectory(src/platform)
        add_subdirectory(src/rhi)
        add_subdirectory(src/render)
    endif()
    add_library(vcs_host INTERFACE)
    target_link_libraries(vcs_host INTERFACE vcs_platform vcs_render)
    set(PS2X_HOST_PLATFORM_TARGET vcs_host CACHE STRING "" FORCE)
endif()
add_subdirectory(third_party/PS2Recomp)

if(VCS_BUILD_GAME_RUNNER)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "${VCS_GAME}")
    # [stories] the game as a DLL (VCS_GAME_DLL): main.cpp's main renamed (as on Android), game_dll.cpp's entry exported,
    # every getenv / _putenv_s of the DLL through game_dll.cpp's per-game environment overlay; and the host exe
    if(VCS_GAME_DLL)
        set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "${VCS_GAME}_game" PREFIX "")
        target_sources(ps2EntryRunner PRIVATE src/stories/game_dll.cpp)
        set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/main.cpp" TARGET_DIRECTORY ps2EntryRunner
                     APPEND PROPERTY COMPILE_DEFINITIONS "main=vcs_game_main")
        set_source_files_properties(src/stories/game_dll.cpp TARGET_DIRECTORY ps2EntryRunner
                                    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
        target_compile_definitions(ps2EntryRunner PRIVATE VCS_GAME_DLL=1)
        target_link_options(ps2EntryRunner PRIVATE "-Wl,--wrap=getenv" "-Wl,--wrap=_putenv_s" "-Wl,--wrap=_Exit"
                           "-Wl,--wrap=__stdio_common_vfprintf" "-Wl,--wrap=fwrite" "-Wl,--wrap=fputs" "-Wl,--wrap=puts"
                           "-Wl,--wrap=fputc")
        add_executable(stories WIN32 src/stories/host.cpp)
        target_compile_definitions(stories PRIVATE UNICODE _UNICODE)
        target_link_options(stories PRIVATE -municode)
        target_link_libraries(stories PRIVATE shell32)
        if(VCS_GAME_DIR)
            set_target_properties(stories PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${VCS_GAME_DIR}")
        endif()
    endif()
    # The main build tree (build/release) puts vcs.exe, its DLLs and vcs.ini at the top of build/ instead of
    # build/release/third_party/PS2Recomp/ps2xRuntime; other trees (PGO training's build/pgo-gen) keep theirs.
    # VCS_GAME_DIR: the build folder chosen elsewhere (tools/toolchain.py passes it for the main tree; docs/LAUNCHER.md).
    set(VCS_GAME_DIR "" CACHE PATH "Where vcs.exe, its DLLs and vcs.ini go (empty: build/ for build/release)")
    string(TOLOWER "${CMAKE_BINARY_DIR}" _vcs_binary_dir)
    string(TOLOWER "${CMAKE_SOURCE_DIR}/build/release" _vcs_main_tree)
    if(VCS_GAME_DIR)
        set_target_properties(ps2EntryRunner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${VCS_GAME_DIR}")
    elseif(_vcs_binary_dir STREQUAL _vcs_main_tree)
        set_target_properties(ps2EntryRunner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
    endif()
    target_include_directories(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # [android] the game as libmain.so: SDL's activity calls SDL_main (src/android/game_main.cpp), which runs main.cpp's
    # main, renamed vcs_game_main there (kept out of the unity batches, so the rename stays in it)
    if(ANDROID)
        set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "main")
        # (side_panel.cpp: the side panel's settings and actions, game-only: the smoke test has no settings)
        if(VCS_ANDROID_STORIES_GUEST)
            # [stories] the guest: no SDL_main, no logcat pipe, no SDL (the host's); its own side panel and cooling copies
            # (local); the entry and the per-game environment (game_so.cpp); one export (game_so.map)
            set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "stories_${VCS_GAME}")
            target_sources(ps2EntryRunner PRIVATE src/stories/game_so.cpp src/android/side_panel.cpp src/android/cooling.cpp)
            set_source_files_properties(src/stories/game_so.cpp TARGET_DIRECTORY ps2EntryRunner
                                        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
        else()
        target_sources(ps2EntryRunner PRIVATE src/android/game_main.cpp src/android/android_log.cpp src/android/side_panel.cpp
                                              src/android/cooling.cpp)
        endif()
        # (source properties belong to a directory: the target's, third_party/PS2Recomp/ps2xRuntime)
        set_source_files_properties(src/main.cpp TARGET_DIRECTORY ps2EntryRunner PROPERTIES
                                    COMPILE_DEFINITIONS "main=vcs_game_main" SKIP_UNITY_BUILD_INCLUSION ON)
        set_source_files_properties(src/android/game_main.cpp src/android/android_log.cpp src/android/side_panel.cpp
                                    src/android/cooling.cpp TARGET_DIRECTORY ps2EntryRunner
                                    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        if(VCS_ANDROID_STORIES_GUEST)
            target_link_libraries(ps2EntryRunner PRIVATE SDL3::Headers android log)
            # -z undefs after the NDK's --no-undefined: SDL_* and the JNI glue resolve to libmain.so at load
            # (tools/android.py checks the guest's undefined symbols against libmain.so's instead)
            target_link_options(ps2EntryRunner PRIVATE
                "-Wl,--version-script=${CMAKE_SOURCE_DIR}/src/stories/game_so.map" "-Wl,-z,undefs"
                "-Wl,--wrap=getenv" "-Wl,--wrap=setenv" "-Wl,--wrap=unsetenv"
                "-Wl,--wrap=fprintf" "-Wl,--wrap=vfprintf" "-Wl,--wrap=fputs")
            set_property(TARGET ps2EntryRunner APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/src/stories/game_so.map")
        else()
        target_link_libraries(ps2EntryRunner PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,SDL3::SDL3-static>" android log)
        endif()
        if(VCS_SPLIT)
            # [android] the split build: game_library.cpp loads the player's game library (game_main.cpp)
            target_sources(ps2EntryRunner PRIVATE src/android/game_library.cpp src/android/stories_host.cpp) # [stories]
            set_source_files_properties(src/android/stories_host.cpp TARGET_DIRECTORY ps2EntryRunner PROPERTIES
                                        SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON)
            set_source_files_properties(src/android/game_library.cpp TARGET_DIRECTORY ps2EntryRunner PROPERTIES
                                        SKIP_UNITY_BUILD_INCLUSION ON
                                        COMPILE_DEFINITIONS "VCS_GAME_HEADERS_HASH=\"${VCS_GAME_HEADERS_HASH}\"")
            if(TARGET ps2GameLibrary)
                target_include_directories(ps2GameLibrary PRIVATE "${CMAKE_SOURCE_DIR}/src")
                set_source_files_properties(src/android/game_library_stamp.cpp TARGET_DIRECTORY ps2GameLibrary PROPERTIES
                                            SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON
                                            COMPILE_DEFINITIONS "VCS_GAME_HEADERS_HASH=\"${VCS_GAME_HEADERS_HASH}\";VCS_GAME_ID=\"${VCS_GAME}\"")
            endif()
        endif()
    endif()
    # another game's bring-up (VCS_GAME): main.cpp boots its ELF (appended: Android sets main.cpp's definitions above)
    if(NOT VCS_GAME STREQUAL "vcs")
        string(TOUPPER "${VCS_GAME}" _vcs_game_upper)
        set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/main.cpp" TARGET_DIRECTORY ps2EntryRunner
                     APPEND PROPERTY COMPILE_DEFINITIONS "VCS_GAME_${_vcs_game_upper}=1")
        # VCS's hooks still compile (they never run): their calls to VCS's generated functions go by address
        # (game/game_functions.h), as in the Android split build - another game's code has none of those names
        set_property(SOURCE ${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp" TARGET_DIRECTORY ps2EntryRunner
                     APPEND PROPERTY COMPILE_DEFINITIONS "VCS_GAME_OTHER=1")
    endif()
    target_link_libraries(ps2EntryRunner PRIVATE vcs_host) # main.cpp: settings menu (Dear ImGui), presenter aspect
    # Windows: our manifest in place of the linker's, for the UTF-8 code page (non-ASCII folders; src/vcs.manifest)
    # ... and the exe's icon and version information (src/vcs.rc): VERSION in the repository root, as a header only
    # the resource file sees - a compile definition would rebuild every generated file at each new version
    if(WIN32)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" VCS_VERSION LIMIT_COUNT 1)
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _vcs_version_numbers "${VCS_VERSION}")
        if(NOT _vcs_version_numbers)
            message(FATAL_ERROR "VERSION must start with major.minor.patch (it holds '${VCS_VERSION}')")
        endif()
        file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/vcs_version/vcs_version.h" CONTENT "#define VCS_VERSION_MAJOR ${CMAKE_MATCH_1}
#define VCS_VERSION_MINOR ${CMAKE_MATCH_2}
#define VCS_VERSION_PATCH ${CMAKE_MATCH_3}
#define VCS_VERSION_TEXT \"${VCS_VERSION}\"
")
        target_sources(ps2EntryRunner PRIVATE "${CMAKE_SOURCE_DIR}/src/vcs.rc")
        # the resource compiler doesn't report the files it embeds
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/vcs.rc" TARGET_DIRECTORY ps2EntryRunner
            PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/vcs.manifest;${CMAKE_SOURCE_DIR}/src/vcs.ico;${CMAKE_SOURCE_DIR}/src/lcs.ico;${CMAKE_BINARY_DIR}/vcs_version/vcs_version.h"
                       INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/vcs_version")
        # [lcs] the icon and file names by game: lcs.exe gets src/lcs.ico (vcs.rc)
        if(VCS_GAME STREQUAL "lcs")
            set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/vcs.rc" TARGET_DIRECTORY ps2EntryRunner
                         APPEND PROPERTY COMPILE_DEFINITIONS "VCS_GAME_LCS=1")
        endif()
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE /MANIFEST:NO)  # lld in mingw mode makes none of its own
        endif()
    endif()
    # Our own sources aren't generated code; keep them out of the unity batches.
    set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE SKIP_PRECOMPILE_HEADERS TRUE)
else()
    message(STATUS "No generated code in ${VCS_GENERATED_DIR}: building tools only. Run python tools/build.py --recompile.")
endif()

# Debug symbols for the runtime and our code (not the 240 MB of generated code),
# so the sampling profiler and debuggers can name host functions.
option(VCS_RUNTIME_DEBUG_INFO "Emit CodeView debug info for runtime + launcher" ON)
if(VCS_RUNTIME_DEBUG_INFO AND MSVC)
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE /Z7)
        endif()
    endforeach()
    if(VCS_BUILD_GAME_RUNNER)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "/Z7")
        target_link_options(ps2EntryRunner PRIVATE /DEBUG)
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
elseif(VCS_RUNTIME_DEBUG_INFO AND MINGW)
    # llvm-mingw: CodeView in a PDB, as clang-cl's /Z7 /DEBUG - dbghelp (src/debug: crash handler, profiler) and
    # tools/minidbg.py read it the same way
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -g -gcodeview)
        endif()
    endforeach()
    if(VCS_BUILD_GAME_RUNNER)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-g;-gcodeview")
        target_link_options(ps2EntryRunner PRIVATE -g -Wl,--pdb=)
        target_link_libraries(ps2EntryRunner PRIVATE dbghelp)
    endif()
elseif(VCS_RUNTIME_DEBUG_INFO)
    # Linux: line tables for the crash handler / profiler (src/debug, symbolized with llvm-symbolizer); frame pointers
    # for their backtraces
    foreach(t ps2_runtime ps2_iop)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -gline-tables-only -fno-omit-frame-pointer)
        endif()
    endforeach()
    if(VCS_BUILD_GAME_RUNNER)
        set_source_files_properties(${VCS_SOURCES} "${CMAKE_SOURCE_DIR}/src/main.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-gline-tables-only;-fno-omit-frame-pointer")
        target_link_options(ps2EntryRunner PRIVATE -rdynamic)
    endif()
endif()
# llvm-mingw: the profiler's __try / __except (src/debug/profiler.cpp) need the Microsoft extensions clang-cl has on
# (here, after the debug info's COMPILE_OPTIONS above, which would replace them)
if(MINGW AND VCS_BUILD_GAME_RUNNER)
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/debug/profiler.cpp" TARGET_DIRECTORY ps2EntryRunner APPEND PROPERTY
                 COMPILE_OPTIONS -fms-extensions)
endif()
