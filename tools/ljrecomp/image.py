"""Load the game from a user supplied file and produce the memory image.

Supported inputs: .d64 disk images and .prg files of the C64 version of
Lazy Jones. Compressed releases are unpacked by running their own depacker
code (cpu6502.py). The result is the plain game: the bytes from $0801 up to
the end of the game and the entry point $080D, exactly as the game sees
memory when its first instruction runs.

No game data is stored in this repository. The only facts about the game
kept here are addresses, a few bytes of the start-up code used to recognise
the game, and hashes of known releases.
"""
import hashlib

from .cpu6502 import CPU

GAME_ENTRY = 0x080D
LOAD_ADDR = 0x0801
GAME_END = 0xA003      # first byte after the game (exclusive)

# First instructions of the game: LDA #$08 ; JSR $FFD2 ; LDA #$1D ; STA $D018
GAME_SIGNATURE = bytes([0xA9, 0x08, 0x20, 0xD2, 0xFF, 0xA9, 0x1D, 0x8D, 0x18, 0xD0])

# Byte ranges that differ between releases only because crackers changed
# texts or because a release was saved after the game had run (variables).
VARYING_RANGES = [
    (0x0801, 0x080D),   # BASIC line
    (0x2BA4, 0x2BAD),   # name of one room ("THE  TURK")
    (0x2E40, 0x2EE0),   # sprite data order in one release
    (0x3340, 0x3348),   # one character definition
    (0x540C, 0x540D),   # variable
    (0x54E6, 0x54E7),   # self modified operand (difficulty)
    (0x61E0, 0x61E2),   # variable
    (0x6220, 0x6222),   # variable
    (0x9F18, 0x9F5F),   # title screen credits
    (0x9FFF, 0xA003),   # end bytes
]

# SHA-1 of the image with VARYING_RANGES cleared, for releases that were
# tested with this port.
KNOWN_RELEASES = {
    '2f27b39aeb46dce90c25ddc9d3fc493a1e83d636':
        'Lazy Jones (1984, Terminal Software); tested with the DKS, '
        'Section 8 and CMM releases',
}


class GameFileError(Exception):
    pass


# ---- D64 ----------------------------------------------------------------

_SPT = [0] + [21] * 17 + [19] * 7 + [18] * 6 + [17] * 10


def _d64_offset(track, sector):
    if track < 1 or track > 40 or sector >= _SPT[track]:
        raise GameFileError('bad track/sector %d/%d' % (track, sector))
    off = sum(_SPT[1:track]) + sector
    return off * 256


def _d64_chain(data, track, sector):
    out = bytearray()
    seen = set()
    while track:
        if (track, sector) in seen:
            raise GameFileError('file sector chain loops')
        seen.add((track, sector))
        o = _d64_offset(track, sector)
        blk = data[o:o + 256]
        if len(blk) < 256:
            raise GameFileError('disk image too short')
        if blk[0] == 0:
            out += blk[2:blk[1] + 1]
            break
        out += blk[2:]
        track, sector = blk[0], blk[1]
    return bytes(out)


def d64_files(data):
    """Return [(name, type, bytes)] for the files on a D64 image."""
    if len(data) not in (174848, 175531, 196608, 197376):
        raise GameFileError('not a D64 image (size %d)' % len(data))
    bam = _d64_offset(18, 0)
    t, s = data[bam], data[bam + 1]
    files = []
    seen = set()
    while t and (t, s) not in seen:
        seen.add((t, s))
        o = _d64_offset(t, s)
        blk = data[o:o + 256]
        for i in range(8):
            e = blk[i * 32:i * 32 + 32]
            ftype = e[2]
            if ftype & 0x07 == 0 and not (ftype & 0x80):
                continue
            name = bytes(e[5:21]).rstrip(b'\xa0')
            ft, fs = e[3], e[4]
            try:
                content = _d64_chain(data, ft, fs) if ft else b''
            except GameFileError:
                continue
            files.append((name, ftype & 0x07, content))
        t, s = blk[0], blk[1]
    return files


# ---- PRG and unpacking ---------------------------------------------------

def basic_sys_address(mem):
    """Return the SYS address of a BASIC line at $0801, or None."""
    p = LOAD_ADDR + 4
    while p < LOAD_ADDR + 40 and mem[p] == 0x20:
        p += 1
    if mem[p] != 0x9E:   # SYS token
        return None
    p += 1
    while mem[p] in (0x20, 0x28):
        p += 1
    digits = ''
    while 0x30 <= mem[p] <= 0x39:
        digits += chr(mem[p])
        p += 1
    return int(digits) if digits else None


