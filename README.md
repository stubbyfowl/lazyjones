# Lazy Jones for Android (arm64)

A native Android port of **Lazy Jones**, the Commodore 64 game by David
Whittaker (Terminal Software, 1984).

The 6502 game code is **statically recompiled** to C and then compiled to
arm64 machine code. A small C64 runtime written in C (VIC-II video, SID
sound, CIA timers, a replacement KERNAL) does the work of the C64 hardware.
The app is one native library in a NativeActivity: no Java code, no
emulator app. It runs on Android phones, tablets, TVs and Android
handhelds, with gamepad support.

> **The game is not in this repository.** Lazy Jones is copyrighted. You
> need your own copy of the game (a `.d64` disk image or a `.prg` file).
> The build reads it and puts the recompiled game into your APK. Do not
> publish an APK that contains the game.

## Features

- Native arm64 code (`arm64-v8a`), Android 7.0 (API 24) and newer.
  Ready for devices with 16 KB memory pages.
- Recompiled game: 9134 instructions of the game are C code. Code that the
  game changes at run time goes through an interpreter that uses the same
  instruction code, so both paths give the same result.
- Exact PAL timing: 50.125 frames per second, cycle-exact 6510 CPU, VIC-II
  bad lines and sprite timing, SID sound with the 6581 filter.
- Controllers: D-pad, analog stick, face and shoulder buttons. Also a
  keyboard and the touch screen.
- Menu: save state, load state, reset, screen size, filter, border size,
  sound and volume, B button function. The game is saved when you leave
  the app and continues when you come back.
- Picture: sharp pixels at any screen size ("sharp bilinear"), correct PAL
  pixel aspect, or integer scaling, or full screen stretch.
- Asks for a 50 Hz display mode where the device has one (Android 11+),
  for smooth scrolling.

## Controls

| Gamepad | Keyboard | Function |
|---|---|---|
| D-pad or left stick | Arrow keys, W A S D | Joystick (C64 port 2) |
| A, X, Y (and B, see menu) | Space, Enter, Ctrl | Fire |
| START | P | Pause the game (C64 key P) |
| R1 or R2 | M | Music on or off (C64 key M) |
| SELECT, Back, Menu, Guide | Esc | Open or close the menu |

In the menu: up and down select a line, A does the action, left and right
change a setting, B or SELECT closes the menu.

Touch screen: the left half of the screen is a joystick (put your finger
down and move it), the right half is fire. Tap the top right corner to
open the menu, then tap a menu line.

## Build the app

You need your copy of the game and Python 3. Put the game file into the
folder `game/` (or give its path with `-PljGame=...`, see below).

### With Android Studio or Gradle (all systems)

