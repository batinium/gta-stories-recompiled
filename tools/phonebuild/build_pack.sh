# [phonebuild] Prototype of the on-phone code pack build (docs/PHONE_BUILD.md). Run by the app's own process (it
# runs as untrusted_app, as the real builder will); proven on the Android 15 emulator with tools/phonebuild/probe.
#
#   sh build_pack.sh <jobs> <game: lcs|vcs> <headers hash (src/android/game_abi.cmake)>
#
# Environment (set by the app): FILES (its files dir), NATIVE_DIR / ADDON_DIR (the nativeLibraryDir that holds
# libclang.so and liblld.so: the app's own, or the builder add-on's). Inputs under $FILES/kit (the layout the
# app prepares): sysroot/ (NDK sysroot: headers, crt*_so.o, libc/libm/libdl/liblog stubs), resource/ (clang's
# headers + libclang_rt.builtins), inc/ (the runtime headers the generated code includes), gen/ee, gen/vu1 (the
# recompiler's output), unity/uNNN.cpp (32 generated files each, as CMake's unity batches), pch.hxx,
# link/libmain.so + link/libc++_shared.so (from the installed APK).
# Resumable: a unit with a .o.ok is done; the app can be killed and started again at any point.
KIT=$FILES/kit; OUT=$FILES/build; JOBS=${1:-4}; GAME=${2:-lcs}; HEADERS=${3:?headers hash}
TC=$NATIVE_DIR; [ -f $TC/libclang.so ] || TC=$ADDON_DIR
export LD_LIBRARY_PATH=$TC
mkdir -p $OUT/obj $FILES/bin
# the driver's mode comes from argv[0] (clang++), lld's from its name (ld.lld): links in files/, pointing into the
# nativeLibraryDir; exec follows the link, and the SELinux check is the target's (apk_data_file: allowed)
ln -sf $TC/libclang.so $FILES/bin/clang++
ln -sf $TC/liblld.so $FILES/bin/ld.lld
CXX=$FILES/bin/clang++
FLAGS="--target=aarch64-none-linux-android33 --sysroot=$KIT/sysroot -resource-dir $KIT/resource
 -DPS2X_ENABLE_IOP_RPC_TRACE=1 -DPS2_RUNTIME_LOGS=1 -DSSE2NEON_PRECISE_DIV=1 -DSSE2NEON_PRECISE_DP=1
 -DSSE2NEON_PRECISE_MINMAX=1 -DSSE2NEON_PRECISE_SQRT=1 -DUSE_SSE2NEON -DVCS_HAVE_VULKAN=1 -Dps2GameLibrary_EXPORTS
 -I$KIT -I$KIT/inc -I$KIT/gen/ee -I$KIT/inc/src -I$KIT/inc/ps2xRuntime/include -I$KIT/inc/kernel -I$KIT/inc/ps2xIOP/include
 -DANDROID -fdata-sections -ffunction-sections -funwind-tables -no-canonical-prefixes -D_FORTIFY_SOURCE=2
 -Wformat -Werror=format-security -std=gnu++20 -fPIC -ffp-contract=off -fno-strict-aliasing -O2 -DNDEBUG
 -march=armv8.2-a -fno-math-errno -fno-stack-protector -Wno-everything"
export CXX FLAGS OUT KIT
t0=$(date +%s)
echo "[build] $(date) jobs=$JOBS cpus=$(nproc) toolchain=$TC"
if [ ! -f $OUT/pch.ok ]; then
  $CXX $FLAGS -x c++-header -Xclang -emit-pch -o $OUT/pch.pch $KIT/pch.hxx || { echo "[build] PCH failed"; exit 1; }
  touch $OUT/pch.ok
fi
echo "[build] pch $(( $(date +%s) - t0 ))s"
# the units, the last (the VU1 code, the longest by far) first. The VU1 unit skips the pre-RA machine scheduler:
# its huge basic blocks make that pass 76% of a 5-minute compile (-ftime-report); without it 2.2x faster
ls $KIT/unity | sort -r | xargs -P $JOBS -n 1 sh -c '
  u=$0; o=$OUT/obj/${u%.cpp}.o
  [ -f $o.ok ] && exit 0
  extra=; grep -q "gen/vu1/" $KIT/unity/$u && extra="-mllvm -enable-misched=false"
  s=$(date +%s)
  $CXX $FLAGS $extra -Xclang -include-pch -Xclang $OUT/pch.pch -c $KIT/unity/$u -o $o.tmp 2>$o.log && mv $o.tmp $o \
    && touch $o.ok && echo "[unit] $u $(( $(date +%s) - s ))s" || { echo "[unit] $u FAILED"; head -20 $o.log; exit 255; }'
rc=$?
echo "[build] units $(( $(date +%s) - t0 ))s rc=$rc"
[ $rc = 0 ] || exit 1
$CXX $FLAGS -DVCS_GAME_HEADERS_HASH=\"$HEADERS\" -DVCS_GAME_ID=\"$GAME\" \
  -c $KIT/inc/src/android/game_library_stamp.cpp -o $OUT/obj/stamp.o || exit 1
# the link as tools/android.py build-game's (CMake's), libc++_shared and libmain.so being the installed APK's
$CXX --target=aarch64-none-linux-android33 --sysroot=$KIT/sysroot -resource-dir $KIT/resource -fuse-ld=lld \
  --ld-path=$FILES/bin/ld.lld -nostdlib++ -shared -fPIC -Wl,--build-id=sha1 -Wl,--no-undefined -Wl,--gc-sections \
  -Wl,-soname,libvcs_game.so -Wl,--strip-debug -o $OUT/libvcs_game.so $OUT/obj/*.o \
  $KIT/link/libmain.so $KIT/link/libc++_shared.so -lm || { echo "[build] link failed"; exit 1; }
echo "[build] linked $(( $(date +%s) - t0 ))s: $(ls -l $OUT/libvcs_game.so)"
echo "[build] build/ holds $(du -sh $OUT | cut -f1)"
