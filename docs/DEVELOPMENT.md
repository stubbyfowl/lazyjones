# Development log

This file tells how the port was made, in order: what was done, why, and
which problems were found and how they were fixed. The git history has the
same steps as commits; this file gives the reasons.

## 1. The game files

The work started from three C64 disk images of Lazy Jones, all cracked
releases: **DKS**, **Section 8** and **CMM**. (A ZX Spectrum `.z80` file and
a `.sid` music file were also available; the C64 port uses only the disk
images.) The game files are copyrighted and are never in the repository.

## 2. Unpacking the game (`tools/ljrecomp/image.py`)

- The disk images are read directly (D64 format: directory track 18).
- The program on the disk is packed. Instead of writing an unpacker, the
  recompiler runs the packed program on a small 6502 CPU written in Python
  (`cpu6502.py`) until the game's start code is in memory at `$080D`
  (signature `A9 08 20 D2 FF A9 1D 8D 18 D0`).
- To identify the game, some bytes that differ between releases are
  cleared (the BASIC line, a room name, some data and variables) and the
  rest is hashed (SHA-1). All three releases give the same hash, so they
  contain the same game code.
- The cracked releases changed some text. The build puts the original text
  back: the title credits ("BY DAVID WHITTAKER / TERMINAL SOFTWARE 1984",
  rebuilt with exactly the same length and cursor codes, 72 bytes) and the
  room name "THE  TURKS".

## 3. The C64 runtime (`runtime/`)

- **CPU.** One opcode table (`tools/ljrecomp/opcodes.py`) makes the
  instruction macros (`runtime/cpu_ops_gen.h`) for both the interpreter and
  the recompiled code, so both always do the same thing. Every bus access
  happens at its exact cycle, including dummy reads and writes. NMOS
  decimal mode and the undocumented opcodes are included.
- **VIC-II.** The picture is drawn line by line. When the game writes a
  VIC-II register in the middle of a line, the picture is first drawn up
  to that point. Bad lines, sprite DMA, sprite collisions and the borders
  follow the timing in Christian Bauer's VIC-II article.
- **SID.** Envelopes and the 6581 filter follow the published reSID
  research. The output goes through a DC blocker.
- **CIA.** Timers are computed when they are read, not every cycle. The
  time-of-day clock and the keyboard matrix (with ghosting) are included.
- **KERNAL.** The Commodore ROMs are not used. The runtime has its own ROM
  image: the KERNAL calls that the game uses are traps (opcode `$02`, which
  halts a real 6502) into C code. The screen editor and the keyboard scan
  were written to work like the ROM. The BASIC random number routine
  (`RND`) was rewritten in C bit for bit, including a carry quirk of the
  original multiply routine.
- **Start.** The machine state at start is the state that BASIC leaves when
  it starts the game with `SYS`: stack, vectors, zero page, random seed.

## 4. Checking the runtime

- Klaus Dormann's 6502 functional test passes.
- The SingleStepTests 6502 vectors pass: 2,440,000 instructions with
  registers, memory and cycle count. (The 12 JAM opcodes are not compared;
  the test data expects the program counter to move on after them.)
- `RND` was compared with the real ROM routine (locally; the ROM is not in
  the repository).
- The title, controls, lives question and hotel screens were compared with
  a reference C64 emulator (locally).

## 5. The recompiler (`tools/ljrecomp/`)

- **Explorer** (`host/ljexplore.c`). The game runs in the interpreter with
  tracing on. From a growing set of saved machine states, short pieces of
  play with random input continue; a state is kept when it reached new
  code. The explorer reached all 15 mini-games.
- **Code map** (`tools/ljrecomp/data/lazyjones_codemap.json`). The start
  address of every instruction: 8,662 seen in the traces, 463 more found by
  static analysis of the code next to them, total 9,134. 9 of them have an
  operand that the game changes while it runs. The file holds addresses
  only, no game bytes.
- **Code generation** (`codegen.py`). The code is cut into 2 KB blocks.
  Each instruction gets a label and becomes the same macro that the
  interpreter uses. Jumps become `goto`. Between instructions the code
  checks for interrupts and timer events, so the timing stays exact.
- **Self-modifying code.** The runtime knows which bytes are compiled code.
  When the game writes to one of them, that instruction runs in the
  interpreter until the bytes are the compiled ones again. The 9 changing
  operands are read from memory.
- **Check.** The recompiled game and the interpreter give the same machine
  state in every one of 12,000 frames of random play.

