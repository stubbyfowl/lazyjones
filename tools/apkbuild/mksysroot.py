#!/usr/bin/env python3
"""
mksysroot.py - make a small arm64 Android sysroot from AOSP sources.

This is for systems without the Android NDK. It makes the same parts that
the NDK sysroot has for the libraries this app uses:

  usr/include/...                       headers (bionic, NDK APIs, EGL, GLES)
  usr/lib/aarch64-linux-android/<api>/  stub libraries and crt objects

The stub libraries only hold symbol names and symbol versions. They are
made from the AOSP symbol files (*.map.txt) with the same rules that the
NDK uses (arch tags, introduced=, versioned=, platform-only and mode tags).
At run time the app links to the real libraries of the device.

fetch_sources.sh gets the source files. Use:

  mksysroot.py --src <sources dir> --out <sysroot dir> [--api 24]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ARCH = 'arm64'
TRIPLE = 'aarch64-linux-android'
ALL_ARCHES = ('arm', 'arm64', 'x86', 'x86_64', 'riscv64', 'mips', 'mips64')

# API levels of the code names that the symbol files use.
CODENAMES = {
    'G': 9, 'I': 14, 'J': 16, 'J-MR1': 17, 'J-MR2': 18, 'K': 19, 'L': 21,
    'L-MR1': 22, 'M': 23, 'N': 24, 'N-MR1': 25, 'O': 26, 'O-MR1': 27,
    'P': 28, 'Q': 29, 'R': 30, 'S': 31, 'S-V2': 32, 'Tiramisu': 33,
    'UpsideDownCake': 34, 'VanillaIceCream': 35, 'Baklava': 36,
}
FUTURE = 10000
MODE_TAGS = ('llndk', 'apex', 'systemapi')

# library name -> (symbol file relative to the sources dir, versioned)
# "versioned" follows the ndk_library modules in the AOSP Android.bp files:
# libandroid, liblog, libEGL and libGLESv2 have unversioned_until: "current",
# so their NDK stubs have no symbol versions at all. libc, libm and libdl
# use the default and are versioned.
LIBS = {
    'libc': ('bionic/libc/libc.map.txt', True),
    'libm': ('bionic/libm/libm.map.txt', True),
    'libdl': ('bionic/libdl/libdl.map.txt', True),
    'liblog': ('misc/liblog.map.txt', False),
    'libandroid': ('misc/libandroid.map.txt', False),
    'libEGL': ('native/opengl/libs/libEGL.map.txt', False),
    'libGLESv2': ('native/opengl/libs/libGLESv2.map.txt', False),
}


def api_value(v):
    if v in CODENAMES:
        return CODENAMES[v]
    if v == 'current' or v == 'future':
        return FUTURE
    try:
        return int(v)
    except ValueError:
        raise SystemExit('unknown API level: %r' % v)


class Version:
    def __init__(self, name, base, tags):
        self.name = name
        self.base = base
        self.tags = tags
        self.symbols = []   # (name, tags)


def parse_tags(comment):
    return comment.split() if comment else []


def parse_map(path):
    versions = []
    cur = None
    in_cpp = False
    with open(path) as f:
        for raw in f:
            line, _, comment = raw.partition('#')
            line = line.strip()
            tags = parse_tags(comment)
            if not line:
                continue
            # C++ name patterns: only in platform blocks, never in the NDK
            if cur is not None and line.startswith('extern "C++"'):
                if not (cur.name.endswith('_PLATFORM') or cur.name.endswith('_PRIVATE')):
                    raise SystemExit('%s: C++ symbols in %s' % (path, cur.name))
                in_cpp = True
                continue
            if in_cpp:
                if line == '};':
                    in_cpp = False
                continue
            if cur is None:
                m = re.match(r'^([A-Za-z0-9_.]+)\s*\{$', line)
                if not m:
                    raise SystemExit('%s: unexpected line: %r' % (path, raw))
                cur = Version(m.group(1), None, tags)
                continue
            m = re.match(r'^\}\s*([A-Za-z0-9_.]*)\s*;$', line)
            if m:
                cur.base = m.group(1) or None
                versions.append(cur)
                cur = None
                continue
            if line in ('global:', 'local:'):
                continue
            if line == '*;':
                continue
            m = re.match(r'^([A-Za-z0-9_.$]+);$', line)
            if not m:
                raise SystemExit('%s: bad symbol line: %r' % (path, raw))
            cur.symbols.append((m.group(1), tags))
    if cur is not None:
        raise SystemExit('%s: version block not closed' % path)
    return versions


def in_arch(tags):
    has_arch = False
    for t in tags:
        if t == ARCH:
            return True
        if t in ALL_ARCHES:
            has_arch = True
    return not has_arch


def in_api(tags, api):
    introduced = None
    arch_specific = False
    for t in tags:
        if t.startswith('introduced='):
            if not arch_specific:
                introduced = t.split('=', 1)[1]
        elif t.startswith('introduced-' + ARCH + '='):
            introduced = t.split('=', 1)[1]
            arch_specific = True
        elif t == 'future':
            return False
    if introduced is None:
        return True
    return api >= api_value(introduced)


def versioned_in_api(tags, api):
    for t in tags:
        if t.startswith('versioned='):
            return api >= api_value(t.split('=', 1)[1])
    return True


def omit_tags(tags, api):
    return not in_arch(tags) or not in_api(tags, api)


def omit_version(v, api):
    if v.name.endswith('_PRIVATE') or v.name.endswith('_PLATFORM'):
        return True
    if 'platform-only' in v.tags:
        return True
    if any(t in MODE_TAGS for t in v.tags):
        return True
    return omit_tags(v.tags, api)


def omit_symbol(tags, api):
    if 'platform-only' in tags:
        return True
    if any(t in MODE_TAGS for t in tags):
        return True
    return omit_tags(tags, api)


def make_stub(name, map_path, versioned, api, outdir, clang, keep):
    versions = parse_map(map_path)
    asm = ['\t.text']
    script = []
    seen = set()
    count = 0
    for v in versions:
        if omit_version(v, api):
            continue
        section_versioned = versioned_in_api(v.tags, api)
        empty = True
        pruned = []
        for sym, tags in v.symbols:
            if omit_symbol(tags, api):
                continue
            if versioned_in_api(tags, api):
                empty = False
            pruned.append((sym, tags))
        if not pruned:
            continue
        open_block = not empty and section_versioned
        if open_block:
            script.append('%s {\n  global:' % v.name)
        for sym, tags in pruned:
            if section_versioned and versioned_in_api(tags, api):
                script.append('    %s;' % sym)
            if sym in seen:
                continue
            seen.add(sym)
            count += 1
            bind = '.weak' if 'weak' in tags else '.globl'
            if 'var' in tags:
                asm += ['\t.data', '\t.balign 8', '\t%s %s' % (bind, sym),
                        '\t.type %s, %%object' % sym, '\t.size %s, 4' % sym,
                        '%s:' % sym, '\t.word 0', '\t.text']
            else:
                asm += ['\t%s %s' % (bind, sym), '\t.type %s, %%function' % sym,
                        '%s:' % sym, '\tret']
        if open_block:
            script.append('}%s;' % (' ' + v.base if v.base else ''))
    os.makedirs(keep, exist_ok=True)
    s_path = os.path.join(keep, name + '.S')
    v_path = os.path.join(keep, name + '.map')
    with open(s_path, 'w') as f:
        f.write('\n'.join(asm) + '\n')
    with open(v_path, 'w') as f:
        f.write('\n'.join(script) + '\n')
    out = os.path.join(outdir, name + '.so')
    cmd = [clang, '--target=%s%d' % (TRIPLE, api), '-shared', '-nostdlib',
           '-fuse-ld=lld', '-Wl,-soname,' + name + '.so', '-Wl,--hash-style=both',
           '-o', out, s_path]
    if versioned:
        cmd.insert(5, '-Wl,--version-script=' + v_path)
    subprocess.check_call(cmd)
    print('  %-12s %5d symbols%s' % (name + '.so', count, '' if versioned else ', unversioned'))


def copy_tree(src, dst, skip=()):
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        for fn in files:
            if not fn.endswith('.h'):
                continue
            if any(s in os.path.join(rel, fn) for s in skip):
                continue
            d = os.path.normpath(os.path.join(dst, rel))
            os.makedirs(d, exist_ok=True)
            shutil.copy2(os.path.join(root, fn), os.path.join(d, fn))


def make_headers(src, sysroot):
    inc = os.path.join(sysroot, 'usr', 'include')
    b = os.path.join(src, 'bionic', 'libc')
    # same layout as the NDK: see the ndk_headers modules in bionic
    copy_tree(os.path.join(b, 'include'), inc)
    up = os.path.join(b, 'kernel', 'uapi')
    for d in sorted(os.listdir(up)):
        p = os.path.join(up, d)
        if not os.path.isdir(p):
            continue
        if d.startswith('asm-') and d != 'asm-generic':
            if d == 'asm-arm64':
                copy_tree(p, os.path.join(inc, TRIPLE))
            continue
        copy_tree(p, os.path.join(inc, d))
    copy_tree(os.path.join(b, 'kernel', 'android', 'uapi'), inc)
    copy_tree(os.path.join(b, 'kernel', 'android', 'scsi'), os.path.join(inc, 'scsi'))
    n = os.path.join(src, 'native')
    copy_tree(os.path.join(n, 'include', 'android'), os.path.join(inc, 'android'))
    copy_tree(os.path.join(n, 'libs', 'arect', 'include', 'android'), os.path.join(inc, 'android'))
    copy_tree(os.path.join(n, 'libs', 'nativewindow', 'include', 'android'),
              os.path.join(inc, 'android'), skip=('_aidl.h',))
    for d in ('EGL', 'GLES', 'GLES2', 'GLES3', 'KHR'):
        copy_tree(os.path.join(n, 'opengl', 'include', d), os.path.join(inc, d))
    m = os.path.join(src, 'misc')
    shutil.copy2(os.path.join(m, 'log.h'), os.path.join(inc, 'android', 'log.h'))
    os.makedirs(os.path.join(inc, 'aaudio'), exist_ok=True)
    shutil.copy2(os.path.join(m, 'AAudio.h'), os.path.join(inc, 'aaudio', 'AAudio.h'))
    shutil.copy2(os.path.join(m, 'jni.h'), os.path.join(inc, 'jni.h'))


def make_crt(src, sysroot, libdir, api, clang, keep):
    c = os.path.join(src, 'bionic', 'libc', 'arch-common', 'bionic')
    priv = os.path.join(src, 'bionic', 'libc')
    base = [clang, '--target=%s%d' % (TRIPLE, api), '--sysroot=' + sysroot,
            '-c', '-O2', '-fPIC', '-I' + priv, '-DPLATFORM_SDK_VERSION=%d' % api]
    o1 = os.path.join(keep, 'crtbegin_so1.o')
    o2 = os.path.join(keep, 'crtbrand.o')
    subprocess.check_call(base + ['-o', o1, os.path.join(c, 'crtbegin_so.c')])
    subprocess.check_call(base + ['-o', o2, os.path.join(c, 'crtbrand.S')])
    subprocess.check_call([clang, '--target=%s%d' % (TRIPLE, api), '-r', '-nostdlib',
                           '-fuse-ld=lld', '-o', os.path.join(libdir, 'crtbegin_so.o'), o1, o2])
    subprocess.check_call(base + ['-o', os.path.join(libdir, 'crtend_so.o'),
                                  os.path.join(c, 'crtend_so.S')])
    print('  crtbegin_so.o crtend_so.o')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, help='sources from fetch_sources.sh')
    ap.add_argument('--out', required=True, help='sysroot to make')
    ap.add_argument('--api', type=int, default=24)
    ap.add_argument('--clang', default=os.environ.get('CLANG', 'clang'))
    a = ap.parse_args()
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    libdir = os.path.join(a.out, 'usr', 'lib', TRIPLE, str(a.api))
    keep = os.path.join(a.out, 'stubsrc')
    os.makedirs(libdir)
    print('headers')
    make_headers(a.src, a.out)
    print('stub libraries (API %d)' % a.api)
    for name, (rel, versioned) in LIBS.items():
        make_stub(name, os.path.join(a.src, rel), versioned, a.api, libdir, a.clang, keep)
    print('crt objects')
    make_crt(a.src, a.out, libdir, a.api, a.clang, keep)
    print('sysroot ready: %s' % a.out)


if __name__ == '__main__':
    sys.exit(main())
