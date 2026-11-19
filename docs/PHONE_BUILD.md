# Building the code pack on the phone (design record, 2026-11-18)

Today a player needs a PC: `tools/android.py build-game` turns their disc into the code pack (`libvcs_game.so`, one per
game) that the runtime-only APK imports. Many phone players have no PC. This is the plan, with measurements and a
working proof, for a **"Build on this phone"** path: the same pipeline, run by the app on the player's phone, from the
player's disc. We still never distribute game code: we ship tools only, and everything made from the disc stays in the
app's private storage.

Status: **shipped as an experimental feature** (branch `phonebuild2`): *Disc > Build code pack on this phone
(experimental)…*, both games. The first version takes the fastest working route, not the recommended one below: it
downloads **Termux's official clang 21 / lld packages at run time** (pinned by SHA-256) instead of our own LLVM 19
build. Proven end to end in the app on the Android 15 emulator for LCS and VCS (see "The shipped version"). Not yet run
on a real phone.

## The shipped version (experimental)

What the app does (`PhoneBuild.java` on `BuildService`'s thread, a `dataSync` foreground service):

| Step | How | Where |
|---|---|---|
| Compiler | Termux's `clang`, `lld`, `libllvm`, `libc++` + `zlib`, `zstd`, `libxml2`, `libiconv`, `liblzma`, `libandroid-glob`, `libffi` (`tools/phonebuild/termux_packages.txt` -> `assets/phonebuild/termux.txt`: version, size, SHA-256, licence; refresh with `tools/phonebuild/termux_pins.py`), downloaded once from `packages-cf.termux.dev`, checked, `data.tar.xz` unpacked (XZ for Java, 0BSD), only `bin/clang-21`, `bin/lld`, `lib/*.so*`, clang's headers | `files/builder/<aarch64|x86_64>/` (~275 MB) |
| Headers / sysroot | `assets/phonebuild/runtime.zip` (the runtime headers, `game_abi.h`, `game_library_stamp.cpp`, sse2neon.h), `sysroot.zip` (the NDK r28 sysroot's headers, `crt*_so.o`, `libc/libm/libdl/liblog` stubs, `libunwind.a`, compiler-rt builtins), `headers_hash.txt` (the APK's `VCS_GAME_HEADERS_HASH`, computed by `tools/android.py` as CMake does) | `files/builder/kit/` |
| ELF | `DiscCheck.readExecutable` from the chosen disc (sha1 checked) | `files/build/<game>/` |
| Recompile | `libps2recomp.so` (ps2xRecomp for arm64, in the APK, run from `nativeLibraryDir`) with `assets/phonebuild/game/<id>/<id>.toml` + `functions.csv` | `gen/ee` |
| VU1 | `libvu1recomp.so`: `tools/phonebuild/vu1_recomp.cpp`, a C++ port of `tools/vu1_recomp.py`, **byte-identical output on both discs** (golden-tested) | `gen/vu1` |
| Compile | unity batches of 32 + PCH, `/system/bin/linker64 clang-21 …` (the game library's flags, no `-g`), jobs = min(cores, 8, (free RAM - 1.5 GB) / 900 MB), `vu1_compiled.cpp` split into 24 parts + a main file (`PhoneBuild.splitVu1`: its ~100 functions spread over files of their own, non-static in `namespace vu1c_split`, the tables and registrar in the main file), each a unit, compiled first, with `-mllvm -enable-misched=false -fno-slp-vectorize -fno-vectorize`; a unit starts only while 700 MB are free; paused while the thermal status is SEVERE or worse; after a low-memory kill the next run halves the jobs (4 kills: it gives up and says so) | `obj/uNNN.o` + `.ok` |
| Link | `linker64 lld -flavor gnu` with the clang driver's Android link line, against the installed APK's `libmain.so` / `libc++_shared.so`, `--no-undefined`, `--strip-debug` | `libvcs_game.so` |
| Install | the import's checks (`checkLibrary`, `stampProblem`, the stamp's game) and place (`files/game/<id>/`, read-only); `files/build/<game>/` deleted | |

**The VU1 unit's memory** (Termux's clang 21, measured in the emulator): as one translation unit, LCS's needs
~6.1 GB at `-O2` even with the scheduler off, ~620 MB without the vectorizers (68 s); **VCS's needs 6.5-7 GB whatever
the flags** (`-O1`: 6.5 GB, 171 s; `-fno-unroll-loops`: 7.1 GB; `-O0` still 1.5 GB after 4 minutes). The low-memory
killer stopped the whole app three times on the 8 GB emulator while it ran. Each of its ~100 functions alone compiles
in 1-21 s (all 104 one by one: 255 s): the blow-up is the whole module at once (clang 19 on the PC: 1.4 GB). So the
phone build splits the file (above) and compiles the parts in parallel like the other units. The parts keep `-O2` with
the vectorizers off (memory); the object is smaller than the PC's (LCS 3.0 MB vs 5.9 MB): VU1 speed on the phone still
to compare with a PC-built pack.

