#!/bin/sh
# fetch_sources.sh - get the AOSP files that mksysroot.py needs.
#
# The files come from the LineageOS mirrors of AOSP (Android 15) at fixed
# commits. Only headers and symbol files are fetched (sparse, no history).
# jni.h comes from the Debian/Ubuntu package android-libnativehelper-dev.
#
# Use: fetch_sources.sh <dir>
set -eu

DIR=${1:?usage: fetch_sources.sh <dir>}
mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)

BIONIC_COMMIT=53baa8eac64027ea286e7d98502e785a7bb8a254
NATIVE_COMMIT=63aa0f3b219f16018e3b99ea7ef2256bc94e4782
BASE_COMMIT=e86c9bed834f7c24ba2df4c5e21ab03b5b942bfc
LOGGING_COMMIT=6d89850d1feec974bf2943013d16a9237c0debbf
AV_COMMIT=c2d9d7b479e84a27c09468a41e39d7c6c58ae748
BRANCH=lineage-22.1
RAW=https://raw.githubusercontent.com/LineageOS

sparse() { # repo dir commit paths...
    repo=$1; dir=$2; commit=$3; shift 3
    if [ ! -d "$DIR/$dir/.git" ]; then
        git clone -q --filter=blob:none --sparse --no-checkout --depth 1 \
            -b "$BRANCH" "https://github.com/LineageOS/$repo" "$DIR/$dir"
    fi
    git -C "$DIR/$dir" sparse-checkout set --no-cone "$@"
    if ! git -C "$DIR/$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
        git -C "$DIR/$dir" fetch -q --filter=blob:none --depth 1 origin "$commit"
    fi
    git -C "$DIR/$dir" checkout -q "$commit"
}

sparse android_bionic bionic "$BIONIC_COMMIT" \
    '/libc/include/' '/libc/kernel/uapi/' '/libc/kernel/android/' \
    '/libc/arch-common/' '/libc/private/bionic_asm*.h' \
    '/libc/libc.map.txt' '/libm/libm.map.txt' '/libdl/libdl.map.txt' '/libc/NOTICE'

sparse android_frameworks_native native "$NATIVE_COMMIT" \
    '/include/android/' '/libs/arect/include/' '/libs/nativewindow/include/' \
    '/opengl/include/' '/opengl/libs/*.map.txt' '/NOTICE'

mkdir -p "$DIR/misc"
get() { # repo commit path
    curl -sSf --retry 4 -o "$DIR/misc/$(basename "$3")" "$RAW/$1/$2/$3"
}
get android_frameworks_base "$BASE_COMMIT"    native/android/libandroid.map.txt
get android_system_logging  "$LOGGING_COMMIT" liblog/include/android/log.h
get android_system_logging  "$LOGGING_COMMIT" liblog/liblog.map.txt
get android_frameworks_av   "$AV_COMMIT"      media/libaaudio/include/aaudio/AAudio.h

# Framework resources for "aapt2 link -I", from the Robolectric android-all
# jar (built from AOSP sources, Apache 2.0). Android 14 (API 34): the aapt2
# of Ubuntu 24.04 cannot read the newer API 35 table. Public resource IDs
# never change, so the linked app is the same.
ROBO_V=14-robolectric-10818077
ROBO_SHA1=94b1490a891e9be559aa35c87cd8a0c163f32d83
ROBO_PATH=org/robolectric/android-all/$ROBO_V/android-all-$ROBO_V.jar
if [ ! -f "$DIR/android-res.jar" ]; then
    # (a copy of the jar that is already in the dir is used as it is)
    [ -f "$DIR/android-all.jar" ] ||
    curl -sSf --retry 4 -o "$DIR/android-all.jar" \
        "https://maven-central.storage-download.googleapis.com/maven2/$ROBO_PATH" ||
    curl -sSf --retry 4 -o "$DIR/android-all.jar" "https://repo1.maven.org/maven2/$ROBO_PATH"
    echo "$ROBO_SHA1  $DIR/android-all.jar" | sha1sum -c --quiet
    python3 - "$DIR/android-all.jar" "$DIR/android-res.jar" <<'PY'
import sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED) as out:
    for n in ('AndroidManifest.xml', 'resources.arsc'):
        out.writestr(n, src.read(n))
PY
    rm -f "$DIR/android-all.jar"
fi

JNI=/usr/include/android/jni.h
if [ ! -f "$JNI" ]; then
    echo "missing $JNI: install the package android-libnativehelper-dev" >&2
    exit 1
fi
cp "$JNI" "$DIR/misc/jni.h"
echo "sources ready in $DIR"
