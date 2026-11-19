# [phonebuild] The recompiler (ps2xRecomp) for Android arm64, as a program the app can run (docs/PHONE_BUILD.md).
# The PS2Recomp tree switches the recompiler off on Android (it builds only the runtime there); this standalone
# project builds just the recompiler, with sse2neon as the ARM runtime build has it (ps2_memory.h includes it).
#
#   cmake -S tools/phonebuild/recomp_android -B <out> -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
#     -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DPS2X_CPU=ARM64
#   cmake --build <out> --target ps2_recomp      # <out>/ps2xRecomp/ps2_recomp (llvm-strip it: 2 MB)
cmake_minimum_required(VERSION 3.21)
project(ps2recomp_android C CXX)
get_filename_component(PS2X_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/PS2Recomp" ABSOLUTE)
include(FetchContent)
FetchContent_Declare(sse2neon GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git GIT_TAG v1.9.1 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(sse2neon)
include_directories(${sse2neon_SOURCE_DIR})
add_compile_definitions(USE_SSE2NEON)
include(${PS2X_ROOT_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake)
add_subdirectory(${PS2X_ROOT_DIR}/ps2xRecomp ps2xRecomp)
# the VU1 microcode translator (C++ port of tools/vu1_recomp.py, byte-identical output)
add_executable(vu1_recomp ${CMAKE_CURRENT_SOURCE_DIR}/../vu1_recomp.cpp)
set_target_properties(vu1_recomp PROPERTIES CXX_STANDARD 17)