## 6. Front end and Android app

- `frontend/` is portable C: menu, settings, save states, input mapping.
- `android/` is a NativeActivity in C: a game thread with EGL and OpenGL ES
  2, AAudio (Android 9 and newer, loaded at run time) or AudioTrack through
  JNI, a lock-free ring buffer for sound, and a rate control that changes
  the sound speed by at most 0.5 % to keep 50 ms of sound queued.
- The app asks for a 50 Hz display mode (Android 11 and newer), hides the
  system bars, and handles gamepads, keyboards and touch.

## 7. Building without the NDK (`tools/apkbuild/`)

The development machine could not download the Android NDK. So the APK is
also built from AOSP source files: bionic headers, the NDK API headers and
the symbol lists (`*.map.txt`). `mksysroot.py` makes stub libraries with
the same rules as the NDK (architecture tags, `introduced=`, `versioned=`,
`unversioned_until`). `build_apk.sh` compiles with clang, links with lld,
and packages with aapt2, zipalign and apksigner. CI checks that this build
and the real Gradle/NDK build import the same symbols.

## 8. Enhancements

The game was studied in memory to find:

| Address | What it is | How it was found |
|---|---|---|
| `$0366` | lives left | RAM compared at 5, 4 and 3 lives |
| `$0FDF` | `DEC $0366`, the only place a life is taken | search for code that uses `$0366` |
| `$0360-$0362` | score, BCD | the routine that writes the score digits to the screen |
| `$0363-$0365` | high score, BCD | the same routine, "HI" digits |
| `$0823` | high score cleared, once at start | all writes to `$0363-$0365` |
| `$9D80-$9DB1` | copy score to high score if higher | called after points (`$1584`) and at game over (`$101A`) |

With these, the front end adds infinite lives (the `DEC` becomes an `LDA` of
the same length), a saved high score, fast forward, auto fire, button
mapping and four save slots. `runtime/lj_extras.c` checks the game code
first and turns the hooks off for an unknown copy.

## 9. Problems found and fixed

| Problem | How it was found | Fix |
|---|---|---|
| Title text drifted on the screen | first screenshots | VC base counter reset at the top of each frame |
| Wrong order of sprite and bad line stalls | screenshots, timing | separate stall fields for sprites and bad lines |
| First display line wrong when the border opened in mid line | screenshots | draw up to the border change first |
| `JSR` with the stack over its own operand | SingleStepTests | read the high address byte after the pushes, like the real CPU |
| `RND` results different | comparison with the ROM routine | V flag and the multiply carry quirk |
| Keyboard scan different from the ROM | comparison with the ROM | rewritten to follow the ROM exactly |
| Sound rate control worked the wrong way | code review | sign of the correction |
| App hung on quit | code review | release the window before `finish()` |
| Title credits 71 instead of 72 bytes | screenshot | rebuilt with the exact length |
| `AWINDOW_FLAG_*` not defined | first build with real NDK headers | include `<android/window.h>` |
| Sound ring buffer: two threads moved the read index | code review | only the audio thread moves it; the game thread asks for a flush |
| Stick or touch "stuck" after leaving the app | code review | clear all input on pause and focus loss |
| Sound differed in the lowest bit on arm64 | arm64 against x86-64 comparison | `-ffp-contract=off` (no fused multiply-add) |
| Back did not quit (key down and up in one frame) | emulator test in CI | key presses are kept until a frame sees them |
| Symbol versions on `libGLESv2`, `libEGL`, `libandroid`, `liblog` (could stop the app loading) | CI comparison with the NDK build | unversioned stubs, as the NDK makes them |
| A half-copied high score could be saved | thinking about frame timing, then a test | save only a value that is the same in two frames |

## 10. Testing method

- The recompiled code is checked against the interpreter, and the arm64
  build against the x86-64 build, frame by frame with a hash of the machine
  state.
- The enhancement tests were checked by breaking each feature on purpose
  (mutation testing): for example no high score restore, cheat ignored,
  wrong auto fire pattern. Each break made checks fail. One break (no wait
  for a stable high score) was not caught at first; a test for it was
  added.
- See [TESTING.md](TESTING.md) for all tests and how to run them.

## 11. What is not done

- The port was not tested on a real phone or handheld by the developer; CI
  tests it on the Android emulator (no-game build).
- The recompiled C is generated, not hand-written: this is a
  recompilation, not a decompilation (see the README).
