#!/bin/sh
# build_apk.sh - build the arm64 APK without Gradle and without the NDK.
#
# Needs: clang and lld (any recent LLVM with the AArch64 target), aapt2,
# zipalign, apksigner, keytool, python3, and a sysroot from mksysroot.py.
# The app id, version and SDK levels are read from android/app/build.gradle,
# so this build and the Gradle build make the same app.
#
# Use:
#   build_apk.sh --sysroot DIR --res-jar FILE --out FILE.apk
#                [--gen DIR] [--keystore FILE] [--work DIR]
#
#   --gen DIR       output of "tools/ljrecomp build" (lj_recomp.c and
#                   lj_game_data.c). Without it the app has no game.
#   --keystore FILE signing key; made (with password "android") when the
#                   file does not exist. Keep it: Android only installs an
#                   update that has the same key as the installed app.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SYSROOT='' RESJAR='' OUT='' GEN='' KEYSTORE='' WORK=''
while [ $# -gt 0 ]; do
    case $1 in
        --sysroot) SYSROOT=$2; shift 2 ;;
        --res-jar) RESJAR=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --gen) GEN=$2; shift 2 ;;
        --keystore) KEYSTORE=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
if [ -z "$SYSROOT" ] || [ -z "$RESJAR" ] || [ -z "$OUT" ]; then
    echo "usage: build_apk.sh --sysroot DIR --res-jar FILE --out FILE.apk [--gen DIR] [--keystore FILE] [--work DIR]" >&2
    exit 2
fi
CLANG=${CLANG:-clang}
STRIP=${STRIP:-llvm-strip}
WORK=${WORK:-$(mktemp -d)}
OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
KEYSTORE=${KEYSTORE:-$WORK/lazyjones.keystore}
mkdir -p "$WORK/obj"

# ---- app values from build.gradle ---------------------------------------
GRADLE=$ROOT/android/app/build.gradle
val() { sed -n "s/^ *$1 *'\{0,1\}\([^' ]*\)'\{0,1\} *$/\1/p" "$GRADLE" | head -n 1; }
APP_ID=$(val applicationId)
MIN_SDK=$(val minSdk)
TARGET_SDK=$(val targetSdk)
VERSION_CODE=$(val versionCode)
VERSION_NAME=$(val versionName)
for v in APP_ID MIN_SDK TARGET_SDK VERSION_CODE VERSION_NAME; do
    eval "x=\$$v"
    [ -n "$x" ] || { echo "cannot read $v from $GRADLE" >&2; exit 1; }
done
echo "app $APP_ID $VERSION_NAME ($VERSION_CODE), minSdk $MIN_SDK, targetSdk $TARGET_SDK"

LIBDIR=$SYSROOT/usr/lib/aarch64-linux-android/$MIN_SDK
[ -f "$LIBDIR/libc.so" ] || { echo "sysroot has no API $MIN_SDK libraries: $LIBDIR" >&2; exit 1; }

# ---- native library -----------------------------------------------------
CPP=$ROOT/android/app/src/main/cpp
RT=$ROOT/runtime
SRC="$CPP/main.c $CPP/audio.c $RT/mem.c $RT/cpu_interp.c $RT/vic.c $RT/cia.c
     $RT/sid.c $RT/kernal.c $RT/basic_rnd.c $RT/machine.c $RT/palette.c
     $ROOT/frontend/fe.c"
if [ -n "$GEN" ]; then
    if [ ! -f "$GEN/lj_recomp.c" ] || [ ! -f "$GEN/lj_game_data.c" ]; then
        echo "no recompiled game in $GEN" >&2
        exit 1
    fi
    SRC="$SRC $GEN/lj_recomp.c $GEN/lj_game_data.c $RT/lj_game.c"
    echo "native code: with the recompiled game from $GEN"
else
    SRC="$SRC $RT/recomp_stub.c $RT/nogame.c"
    echo "native code: WITHOUT the game"
fi

TARGET=--target=aarch64-linux-android$MIN_SDK
# NDK default flags, then the flags of CMakeLists.txt
CFLAGS="$TARGET --sysroot=$SYSROOT -fPIC -DANDROID -fdata-sections -ffunction-sections
        -funwind-tables -fstack-protector-strong -no-canonical-prefixes
        -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security -g
        -std=c11 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
        -I$RT -I$ROOT/frontend -I$CPP"
OBJS=
for f in $SRC; do
    o=$WORK/obj/$(basename "$f" .c).o
    # shellcheck disable=SC2086
    $CLANG $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

SO=$WORK/liblazyjones.so
# shellcheck disable=SC2086
$CLANG $TARGET --sysroot="$SYSROOT" -shared -nostdlib -fuse-ld=lld \
    -Wl,-soname,liblazyjones.so -Wl,--no-undefined -Wl,--fatal-warnings \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,--hash-style=both \
    -u ANativeActivity_onCreate -Wl,--gc-sections \
    -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 \
    -o "$SO.debug" "$LIBDIR/crtbegin_so.o" $OBJS \
    -L"$LIBDIR" -landroid -llog -lEGL -lGLESv2 -lm -ldl -lc "$LIBDIR/crtend_so.o"
$STRIP --strip-unneeded -o "$SO" "$SO.debug"
echo "native library: $(wc -c < "$SO") bytes"

# ---- resources and manifest ---------------------------------------------
RES=$ROOT/android/app/src/main/res
rm -rf "$WORK/res" "$WORK/apk"
mkdir -p "$WORK/res" "$WORK/apk/lib/arm64-v8a"
aapt2 compile --dir "$RES" -o "$WORK/res/res.zip"
# Gradle puts the package name into the manifest; do the same here.
sed "s|<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">|<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"$APP_ID\">|" \
    "$ROOT/android/app/src/main/AndroidManifest.xml" > "$WORK/AndroidManifest.xml"
grep -q "package=\"$APP_ID\"" "$WORK/AndroidManifest.xml" || {
    echo "cannot set the package name in the manifest" >&2; exit 1; }
aapt2 link -o "$WORK/base.apk" -I "$RESJAR" --manifest "$WORK/AndroidManifest.xml" \
    --min-sdk-version "$MIN_SDK" --target-sdk-version "$TARGET_SDK" \
    --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
    "$WORK/res/res.zip"

# ---- package, align, sign -----------------------------------------------
cp "$SO" "$WORK/apk/lib/arm64-v8a/liblazyjones.so"
cp "$WORK/base.apk" "$WORK/unaligned.apk"
(cd "$WORK/apk" && zip -q -9 -X "$WORK/unaligned.apk" lib/arm64-v8a/liblazyjones.so)
zipalign -f -p 4 "$WORK/unaligned.apk" "$WORK/aligned.apk"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias lazyjones -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Lazy Jones port" >/dev/null 2>&1
    echo "made a new signing key: $KEYSTORE"
fi
apksigner sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --min-sdk-version "$MIN_SDK" --out "$OUT" "$WORK/aligned.apk"
apksigner verify --min-sdk-version "$MIN_SDK" "$OUT"
echo "APK ready: $OUT"
