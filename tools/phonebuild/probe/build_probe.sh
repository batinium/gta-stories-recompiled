# [phonebuild] The probe APKs of docs/PHONE_BUILD.md, without Gradle (SDK build-tools 35, platform android-35, a JDK):
#   sh build_probe.sh <termux dir from termux_fetch.py> <out dir>
# phonebuildprobe: Run.java + Termux's clang/lld as lib/x86_64/lib*.so (the emulator) - pass ABI=arm64-v8a for a phone
# phonebuildhost:  Run.java only; it runs the probe's compiler from the probe's nativeLibraryDir (the add-on case)
# Run: put a script at files/test.sh (adb shell run-as <pkg>), then `am start -n <pkg>/io.github.batinium.phonebuildprobe.Run`;
# its output lands in files/out.txt. Debug-signed (~/.android/debug.keystore). Never distribute these APKs.
# Windows (Git Bash): BAT=.bat for d8 / apksigner.
set -e
T=$1; OUT=$2; ABI=${ABI:-x86_64}; HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}; BT=$SDK/build-tools/35.0.0; AJ=$SDK/platforms/android-35/android.jar
JAVA=${JAVA_HOME:+$JAVA_HOME/bin/}
case $ABI in arm64-v8a) TA=aarch64;; *) TA=x86_64;; esac
mkdir -p $OUT/cls $OUT/dex
"${JAVA}javac" --release 11 -cp "$AJ" -d $OUT/cls $HERE/Run.java
"$BT/d8${BAT}" --min-api 33 --lib "$AJ" --output $OUT/dex $(find $OUT/cls -name "*.class")
for pkg in phonebuildprobe phonebuildhost; do
  "$BT/aapt2" link --manifest $HERE/Manifest_$pkg.xml -I "$AJ" -o $OUT/${pkg}_base.apk
  python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'a'); z.write(sys.argv[2],'classes.dex'); z.close()" \
    $OUT/${pkg}_base.apk $OUT/dex/classes.dex
  [ $pkg = phonebuildprobe ] && python3 -I $HERE/../make_probe.py $ABI $T/$TA/root/data/data/com.termux/files/usr $OUT/${pkg}_base.apk
  "$BT/zipalign" -f -p 4 $OUT/${pkg}_base.apk $OUT/${pkg}_aligned.apk
  "$BT/apksigner${BAT}" sign --ks ~/.android/debug.keystore --ks-pass pass:android --out $OUT/$pkg.apk $OUT/${pkg}_aligned.apk
  rm -f $OUT/${pkg}_base.apk $OUT/${pkg}_aligned.apk
done
ls -l $OUT/*.apk
