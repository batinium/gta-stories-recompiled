#pragma once
// [android] The game library's interface number (docs/ANDROID.md "Runtime / game split"). libvcs_game.so (the
// player's generated code, built by tools/android.py build-game) is compiled against the runtime's headers and called
// by the runtime in libmain.so (the APK). Both carry this number; the APK refuses a game library with another one
// (src/android/game_library.cpp, and the launcher before it starts the game: GameFiles.java reads the same stamp).
//
// It is the ONLY thing that decides compatibility: players keep their library across app updates for as long as it
// stays the same. Bump it (and only then) when a change alters what the generated code compiled against:
//   - R5900Context: its layout (registers, fields, sizes, order), or the meaning of a field the generated code uses
//   - ps2_runtime_macros.h: the memory access / arithmetic macros and inline helpers the generated code expands
//   - PS2Runtime: its data members' layout up to the ones the generated code's inline calls reach (m_eeClock,
//     m_stopRequested, ...); the signatures of methods it calls (dispatchGuestBranch[Slow], eeCheckpointDue[Slow],
//     lookupFunction, memory(), ...) and RecompiledFunction's type / calling convention (ps2_guest_cc.h)
//   - PS2Memory / EeFastClock and the other classes the generated code touches inline (ps2_memory.h, ee_fast_clock.h)
//   - ps2_syscalls.h / ps2_stubs.h: a syscall or stub the generated code calls removed or its signature changed
//   - the function table: g_ps2RecompiledFunctionTable* (names, types) and how the runtime reaches it
//   - VU1: VU1Interpreter's members reached by Vu1Compiled, Vu1CompiledPiece / Vu1PairShape / Vu1CompiledFn,
//     vu1RegisterCompiledPieces (runtime/ps2_vu1.h, runtime/ps2_vu1_compiled.h)
//   - the recompiler's output itself (ps2xRecomp: what it emits, the names of the table's symbols)
//   - the compiler flags the generated code needs to match (C++ standard library: c++_shared; -march; FP contraction)
// Never for: our own src/ (game hooks, the RHI, the renderer, Dear ImGui, Android), Java, settings, comments, or a new
// function / syscall / stub that only adds to the runtime. Runtime changes should prefer state outside PS2Runtime and
// R5900Context (a new global, a side table) to a new member that shifts their layout.
//
// The headers above are also hashed at configure time (game_abi.cmake, VCS_GAME_HEADERS_HASH): a different hash with
// the same number only logs a hint ("did the interface change? bump VCS_GAME_ABI"), it never refuses.
#define VCS_GAME_ABI 1

// The stamp: a section of its own in the game library, read from the file before it is loaded (no game code runs
// for a library that is refused), by the runtime (game_library.cpp) and the launcher (GameFiles.java).
#define VCS_GAME_STAMP_SECTION ".vcs_game_stamp"
#define VCS_GAME_STAMP_PREFIX "vcs-game-library abi="
// [lcs] ... and then " game=<id>": the game the library was built from (CMake VCS_GAME: vcs, lcs; disc/known_games.h).
// The runtime boots that game's ELF from the disc, the launcher names the game and checks the disc against it. Adding
// it changed nothing the generated code compiles against (no VCS_GAME_ABI bump): a stamp without it is VCS's.
#define VCS_GAME_STAMP_GAME "game="