1. Install the Android SDK and the NDK (Android Studio: SDK Manager, "NDK
   (Side by side)" and "CMake").
2. Put your `.d64` or `.prg` file into `game/`.
3. Build:

   ```sh
   cd android
   ./gradlew assembleRelease
   ```

   Or with the game file somewhere else:

   ```sh
   ./gradlew assembleRelease -PljGame=/path/to/LazyJones.d64
   ```

4. The APK is `android/app/build/outputs/apk/release/app-release.apk`.

Gradle runs the recompiler (`tools/ljrecomp`) before it compiles the
native code. Without a game file the build still works, but the app only
shows a message.

### Without the NDK (Ubuntu 24.04)

`tools/apkbuild` builds the same APK with Ubuntu packages only. It makes a
small Android sysroot from AOSP source files (headers and symbol lists).

```sh
sudo apt-get install clang lld llvm aapt apksigner zipalign android-libnativehelper-dev
tools/apkbuild/fetch_sources.sh ~/aosp-src
python3 tools/apkbuild/mksysroot.py --src ~/aosp-src --out ~/lj-sysroot
python3 tools/ljrecomp build --game game/YourCopy.d64 --out build/gen
tools/apkbuild/build_apk.sh --sysroot ~/lj-sysroot --res-jar ~/aosp-src/android-res.jar \
    --gen build/gen --keystore ~/lazyjones.keystore --out LazyJones.apk
```

Keep the keystore file. Android only installs an update when it has the
same signing key as the app on the device. (A new key means: uninstall
first, and the saved game is lost.)

## Install the app

- With a USB cable: `adb install LazyJones.apk`
- Or copy the APK to the device and open it in a file manager. Android asks
  you to allow installs from that app.

## Game files that were tested

The recompiler finds the game in a disk image or a program file and checks
it. These releases were tested: the DKS, Section 8 (S8) and CMM disk
images. Other copies are not tested: for them the build shows "unknown
release". They work only if their game code is the same.

These cracked releases changed some text. The build puts the original text
back: the title shows "BY DAVID WHITTAKER / TERMINAL SOFTWARE 1984", and
the name of the room "THE  TURKS" is correct. Use
`python3 tools/ljrecomp build --no-text-fix ...` to keep the text of your
copy.

## How it works

```
your .d64/.prg --> tools/ljrecomp --> build/gen/lj_recomp.c    (game code as C)
                                      build/gen/lj_game_data.c (game memory image)
runtime/   C64 hardware in C: CPU, VIC-II, SID, CIA, memory, KERNAL
frontend/  menu, settings, save states, input mapping (portable C)
android/   NativeActivity: OpenGL ES 2 video, AAudio/AudioTrack sound, input
```

1. **Unpack.** The game on disk is packed. The recompiler runs the
   unpacker on a small 6502 CPU written in Python and takes the game from
   memory when it starts.
2. **Code map.** `tools/ljrecomp/data/lazyjones_codemap.json` lists the
   start address of every game instruction (addresses only, no game data).
   It was made with a coverage-guided explorer (`host/ljexplore`) that
   played the game and reached all 15 mini-games, plus static analysis.
3. **Recompile.** Each instruction becomes a block of C code that uses the
   same instruction macro as the interpreter (`runtime/cpu_ops_gen.h`,
   generated from one opcode table). Jumps become `goto` inside a code
   block. Interrupts and timer events are checked between instructions, so
   the timing stays exact.
4. **Self-modifying code.** The game changes some instructions while it
   runs. The runtime watches writes to code. A changed instruction runs in
   the interpreter, and the 9 places where the game changes only an
   operand read the operand from memory.
5. **KERNAL.** The C64 ROMs are not used. The runtime has its own small
   KERNAL and the BASIC random number routine, written to give the same
   results as the original ROM code.

## Tests

| Test | Result |
|---|---|
| Klaus Dormann 6502 functional test | pass |
| SingleStepTests 6502 (all 244 opcodes that do not halt the CPU, 2,440,000 tests: registers, memory, cycle count) | pass |
| Recompiled code against the interpreter, 12,000 frames of random play | same state every frame |
| arm64 build (qemu) against the x86-64 build, 12,000 frames | same state and same sound, bit for bit |
| Front end: menu, save and load, settings, autosave, input, volume | 84 checks pass (the game part also as arm64 code) |
| CI: Gradle and NDK build, build without NDK, start test on the Android emulator | see `.github/workflows/build.yml` |

See [docs/TESTING.md](docs/TESTING.md) for the details and for how to run
the tests with your copy of the game.

## Files

| Folder | Content |
|---|---|
| `runtime/` | C64 runtime (C11) |
| `frontend/` | Menu, settings, save states |
| `android/` | Android app (Gradle project, NativeActivity in C) |
| `tools/ljrecomp/` | Recompiler (Python 3, no extra packages) |
| `tools/apkbuild/` | APK build without the NDK |
| `tools/gen_cpu_ops.py` | Makes the instruction macros from the opcode table |
| `host/` | Host programs: game runner with screenshots and WAV output, explorer, tests |
| `tests/` | CPU tests and front end tests |
| `game/` | Put your game file here (not in git) |

## Credits

- Lazy Jones: David Whittaker, Terminal Software, 1984.
- VIC-II timing: "The MOS 6567/6569 video controller (VIC-II) and its
  application in the Commodore 64" by Christian Bauer.
- SID envelope and filter behavior follows the published reSID research
  by Dag Lem.
- Colodore palette by Philip "Pepto" Timmermann.
- 8x8 font: public domain, by Daniel Hepper.
- 6502 tests: Klaus Dormann; SingleStepTests by Tom Harte and others.
