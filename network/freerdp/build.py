#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build RemoteDesktop and its separate PDF/XPS renderer for an ADT SDK."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for option in ('source', 'renderer-source', 'build-dir', 'toolchain', 'sdk', 'install'):
    p.add_argument('--' + option, type=Path, required=True)
p.add_argument('--jobs', type=int, default=4)
a = p.parse_args()
for key in ("source", "renderer_source", "build_dir", "toolchain", "sdk", "install"):
    setattr(a, key, getattr(a, key).resolve())
if a.jobs < 1:
    p.error('--jobs must be positive')
root = Path(__file__).resolve().parent
a.build_dir.mkdir(parents=True, exist_ok=True)
flags = '-D_XOPEN_SOURCE=700 -D__BSD_VISIBLE=1 -D__STDC_NO_THREADS__=1 -mcmodel=large -mno-red-zone'
toolchain = a.build_dir / 'aros-toolchain.cmake'
toolchain.write_text('include(' + json.dumps(str(a.toolchain.resolve())) + ')\nlist(PREPEND CMAKE_MODULE_PATH ' + json.dumps(str(root / 'cmake')) + ')\n' + '''
if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  message(FATAL_ERROR "This port currently supports AROS x86_64 only")
endif()
set(CMAKE_C_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH "Target C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "Target C++ compiler" FORCE)
set(UNIX TRUE)
set(CMAKE_DL_LIBS "")
set(CMAKE_SYSTEM_PREFIX_PATH /)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_SHARED_LIBRARY_SUPPORTED FALSE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
''')
env = dict(os.environ, PKG_CONFIG_LIBDIR=str(a.sdk / 'lib/pkgconfig'),
           PKG_CONFIG_SYSROOT_DIR=str(a.sdk), PKG_CONFIG_PATH='')
build = a.build_dir / 'freerdp'
args = ['cmake', '-S', str(a.source), '-B', str(build),
        '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain.resolve()),
        '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=OFF',
        '-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags,
        '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF', '-DCMAKE_POSITION_INDEPENDENT_CODE=OFF',
        '-DOPENSSL_ROOT_DIR=' + str(a.sdk),
        '-DOPENSSL_INCLUDE_DIR=' + str(a.sdk / 'include'),
        '-DOPENSSL_CRYPTO_LIBRARY=' + str(a.sdk / 'lib/libcrypto.a'),
        '-DOPENSSL_SSL_LIBRARY=' + str(a.sdk / 'lib/libssl.a'),
        '-DZLIB_INCLUDE_DIR=' + str(a.sdk / 'include'),
        '-DZLIB_LIBRARY=' + str(a.sdk / 'lib/libz.a')]
for key in ('WITH_CLIENT_SDL WITH_X11 WITH_WAYLAND WITH_SERVER WITH_SHADOW WITH_SAMPLE '
            'WITH_FFMPEG WITH_DSP_FFMPEG WITH_VIDEO_FFMPEG WITH_SWSCALE WITH_OPUS '
            'WITH_CUPS WITH_PCSC WITH_KRB5 WITH_MANPAGES WITH_FUSE WITH_ALSA '
            'WITH_PULSE WITH_OSS WITH_FAAC WITH_FAAD2 WITH_OPENH264 WITH_WINPR_TOOLS '
            'WITH_WINPR_JSON WITH_AAD WITH_SSE2 BUILD_TESTING WITH_TIMEZONE_ICU '
            'WITH_SMARTCARD_PCSC WITH_PKCS11 USE_UNWIND CHANNEL_URBDRC CHANNEL_SMARTCARD '
            'CHANNEL_PARALLEL CHANNEL_SERIAL').split():
    args.append('-D' + key + '=OFF')
args += ['-DCHANNEL_PRINTER=ON', '-DCHANNEL_PRINTER_CLIENT=ON',
         '-DWITH_UNICODE_BUILTIN=ON', '-DWITH_JSON_DISABLED=ON']
subprocess.run(args, env=env, check=True)
subprocess.run(['cmake', '--build', str(build), '--target', 'adt-rdp',
                '--parallel', str(a.jobs)], env=env, check=True)
cache = {}
for line in (build / 'CMakeCache.txt').read_text().splitlines():
    if '=' in line and not line.startswith(('#', '//')):
        key, value = line.split('=', 1)
        cache[key.split(':', 1)[0]] = value
cc = cache['CMAKE_C_COMPILER']
renderer = a.build_dir / 'mupdf'
renderer.mkdir(exist_ok=True)
renderer_flags = '--sysroot=' + shlex.quote(str(a.sdk)) + ' ' + flags + ' -DFZ_ENABLE_HYPHEN=0'
args = ['make', '-j' + str(a.jobs), 'OS=AROS', 'build=release', 'OUT=' + str(renderer),
        'CC=' + cc, 'CXX=' + cache['CMAKE_CXX_COMPILER'],
        'AR=' + cache['CMAKE_AR'], 'RANLIB=' + cache['CMAKE_RANLIB'],
        'HAVE_OBJCOPY=no', 'HAVE_X11=no', 'HAVE_GLUT=no', 'HAVE_LIBCRYPTO=no',
        'HAVE_PTHREAD=no', 'HAVE_SYS_CURL=no', 'USE_SYSTEM_LIBS=no', 'threading=no',
        'mujs=no', 'html=no', 'extract=no', 'barcode=no', 'tesseract=no', 'tofu=yes',
        'XCFLAGS=' + renderer_flags, 'libs']
subprocess.run(args, cwd=a.renderer_source, env=env, check=True)
helper = renderer / 'adt-rdp-render'
subprocess.run([cc, '--sysroot=' + str(a.sdk), *shlex.split(flags), '-O2', '-Wall', '-Wextra',
                '-I' + str(a.renderer_source / 'include'), '-I' + str(root / 'renderer'),
                *[str(root / 'renderer' / name) for name in ('render.c', 'render-spool.c', 'render-native.c')],
                '-Wl,--start-group', str(renderer / 'libmupdf.a'), str(renderer / 'libmupdf-third.a'),
                '-Wl,--end-group', '-lm', '-lcybergraphics', '-lgraphics', '-lintuition',
                '-o', str(helper)], env=env, check=True)
a.install.mkdir(parents=True, exist_ok=True)
(a.install / 'Profiles').mkdir(exist_ok=True)
for source, name in [(build / 'client/AROS/adt-rdp', 'RemoteDesktop'),
                     (helper, 'adt-rdp-render'),
                     (root / 'icons/RemoteDesktop.info', 'RemoteDesktop.info'),
                     (root / 'README.md', 'README.md'),
                     (a.source / 'LICENSE', 'LICENSE-FreeRDP'),
                     (a.renderer_source / 'COPYING', 'LICENSE-renderer')]:
    shutil.copy2(source, a.install / name)
for source in (a.renderer_source / 'thirdparty').rglob('*'):
    if source.is_file() and source.name.upper().startswith(('COPYING', 'LICENSE', 'LICENCE', 'COPYRIGHT')):
        destination = a.install / 'licenses/MuPDF-thirdparty' / source.relative_to(a.renderer_source / 'thirdparty')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
print('Installed RemoteDesktop and adt-rdp-render in', a.install)
