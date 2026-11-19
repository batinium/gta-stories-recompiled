# [android] The game library's interface (src/android/game_abi.h): VCS_GAME_ABI, the number that decides, read from the
# header; VCS_GAME_HEADERS_HASH, a hash of the runtime headers the generated code compiles against (the list below:
# what a generated file and vu1_compiled.cpp include), only for a warning when it differs while the number is the same.
file(STRINGS "${CMAKE_SOURCE_DIR}/src/android/game_abi.h" _vcs_abi_line REGEX "^#define VCS_GAME_ABI [0-9]+")
string(REGEX MATCH "[0-9]+$" VCS_GAME_ABI "${_vcs_abi_line}")
if(NOT VCS_GAME_ABI)
    message(FATAL_ERROR "no '#define VCS_GAME_ABI <n>' in src/android/game_abi.h")
endif()
set(_vcs_rt "${CMAKE_SOURCE_DIR}/third_party/PS2Recomp/ps2xRuntime")
set(VCS_GAME_INTERFACE_HEADERS
    "${CMAKE_SOURCE_DIR}/third_party/PS2Recomp/ps2xIOP/include/ps2x/iop/iop_types.h"
    "${CMAKE_SOURCE_DIR}/third_party/PS2Recomp/ps2xIOP/include/ps2x/iop/ps2_path.h"
    "${_vcs_rt}/include/ps2_call_list.h"
    "${_vcs_rt}/include/ps2_guest_cc.h"
    "${_vcs_rt}/include/ps2_log.h"
    "${_vcs_rt}/include/ps2_runtime.h"
    "${_vcs_rt}/include/ps2_runtime_macros.h"
    "${_vcs_rt}/include/ps2_stubs.h"
    "${_vcs_rt}/include/ps2_syscalls.h"
    "${_vcs_rt}/include/runtime/ee_fast_clock.h"
    "${_vcs_rt}/include/runtime/gs/gs_backend.h"
    "${_vcs_rt}/include/runtime/gs/gs_frontend.h"
    "${_vcs_rt}/include/runtime/gs/gs_types.h"
    "${_vcs_rt}/include/runtime/gs/ps2_gif_arbiter.h"
    "${_vcs_rt}/include/runtime/ps2_address.h"
    "${_vcs_rt}/include/runtime/ps2_audio.h"
    "${_vcs_rt}/include/runtime/ps2_hle_survey.h"
    "${_vcs_rt}/include/runtime/ps2_host_std.h"
    "${_vcs_rt}/include/runtime/ps2_memory.h"
    "${_vcs_rt}/include/runtime/ps2_pad.h"
    "${_vcs_rt}/include/runtime/ps2_rom_device.h"
    "${_vcs_rt}/include/runtime/ps2_vfs.h"
    "${_vcs_rt}/include/runtime/ps2_vu1.h"
    "${_vcs_rt}/include/runtime/ps2_vu1_compiled.h"
    "${_vcs_rt}/src/lib/Kernel/Stubs/Unimplemented.h")
set(_vcs_all "")
foreach(_h IN LISTS VCS_GAME_INTERFACE_HEADERS)
    file(READ "${_h}" _vcs_text HEX)
    string(APPEND _vcs_all "${_vcs_text}")
endforeach()
string(SHA256 _vcs_hash "${_vcs_all}")
string(SUBSTRING "${_vcs_hash}" 0 16 VCS_GAME_HEADERS_HASH)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/android/game_abi.h" ${VCS_GAME_INTERFACE_HEADERS})
message(STATUS "Game library interface: VCS_GAME_ABI ${VCS_GAME_ABI}, headers ${VCS_GAME_HEADERS_HASH}")
