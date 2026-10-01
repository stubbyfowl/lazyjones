# Testing

This file tells what is tested, how, and how you run the tests yourself.

The game is not in the repository, so CI tests everything except the game
itself. The tests with the game run on your computer with your copy.

## Tests without the game (CI runs these)

Build the host programs and tests (Linux or macOS, any C11 compiler):

```sh
make -C host all tests
```

| Command | What it checks |
|---|---|
| `tests/cpu_klaus 6502_functional_test.bin` | Klaus Dormann's 6502 functional test: all documented instructions and flags, decimal mode. Ends with `PASS`. |
| `tests/cpu_ss DIR` | SingleStepTests 6502 vectors: registers, memory and cycle count for each instruction. Convert the JSON files first: `python3 tests/ss_convert.py JSON_DIR DIR`. The 12 JAM opcodes are not compared (the CPU halts there). |
| `host/fetest_nogame DIR` | Front end without the game: message screen, Back quits. |
| `python3 tools/gen_cpu_ops.py \| cmp - runtime/cpu_ops_gen.h` | The instruction macros are up to date with the opcode table. |

Where to get the test data (CI uses these fixed versions):

- Klaus Dormann: `bin_files/6502_functional_test.bin` from
  github.com/Klaus2m5/6502_65C02_functional_tests (commit `7954e2d`).
- SingleStepTests: `6502/v1/*.json` from github.com/SingleStepTests/65x02
  (commit `2f6980a`). CI uses 69 of the files (decimal mode, undocumented
  opcodes, control flow, stack, page crossing). All 244 files that do not
  halt the CPU pass: 2,440,000 tests.

CI (`.github/workflows/build.yml`) also:

- runs the host tests on x86-64 and on arm64 (native arm64 runner),
- builds the Android app with Gradle and the NDK and checks the APK
  (signature, alignment, 16 KB aligned segments of the native library),
- builds the app with `tools/apkbuild` (no NDK) and checks that the native
  library imports the same symbols and that the app data is the same,
- installs the app on the Android emulator (Android 11, ARM code runs
  through the emulator's ARM translation), checks that it starts, draws
  its screen and quits with Back.

## Tests with your copy of the game

Make the recompiled sources and the game programs:

```sh
python3 tools/ljrecomp build --game game/YourCopy.d64 --out build/gen
make -C host ljrun_game fetest
```

### Front end

```sh
host/fetest /tmp/fetest
```

79 checks: menu, save state and load state (the machine state after a
load is exactly the saved state), refused state files from another build,
reset, settings saved to a file, volume, B button, touch, stick, autosave
and resume, quit, and that the same input gives the same result.

### Recompiled code against the interpreter

`ljrun_game` runs the recompiled game. With `--interp` it runs the same
game in the interpreter only. Both must give the same machine state in
every frame:

```sh
# random input for 12000 frames (4 minutes of play)
python3 - > /tmp/script.txt <<'EOF'
import random
random.seed(1)
f = 400
while f < 12000:
    print(f, 'joy2', random.choice([0, 1, 2, 4, 8, 16, 5, 6, 9, 10]))
    f += random.randint(5, 60)
EOF
host/ljrun_game --game --frames 12000 --script /tmp/script.txt --hash > /tmp/recomp.txt
host/ljrun_game --game --frames 12000 --script /tmp/script.txt --hash --interp > /tmp/interp.txt
cmp /tmp/recomp.txt /tmp/interp.txt && echo same
```

The hash covers RAM, colour RAM, CPU registers, the clock, the picture and
the VIC-II and SID registers.

### arm64 code against x86-64 code

The same program built for arm64 Linux and run with qemu must give the
same hashes and the same sound (bit for bit):

```sh
sudo apt-get install qemu-user-static libc6-dev-arm64-cross libgcc-13-dev-arm64-cross
RT=runtime
clang --target=aarch64-linux-gnu -fuse-ld=lld -static -O2 -ffp-contract=off -std=c11 -I$RT \
    -o /tmp/ljrun_arm64 host/ljrun.c $RT/mem.c $RT/cpu_interp.c $RT/vic.c $RT/cia.c $RT/sid.c \
    $RT/kernal.c $RT/basic_rnd.c $RT/machine.c $RT/palette.c $RT/lj_extras.c $RT/trace.c $RT/lj_game.c \
    build/gen/lj_recomp.c build/gen/lj_game_data.c -lm
host/ljrun_game --game --frames 12000 --script /tmp/script.txt --hash --wav /tmp/x86.wav > /tmp/x86.txt
qemu-aarch64-static /tmp/ljrun_arm64 --game --frames 12000 --script /tmp/script.txt --hash --wav /tmp/arm.wav > /tmp/arm.txt
cmp /tmp/x86.txt /tmp/arm.txt && cmp /tmp/x86.wav /tmp/arm.wav && echo same
```

`-ffp-contract=off` is important: without it the compiler uses fused
multiply-add on arm64, and the sound differs in the lowest bit in some
samples. The Android build uses this flag too.

### Pictures and sound

```sh
host/ljrun_game --game --frames 600 --shotevery 100 --out /tmp/shot --wav /tmp/title.wav
```

writes a picture (PPM) every 100 frames and the sound as a WAV file.

## Results at the time of writing

| Test | Result |
|---|---|
| Klaus Dormann functional test | pass |
| SingleStepTests, 244 opcodes, 2,440,000 tests | pass |
| Front end (game build and no-game build) | 84 of 84 checks pass, game part also as arm64 code |
| Recompiled against interpreter, 12,000 frames of random play | same state in every frame |
| arm64 (qemu) against x86-64, 12,000 frames | same state in every frame, same sound |
| DKS, Section 8 and CMM releases | build and run |