**Resume**: every step leaves a marker; a killed build continues where it stopped: `START_STICKY` restarts the service
(seen on the emulator after `kill -9` of the app: 44 of 325 units kept, the rest compiled), or the row's tap.
Cancel: the notification's action or the row while it runs. An APK with another headers hash / version starts over.

**Measured in the app** (Android 15 emulator, x86_64, 6 vCPU, x86_64 Termux clang targeting arm64; the recompiler and
VU1 translator are arm64 and ran through the emulator's ARM translation):

| | LCS | VCS |
|---|---|---|
| Compiler download (once) | 24 s, 70 MB (x86_64 set; arm64 68 MB) | (kept) |
| ELF + recompile + VU1 + PCH | 7 s | 30 s |
| Units (6 jobs) | 192 + the VU1 parts | 325 + 25 VU1 parts (the parts: 2-89 s each, all done after 1 min 40 s) |
| Total, to "installed" | **4 min 0 s** (first run, VU1 as one unit: 4 min 28 s) | **6 min 48 s** |
| Pack | 41.6 MB, stamp `abi=1 headers=c309efb75b243068 game=lcs`, accepted | 60 MB, stamp `… game=vcs`, accepted |
| Largest clang process / lowest free RAM | | 485 MB / 6.1 GB free of 8 GB (no low-memory kill) |

Before the VU1 split, VCS's single VU1 unit got the app killed by the low-memory killer three times (resume worked
each time, but the unit never finished). A kill during the build was also tested on purpose (`kill -9` of the app
mid-compile): the service restarted by itself and went on with 281 of 325 units left.

