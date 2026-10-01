#!/bin/sh
# emulator_test.sh APK - start test of the app on a running emulator.
#
# Installs the APK, starts it, checks that the native code runs and draws
# (the app without the game shows a text screen), then presses Back, which
# quits that screen. Writes logcat.txt and screen.raw for the CI artifacts.
set -eu

APK=${1:?usage: emulator_test.sh APK}
PKG=io.github.stubbyfowl.lazyjones
ACT=$PKG/android.app.NativeActivity

fail() {
    echo "FAIL: $*"
    adb logcat -d > logcat.txt 2>/dev/null || true
    exit 1
}

resumed() { # prints the resumed activity line
    adb shell dumpsys activity activities | grep -E "mResumedActivity|topResumedActivity" || true
}

adb wait-for-device
echo "device ABIs: $(adb shell getprop ro.product.cpu.abilist)"
adb install -r "$APK" || fail "install"
adb logcat -c
adb shell am start -W -n "$ACT" || fail "start"
sleep 20

adb logcat -d > logcat.txt
echo "---- app log"
grep -E "LazyJones|$PKG" logcat.txt | tail -n 50 || true
if grep -E "Fatal signal|FATAL EXCEPTION|UnsatisfiedLinkError|dlopen failed" logcat.txt | grep -q .; then
    grep -E -A 20 "Fatal signal|FATAL EXCEPTION|UnsatisfiedLinkError|dlopen failed" logcat.txt | head -n 60
    fail "crash or library load error"
fi
[ -n "$(adb shell pidof $PKG || true)" ] || fail "app is not running"
grep -q "game data missing in this build" logcat.txt || fail "native code did not start"
resumed | grep -q "$PKG" || fail "app is not in front"

# the screen must show the text (white and yellow on black)
adb exec-out screencap > screen.raw
python3 - <<'PY' || fail "screen is empty"
import struct, sys
d = open('screen.raw', 'rb').read()
w, h, fmt = struct.unpack_from('<III', d, 0)
# Android 8+ adds a colour space word after the format
off = 16 if len(d) >= 16 + w * h * 4 else 12
px = d[off:off + w * h * 4]
bright = sum(1 for i in range(0, len(px), 4 * 7) if px[i] > 160 and px[i + 1] > 160)
total = len(px) // (4 * 7)
print("screen %dx%d format %d, bright pixels %.2f %%" % (w, h, fmt, 100.0 * bright / total))
sys.exit(0 if bright > total * 0.002 else 1)
PY

# Back quits the "no game" screen
adb shell input keyevent KEYCODE_BACK
sleep 5
if resumed | grep -q "$PKG"; then
    fail "app still in front after Back"
fi
adb logcat -d > logcat.txt
echo "PASS: app started, drew its screen and quit with Back"
