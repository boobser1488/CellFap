#!/usr/bin/env bash
# Builds build/android/CellFap.apk: native library with the NDK (CMake + Ninja from the SDK), the Java activity with
# javac + d8 (a JDK on PATH), then aapt / zipalign /
# apksigner. No Gradle and no downloads: everything comes from the installed Android SDK.
#   android/build_apk.sh [abi ...]      (default: arm64-v8a)
# Before building, the dypt organisms are exported to android/generated/organisms_data.inc by the desktop tool
# build/release/export_organisms.exe (rebuild it with the desktop build after changing scripts/organisms.dpt).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$LOCALAPPDATA/Android/Sdk}}"
SDK="$(cygpath -u "$SDK" 2>/dev/null || echo "$SDK")"
NDK="$(ls -d "$SDK"/ndk/* | sort -V | tail -1)"
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
PLATFORM="$(ls -d "$SDK"/platforms/* | sort -V | tail -1)"
CMAKE="$(ls -d "$SDK"/cmake/* | sort -V | tail -1)/bin"
ABIS=("${@:-arm64-v8a}")
OUT="$ROOT/build/android"
mkdir -p "$OUT"
echo "SDK $SDK"
echo "NDK $NDK"

if [ -x "$ROOT/build/release/export_organisms.exe" ]; then
    (cd "$ROOT/build/release" && ./export_organisms.exe "$ROOT/android/generated/organisms_data.inc")
fi

STAGE="$OUT/stage"
rm -rf "$STAGE"
for ABI in "${ABIS[@]}"; do
    B="$OUT/$ABI"
    "$CMAKE/cmake.exe" -G Ninja -S "$ROOT/android" -B "$B" \
        -DCMAKE_MAKE_PROGRAM="$CMAKE/ninja.exe" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release > "$B.configure.log"
    "$CMAKE/cmake.exe" --build "$B" -j 4
    mkdir -p "$STAGE/lib/$ABI"
    cp "$B/libcellfap.so" "$STAGE/lib/$ABI/"
    "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" --strip-unneeded "$STAGE/lib/$ABI/libcellfap.so"
done

# ---- Java: the activity subclass (orientation, keyboard, files) -> classes.dex
JOUT="$OUT/java"
rm -rf "$JOUT"
mkdir -p "$JOUT/classes"
javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options -bootclasspath "$(cygpath -w "$PLATFORM/android.jar" 2>/dev/null || echo "$PLATFORM/android.jar")" \
    -d "$JOUT/classes" $(find "$ROOT/android/java" -name '*.java')
"$BT/d8.bat" --release --min-api 26 --lib "$PLATFORM/android.jar" --output "$STAGE" $(find "$JOUT/classes" -name '*.class')

# ---- package
APK0="$OUT/unaligned.apk"
rm -f "$APK0" "$OUT/aligned.apk" "$OUT/CellFap.apk"
"$BT/aapt.exe" package -f -M "$ROOT/android/AndroidManifest.xml" -S "$ROOT/android/res" -I "$PLATFORM/android.jar" -F "$APK0"
(cd "$STAGE" && "$BT/aapt.exe" add "$APK0" classes.dex > /dev/null && for f in lib/*/*.so; do "$BT/aapt.exe" add "$APK0" "$f" > /dev/null; done)
"$BT/zipalign.exe" -p -f 4 "$APK0" "$OUT/aligned.apk"

# ---- sign with the debug key (created once)
KS="$HOME/.android/debug.keystore"
if [ ! -f "$KS" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi
"$BT/apksigner.bat" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT/CellFap.apk" "$OUT/aligned.apk"
"$BT/apksigner.bat" verify "$OUT/CellFap.apk"
ls -la "$OUT/CellFap.apk"