Rough edges: the compiler is clang 21 (the PC builds with the NDK's clang 19): same flags, different code; the
Termux pins break when Termux updates clang (its pool keeps only current versions: the download then fails with
"update the app"); ~275 MB stays in `files/builder/` until *Delete the code pack compiler*; libiconv is LGPL-2.1
(dynamically linked, unmodified, from Termux's published sources); no real-phone timing yet.

## Recommendation (summary)

| Piece | Choice |
|---|---|
| Disc -> ELF | Java, in the launcher (`DiscCheck` already reads SYSTEM.CNF and the executable out of the ISO in place) |
| Recompiler | `ps2xRecomp` built for arm64 with the NDK, shipped in the **main APK** as `libps2recomp.so` in `jniLibs` (2.0 MB stripped), run from `nativeLibraryDir`; the game configs (`game/<id>/*.toml`, `functions.csv`, already public) as assets (~0.7 MB) |
| VU1 microcode -> C++ | port `tools/vu1_recomp.py` (1064 lines, stdlib Python) to C++ inside the recompiler; golden test: byte-identical output on both discs |
| Compiler | **our own LLVM build, pinned to the NDK r28 clang (19.0.1, r530567)**, AArch64 target only, clang + lld as one static multi-call binary, plus a trimmed NDK sysroot and the runtime headers |
| Compiler delivery | an optional **"builder tools" download** (one zip from the same GitHub release the in-app updater uses, sha256 pinned in the APK), unpacked into `files/builder/`, run through `/system/bin/linker64` (proven). Fallback if that ever stops working: the same binary as an add-on APK, executed from its `nativeLibraryDir` (proven too) |
| Runtime headers | in the main APK (assets, ~1 MB raw): they are this APK's interface, so they always match its `VCS_GAME_ABI` / headers hash |
| Libraries to link against | the installed APK's own `libmain.so` and `libc++_shared.so` (readable in `nativeLibraryDir`) |
| Build runner | Java scheduler in a `:builder` process under a foreground service (progress notification, wake lock, thermal and memory guards), one clang process per unit, resumable |
| Code changes for speed | no `-g` (the PC build strips it anyway); `-mllvm -enable-misched=false` for the VU1 unit only (3-6x faster, see below); optionally split `vu1_compiled.cpp` per program |

Estimated time on a Snapdragon 8 Elite-class phone: **LCS ~3-5 min, VCS ~5-8 min**; mid-range (4 big + 4 little
cores): LCS ~10 min, VCS ~15 min. Disk: about **1 GB free** needed during the build; afterwards only the pack (35-50 MB)
plus, if kept, the builder tools (~100 MB). Details below.

## The pipeline, and what each step costs

| Step | PC today | On the phone | Measured |
|---|---|---|---|
| ELF out of the ISO | `tools/vcs_disc.py` | Java (`DiscCheck` reads it already) | ~1 s |
| Recompile EE | `ps2_recomp <toml>` | the same program, built for arm64 | PC 6.7 s (1 or all threads, same); **arm64 build on the emulator (through its ARM translation!) 6.7 s, output identical to `generated-lcs/ee` (6143 files, md5 of every file, PC's CRLF normalised)** |
| VU1 microcode | `tools/vu1_recomp.py` | to port to C++ | PC 0.25 s, output identical to `generated-lcs/vu1` |
| Unity batches + PCH | CMake | the builder writes `uNNN.cpp` (32 includes each, CMake's batching) and builds the PCH | PCH 2-3.6 s |
| Compile | NDK clang, 8-24 jobs | Android clang | see "Compile time" |
| Stamp + link + strip | CMake / llvm-strip | clang driver + lld, `--strip-debug` | link < 2 s |

Generated code size: **LCS 6143 files, 159 MB** (+ 4.1 MB VU1); **VCS 10385 files, 228 MB** (+ 4.3 MB VU1).

## Compile time

PC: Core Ultra 9 275HX (8 P + 16 E cores), NDK r28 clang 19.0.1, the game library's exact command line
(`compile_commands.json` of `build/android/game-lcs/cmake`), `-O2`; "CPU" = the sum of the units' times while 8 ran
in parallel (`tools/phonebuild/measure_tu.py`).

| | LCS | VCS |
|---|---|---|
| Units (32 files each) | 192 | 325 |
| EE units, CPU | **612 s** | **965 s** |
| VU1 unit, as built today | 312 s (one unit!) | 592 s |
| VU1 unit, `-mllvm -enable-misched=false` | **97 s** | **92 s** |
| Total CPU with the VU1 fix | ~710 s | ~1060 s |
| Wall, 8 jobs, today | 312 s (VU1-bound) | 592 s (VU1-bound) |
| Wall, 8 jobs, with the VU1 fix (est.) | ~100 s | ~135 s |
| Peak RSS, EE unit / VU1 unit | 350 MB / 1.0 GB | 460 MB / 1.4 GB |
| Objects (no `-g`) | 54 MB | 89 MB |
| PCH | 31 MB | 32 MB |

- **`-g` costs 31%** (sample of 22 LCS units: 108 s with, 74 s without) and is stripped from the pack anyway: drop it.
- **`-O1`** would save another ~30% (52 s on that sample) but changes the game's code: not without an FPS check.
- **The VU1 unit** (`vu1_compiled.cpp`: 48 functions of up to thousands of lines of straight-line code) is the
  critical path. `-ftime-report`: 99.7% code generation, of which **76% "Machine Instruction Scheduler"** (the
  pre-register-allocation scheduler, superlinear in basic block size). `-mllvm -enable-misched=false` on that one
  unit: LCS 312 s -> 97 s, VCS 592 s -> 92 s, same object size. The phone's cores are out of order, so the runtime
  cost should be small (to check: the VU1 time in the stats overlay). Splitting the file per program (vu1_recomp.py
  emits N files) would cut it further and let it run on several cores. Worth doing for the PC build too.

**On Android** (emulator, Android 15 x86_64, 6 vCPU on the same PC, Termux's clang 21.1.8 for x86_64 targeting
aarch64, in the app's process): the 191 EE units took 1351 s CPU (**2.2x the PC's NDK clang**: emulation, vCPUs
landing on E-cores, clang 21, and my PC runs in parallel), 227 s wall at 6 jobs; the VU1 unit with the scheduler off
164 s. **Clang 21 needed 6.1 GB of RAM for the VU1 unit** (clang 19 on the PC: 1.0 GB) and the emulator's
low-memory killer killed the whole app twice ("min watermark is breached", "device is not responding", even as the
TOP app): another reason to pin the NDK's clang 19, and the reason the scheduler watches memory (below). The resumable
build continued where it stopped each time.

**Phone estimate.** Snapdragon 8 Elite: 2 cores at 4.3 GHz + 6 at 3.5 GHz; per core roughly 0.7-0.8 of the PC's
P-core on compiler work, and under 5+ minutes of all-core load phones settle at ~60-75% of their peak clock. Counting
6-8 jobs x 0.7 x 0.7 = **3-4 PC-core equivalents**:

| | LCS | VCS |
|---|---|---|
| Flagship (8 Elite / 8 Gen 3 class), VU1 fix, clang 19 | 710 s / 3.5 ≈ **3.5 min** (VU1 ~2.5 min runs alongside) | 1060 s / 3.5 ≈ **5 min**, say 5-8 |
| Mid-range (4 x A7xx + 4 x A5xx, ~1.2 PC-core eq.) | **~10 min** | **~15 min** |
| Flagship as today (no VU1 fix) | ≥ 8 min, VU1-bound | ≥ 14 min, VU1-bound |
| Termux clang 21 as is | VU1 unit needs ~6 GB: killed on most phones | worse |

Heat: ~5-10 min of all-core load is a benchmark-loop's worth; the phone gets warm, the guard below throttles it.

## Running a compiler inside an app (Android rules, tested)

Apps targeting API 29+ may not `exec()` files in their writable storage (W^X). Tested with `tools/phonebuild/probe`
(two tiny APKs: `Run.java` runs `files/test.sh` from the app's own process; context `u:r:untrusted_app`, targetSdk 35,
Android 15):

| Test | Result |
|---|---|
| exec `nativeLibraryDir/libclang.so` (clang packaged as `lib*.so`, `extractNativeLibs=true`) | works |
| exec **another package's** `nativeLibraryDir/libclang.so` (`<queries>` + `getApplicationInfo(pkg).nativeLibraryDir`) | works (the add-on case) |
| exec a copy in `files/` | **Permission denied** (as documented) |
| `/system/bin/linker64 files/tc/clang`, its libraries in `files/` too | works: `--version`, a full unit compile (6 s), `linker64 lld -flavor gnu` link of a `.so` |
| a symlink in `files/bin/` pointing into `nativeLibraryDir` (so argv[0] is `clang++` / `ld.lld`) | works: the driver ran lld through it for the whole pack |

The `linker64` path maps app-private files executable, exactly what loading the imported code pack (`dlopen` from
`files/game/<id>/`) already relies on: it adds no platform risk the app doesn't have already. Notes: under linker64
`/proc/self/exe` is the linker, so the driver needs `-resource-dir` and `--sysroot` (always given) and the link step
calls lld directly (`-flavor gnu`), not through the driver's exec.

Other Android limits that shape the runner:

- **Low-memory killer**: kills the app's whole process group, foreground or not (seen twice in the emulator). Jobs =
  `min(cores, (MemAvailable - 1.5 GB) / 500 MB)`, the VU1 unit counted as 1.5 GB (clang 19), started first while the
  memory is free; on `onTrimMemory` / low `MemAvailable` stop starting units.
- **Phantom process killer** (Android 12+): at most 32 child processes across all apps, and children of background
  apps that use much CPU are killed. One clang process per unit started straight from Java (no `sh` / `xargs`), at
  most 8, while a foreground service runs (not "background").
- **Foreground service** (Android 14+ needs a type): `specialUse` (we are not on Play) or `dataSync` (Android 15
  allows dataSync 6 h a day: plenty). `POST_NOTIFICATIONS` for the progress notification (Android 13+), a
  `PARTIAL_WAKE_LOCK` while building.
- **Thermal**: `PowerManager.getThermalHeadroom()` / `addThermalStatusListener`: fewer jobs at `SEVERE`, pause at
  `CRITICAL`, resume when it cools. Battery: ask to plug in below 30%, pause below 15% unless charging.

## Toolchain options

| | Size (arm64) | Notes |
|---|---|---|
| Termux's packages as is (clang 21.1.8, lld, libLLVM, libclang-cpp, zlib, zstd, libxml2, libiconv, ...) | **67 MB APK / 211 MB extracted** | Works (the proof uses it). All LLVM targets, many tools, dynamic. Different clang than the PC build, 6 GB for the VU1 unit. libiconv is LGPL-2.1 (dynamic linking + source offer), the rest permissive |
| **Own LLVM 19 (NDK r28's revision), AArch64 only, static, `LLVM_TOOL_LLVM_DRIVER_BUILD` (clang + lld in one binary), no libxml2 / terminfo / zstd** | est. **25-35 MB zip / ~90 MB unpacked** | Same compiler as `build-game` on the PC: the same code, ideally a byte-identical pack (with `-ffile-prefix-map`), so a phone build can be checked against a PC build of the same version. Built once per LLVM bump by a script with the NDK, like FFmpeg today |
| NDK sysroot + clang resource headers + libunwind.a | 22 MB headers raw (5.5 MB actually included) + 1.1 MB stubs; **~4 MB zipped trimmed** | in the builder zip |
| Runtime headers (`ps2xRuntime/include`, `ps2xIOP/include`, `Kernel/Stubs/*.h`, `src/android/game_abi.h`, `game_library_stamp.cpp`, sse2neon.h) | ~1 MB raw | in the main APK (they are its interface) |
| Recompiler `libps2recomp.so` + game configs | 2.0 MB + 0.7 MB | in the main APK |

Main APK growth: ~2 MB compressed. Builder download: ~30-40 MB, deletable after the build ("Remove builder tools").

Termux packages used by the prototype (official repository, termux-main, stable; `tools/phonebuild/termux_fetch.py`
checks each against the index's SHA256): `https://packages-cf.termux.dev/apt/termux-main/pool/main/` -
`c/clang/clang_21.1.8-3_<arch>.deb`, `l/lld/lld_21.1.8-3_<arch>.deb`, `libl/libllvm/libllvm_21.1.8-3_<arch>.deb`,
`libc/libc++/libc++_30_<arch>.deb`, `libc/libcompiler-rt/libcompiler-rt_21.1.8-3_<arch>.deb`,
`n/ndk-sysroot/ndk-sysroot_30_<arch>.deb`, and zlib 1.4, zstd 1.5.7-1, libxml2 2.15.4-1, ncurses, libiconv 1.19.1,
liblzma 5.8.4, libandroid-glob 0.6-3, libffi 3.8.1 (arch aarch64 and x86_64). Licences: LLVM (clang, lld, libLLVM,
libc++, compiler-rt) Apache-2.0 WITH LLVM-exception; zlib (Zlib), zstd (BSD-3-Clause), libxml2 (MIT), ncurses (MIT-X11),
libffi (MIT), liblzma (0BSD / public domain), libiconv (LGPL-2.1+); Termux's build recipes Apache-2.0. Never committed
or shipped by this branch.

## Alternatives considered

- **A lighter compiler (TCC, cproc, ...)**: TCC compiles C only, the generated code is C++20 with templates and
  inline runtime methods (sse2neon, `PS2Runtime`, `R5900Context`): a C back end for the recompiler would be a large
  upstream change, and TCC's code runs ~3-10x slower than `-O2`, on phones where the game needs every frame. No.
- **Only hot functions compiled, the rest interpreted**: there is no EE interpreter in the runtime (static recompiler
  only), writing one is months, and an interpreted PS2 CPU on a phone is far too slow for cold-but-frequent code. No.
- **LLVM bitcode + on-device JIT/AOT**: bitcode made on our side would be game code (not distributable); made on the
  phone it saves only the frontend, which the PCH already amortises (codegen is ~100% of the VU1 unit, most of the
  rest). A JIT at every start costs the same minutes each time. No.
- **Recompiler emitting LLVM IR directly**: skips the C++ frontend (~10-20% of an EE unit), big change. Not now.
- **`-O1` for cold code**: needs a profile of hot functions; the whole-build `-O1` saving is ~30%. Possible later as a
  "fast build" option, after an FPS comparison.
- **Unity size**: 32 is CMake's default here; bigger batches save PCH load per unit (small), smaller batches make a
  resume lose less work. Keep 32.

## Legal

We ship tools only: the recompiler (GPL-3.0, ours/upstream's), the runtime headers (GPL-3.0), the game configs and
function maps already public in this repository (addresses and names, no game code), and the LLVM toolchain + NDK
sysroot (Apache-2.0 WITH LLVM-exception, bionic's BSD-style headers, Linux UAPI headers under GPL-2.0 WITH
Linux-syscall-note; NOTICE files in the zip). Everything made from the disc - the ELF, the generated C++, the objects,
the PCH and the pack - is made on the player's phone from the player's disc, stays in the app's private storage
(`files/`, not readable by other apps, not backed up: `allowBackup=false`) and is deleted after the build except the
pack, exactly as the imported pack is kept today. Nothing is uploaded; the builder never touches the network except
to download the tools zip when asked. Same position as the PC `build-game`, moved onto the phone.

## In the app

**Launcher** (Disc category, per game, next to "Import game library (code pack)…"):

- **"Build code pack on this phone…"**, enabled when the selected game's disc is picked and checks out (`discOk`).
  Value: "not built" / "built <date>, <size>" / "building 63% (U 121/192)" / "paused (too hot)" / the last error.
- First use: a page that says what happens (time estimate for this phone from its core count and RAM, ~1 GB free
  needed, plug in, keep the app open or let the notification run), then "Download builder tools (35 MB)". The zip is
  checked against the sha256 in the APK and unpacked to `files/builder/<llvm version>/`.
- Progress: the launcher row and a notification ("Building Liberty City Stories code pack - 121 of 192, ~2 min
  left"), with Pause / Cancel. Estimate from the units' sizes (bytes of generated code per unit, the VU1 unit
  weighted) and the measured rate so far.
- Done: the pack goes through the same checks as an import (`checkLibrary`, `stampProblem`, `libraryFileGame` must be
  the selected game) and is installed the same way (`files/game/<id>/libvcs_game.so`, read-only), the build folder is
  deleted. The launcher shows "Built on this phone" in the library row; Play works as with an imported pack.
- "Remove builder tools" (frees ~90 MB) under the same category once a pack exists.

**Files** (all app-private, under `files/`, never `getExternalFilesDir`):

```
files/builder/<version>/           the tools (clang+lld binary, sysroot/, resource/), kept or removed by the player
files/build/<game>/                one build in progress; deleted when the pack is installed
    state.json                     version of the APK + headers hash + disc sha1 + step reached (a mismatch: start over)
    elf                            the executable from the disc
    gen/ee, gen/vu1                the recompiler's output (a generated file is deleted once its unit's .o exists)
    unity/uNNN.cpp, pch.hxx(.pch)
    obj/uNNN.o + uNNN.ok           a unit is done when its .ok exists (written after the rename of the .o)
    libvcs_game.so                 linked, checked, then moved to files/game/<id>/
```

Disk (measured LCS on the emulator, VCS scaled): LCS gen 163 MB + PCH 33 MB + objects 52 MB + pack 36 MB ≈ 290 MB
peak (deleting generated files as their units finish keeps it under ~200 MB); VCS ≈ 232 + 32 + 89 + ~50 ≈ 400 MB;
plus the builder tools ~90 MB (+ the 35 MB zip while unpacking). The button checks `StatFs` for **1 GB** free and says
so before starting.

**Runner** (`BuildService`, `:builder` process, foreground service): reads `state.json`, runs the steps not done:
extract (Java) -> recompile (`libps2recomp.so`) -> VU1 (in the recompiler) -> unity + PCH -> units (a Java pool of
`ProcessBuilder`s, the job count from memory and thermal state, the VU1 unit first, each unit's stderr kept on
failure) -> stamp -> link -> checks -> install. Resume after the app or the phone was killed: everything with an `.ok`
stays; a unit's partial output is a `.tmp` that is deleted. A new APK version whose headers hash differs restarts
the build (its interface changed); the same hash keeps the work. Cancel deletes `files/build/<game>/`.

**Errors** (shown on the row and in the notification, with the log file kept for "Share build log"):

| Case | Message |
|---|---|
| Not enough space | "Needs about 1 GB free (you have 400 MB): free some space and try again." |
| Disc changed / wrong disc | the existing `DiscCheck` messages |
| Tools download failed / sha256 mismatch | "Couldn't download the builder tools (…): try again on Wi-Fi." |
| Killed by the system (low memory) | resumes with fewer jobs; after 3 kills at 1 job: "This phone doesn't have enough memory to build it; build it on a PC (Player's guide)." |
| Too hot | "Paused: the phone is too hot. It goes on when it cools down." |
| A unit fails to compile | "Build failed in unit 121 (a bug, not your phone): please report it with the build log." |
| Pack refused by the checks | the existing `stampProblem` / `checkLibrary` text |

## Phased plan

| Phase | Work | Effort |
|---|---|---|
| 1 | VU1 compile fix in the shared build (`-mllvm -enable-misched=false` on `vu1_compiled.cpp`, CMake `set_source_files_properties`; FPS check on the phone) - likely shortens the PC builds too (to measure there) | 0.5 day |
| 2 | Toolchain: script that builds LLVM 19 at NDK r28's revision for aarch64-linux-android (clang+lld driver build, AArch64 only, static) + the trimmed sysroot zip with NOTICEs; reproducibility check: a pack built with it on the PC (through qemu or a phone) equals `build-game`'s | 2-3 days |
| 3 | Recompiler for Android: `tools/phonebuild/recomp_android` into the Gradle build as `libps2recomp.so`; `vu1_recomp.py` ported to C++ with a golden test (both discs); output to a given folder; game configs as assets | 2-3 days |
| 4 | `BuildService` + runner (steps, unit pool, memory / thermal / battery guards, resume, logs), install through the import checks | 3-4 days |
| 5 | Launcher UI: the row, the first-use page, tools download (reuse UpdateMenu's downloader), progress, errors, "Remove builder tools"; docs (ANDROID.md, README Player's guide) | 2 days |
| 6 | Real-device runs: flagship + mid-range + 8 GB phone, LCS and VCS, timings and temperatures into this doc; a pack built on the phone vs one from the PC (same bytes?) | 1-2 days |

Total ~2-3 weeks of focused work. Phase 1 is independent and worth doing first.

## The prototype in this branch

- `tools/phonebuild/recomp_android/CMakeLists.txt`: the recompiler for Android arm64 (standalone; PS2Recomp's own tree
  turns the recompiler off on Android). Built and run: identical output.
- `tools/phonebuild/build_pack.sh`: the on-device build as a shell script (PCH, units with resume, stamp, link) - the
  plan for the Java runner.
- `tools/phonebuild/probe/`: the probe APKs (Run.java, manifests, `build_probe.sh`, no Gradle) and
  `tools/phonebuild/make_probe.py` (Termux's clang as `lib*.so`).
- `tools/phonebuild/termux_fetch.py`: downloads and checks the official Termux packages for the prototype.
- `tools/phonebuild/measure_tu.py`: the PC timings above (Windows).
