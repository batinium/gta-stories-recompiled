// [android] Compiled into the game library only (libvcs_game.so, tools/android.py build-game): its interface stamp
// (game_abi.h), in a section of its own so that the runtime and the launcher read it from the file before loading it.
#include "android/game_abi.h"

#define VCS_STR2(x) #x
#define VCS_STR(x) VCS_STR2(x)

#ifndef VCS_GAME_HEADERS_HASH
#error "VCS_GAME_HEADERS_HASH comes from CMake (src/android/game_abi.cmake)"
#endif
#ifndef VCS_GAME_ID
#error "VCS_GAME_ID (the game: vcs, lcs) comes from CMake (VCS_GAME)"
#endif

// exported (the dynamic symbol table keeps it through --gc-sections) and found by name too: vcs_game_library_stamp
extern "C" __attribute__((visibility("default"), used, section(VCS_GAME_STAMP_SECTION)))
const char vcs_game_library_stamp[] = VCS_GAME_STAMP_PREFIX VCS_STR(VCS_GAME_ABI) " headers=" VCS_GAME_HEADERS_HASH
                                     " " VCS_GAME_STAMP_GAME VCS_GAME_ID; // [lcs] the game it is from