def unpack(prg, max_steps=20000000):
    """Load a PRG into flat memory and run it until the game entry point.

    Returns the 64 KiB memory as it is when the game's first instruction is
    about to run."""
    if len(prg) < 3:
        raise GameFileError('PRG file too short')
    la = prg[0] | (prg[1] << 8)
    mem = bytearray(0x10000)
    body = prg[2:]
    if la + len(body) > 0x10000:
        raise GameFileError('PRG does not fit into memory')
    mem[la:la + len(body)] = body
    if mem[GAME_ENTRY:GAME_ENTRY + len(GAME_SIGNATURE)] == GAME_SIGNATURE:
        return mem
    if la != LOAD_ADDR:
        raise GameFileError('unknown file: loads at $%04X' % la)
    start = basic_sys_address(mem)
    if start is None:
        raise GameFileError('unknown file: no BASIC SYS line')
    # Run the depacker with the machine state of BASIC's SYS command.
    mem[0x00] = 0x2F
    mem[0x01] = 0x37
    cpu = CPU(mem)
    cpu.pc = start
    cpu.sp = 0xF6
    while cpu.steps < max_steps:
        if cpu.pc == GAME_ENTRY and mem[GAME_ENTRY:GAME_ENTRY + len(GAME_SIGNATURE)] == GAME_SIGNATURE:
            return mem
        if cpu.pc >= 0xA000:
            # A ROM call before the game started (for example a trainer
            # menu). Accept the image if the game is already unpacked.
            if mem[GAME_ENTRY:GAME_ENTRY + len(GAME_SIGNATURE)] == GAME_SIGNATURE:
                return mem
            raise GameFileError('depacker called ROM at $%04X before the game was unpacked' % cpu.pc)
        cpu.step()
    raise GameFileError('depacker did not finish')


def normalized_hash(mem):
    m = bytearray(mem[LOAD_ADDR:GAME_END])
    for lo, hi in VARYING_RANGES:
        for a in range(lo, hi):
            m[a - LOAD_ADDR] = 0
    return hashlib.sha1(bytes(m)).hexdigest()


# ---- text fixes -----------------------------------------------------------

def _petscii(s):
    return bytes(ord(c) for c in s)


def restore_texts(mem, log):
    """Undo cracker text changes.

    The originals were checked against the releases that kept them and
    against screenshots of the original game (C64-Wiki), character by
    character with the game's own font.
    """
    # Room name: 9 characters at $2BA4-$2BAC, then HOME ($13) at $2BAD.
    # The DKS release wrote "DKS INC" there; Section 8 and CMM kept
    # "THE  TURK" (two spaces).
    room = mem[0x2BA4:0x2BAD]
    if room != _petscii('THE  TURK'):
        mem[0x2BA4:0x2BAD] = _petscii('THE  TURK')
        log('restored room name "THE  TURK"')
    # Title credits: 72 bytes at $9F18-$9F5F, printed after "LAZY JONES";
    # a zero byte at $9F60 ends the string. All releases keep the frame
    # (cursor moves, "BY DAVID WHITTAKER", end with HOME) and replaced the
    # second line with their crack group. The original second line (row 17):
    #   (c) TERMINAL SOFTWARE INTL. LTD MCMLXXXIV
    # with the (c) in light blue, the name in light red and the year in light
    # blue. In the game's font, screen code $40 (PETSCII $C0) is the (c)
    # sign and screen code $00 (PETSCII $40, "@") is the dot after "INTL".
    # These bytes fill the 72 bytes exactly.
    old = bytes(mem[0x9F18:0x9F60])
    if len(old) == 72 and old[-1] == 0x13 and old[:8] == bytes([0x1D] * 5 + [0x11] * 3) \
            and mem[0x9F60] == 0x00:
        new = bytearray()
        new += bytes([0x1D] * 5 + [0x11] * 3)          # same start as before
        new += _petscii('BY DAVID WHITTAKER')          # yellow, row 14, columns 11-28
        new += bytes([0x8D, 0x11, 0x11])                # row 17, column 0
        new += bytes([0x9A, 0xC0, 0x20])                # light blue (c), space
        new += bytes([0x96])                            # light red
        new += _petscii('TERMINAL SOFTWARE INTL') + bytes([0x40]) + _petscii(' LTD ')
        new += bytes([0x9A]) + _petscii('MCMLXXXIV')    # light blue
        new += bytes([0x13])                            # HOME
        if len(new) != 72:
            raise GameFileError('credits rebuild has %d bytes, not 72' % len(new))
        mem[0x9F18:0x9F60] = new
        log('restored the original title credits')
    return mem


def load_game(path, log=print, fix_texts=True):
    """Return (memory image bytes $0801-$A002, info dict)."""
    data = open(path, 'rb').read()
    lower = path.lower()
    candidates = []
    if lower.endswith('.d64') or len(data) in (174848, 175531, 196608, 197376):
        for name, ftype, content in d64_files(data):
            if ftype == 2 and len(content) > 1000:
                candidates.append((name.decode('latin-1', 'replace'), content))
        if not candidates:
            raise GameFileError('no program file on the disk image')
    else:
        candidates.append((path, data))
    errors = []
    for name, prg in candidates:
        try:
            mem = unpack(prg)
        except GameFileError as e:
            errors.append('%s: %s' % (name, e))
            continue
        h = normalized_hash(mem)
        release = KNOWN_RELEASES.get(h, 'unknown release')
        log('found the game in "%s" (%s, image hash %s)' % (name, release, h[:12]))
        if fix_texts:
            restore_texts(mem, log)
        info = {'file': name, 'release': release, 'hash': h,
                'load': LOAD_ADDR, 'entry': GAME_ENTRY}
        return bytes(mem[LOAD_ADDR:GAME_END]), info
    raise GameFileError('Lazy Jones was not found in %s (%s)' % (path, '; '.join(errors)))
