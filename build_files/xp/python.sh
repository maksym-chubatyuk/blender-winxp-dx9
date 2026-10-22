#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Cross-compile CPython 3.13 for the Windows XP target from the MSYS2 MinGW fork
# (github.com/msys2-contrib/cpython-mingw), which carries the patches upstream lacks for MinGW.
# A native build of the same source is made first: cross builds need a same-version Python.
#
# Output: $XP_BUILD_DIR/deps/install/python/{bin,lib,include} (default build_xp/ in the root).

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${XP_BUILD_DIR:-$ROOT/build_xp}
SRC=$OUT/deps/src/cpython
BLD=$OUT/deps/build
PREFIX=$OUT/deps/install
HOSTPY=$OUT/deps/hostpython
JOBS=${JOBS:-$(nproc)}

PY_BRANCH=mingw-v3.13.11
AUTOCONF_ARCHIVE=autoconf-archive-2024.10.16

log() { printf '\033[1;36m[python]\033[0m %s\n' "$*"; }

if [ ! -d "$SRC" ]; then
  log "clone $PY_BRANCH"
  git clone -q --depth 1 -b "$PY_BRANCH" https://github.com/msys2-contrib/cpython-mingw.git "$SRC"
  for p in "$HERE"/patches/cpython_*.diff; do
    log "patch $(basename "$p")"
    git -C "$SRC" apply "$p"
  done
fi

# The fork changes configure.ac without regenerating configure; its AX_ macros come from
# autoconf-archive, fetched locally instead of requiring the system package.
TOOLS=$OUT/deps/tools
if [ ! -d "$TOOLS/$AUTOCONF_ARCHIVE" ]; then
  mkdir -p "$TOOLS"
  curl -fsSL "https://ftp.gnu.org/gnu/autoconf-archive/$AUTOCONF_ARCHIVE.tar.xz" | tar -xJ -C "$TOOLS"
fi
if [ ! -f "$SRC/.xp-autoreconf" ]; then
  log "autoreconf"
  (cd "$SRC" && ACLOCAL_PATH="$TOOLS/$AUTOCONF_ARCHIVE/m4" autoreconf -fi)
  touch "$SRC/.xp-autoreconf"
fi

# The fork does not build natively on Linux: the build-python comes from upstream CPython,
# the version Blender pins (only the 3.13 minor version has to match).
if [ ! -x "$HOSTPY/bin/python3.13" ]; then
  log "native build-python"
  read -r uri _ _ file <<<"$(cmake -DNAME=PYTHON -P "$HERE/dep_info.cmake")"
  mkdir -p "$OUT/deps/download"
  [ -f "$OUT/deps/download/$file" ] || curl -fsSL -o "$OUT/deps/download/$file" "$uri"
  rm -rf "$BLD/hostpython" "$OUT/deps/src/hostpython"
  mkdir -p "$BLD/hostpython" "$HOSTPY" "$OUT/deps/src/hostpython"
  tar -xf "$OUT/deps/download/$file" -C "$OUT/deps/src/hostpython" --strip-components=1
  (cd "$BLD/hostpython" &&
    "$OUT/deps/src/hostpython/configure" --prefix="$HOSTPY" --without-ensurepip >/dev/null &&
    make -j "$JOBS" >/dev/null &&
    make install >/dev/null)
fi

log "cross build"
ARCH_FLAGS="-march=pentium4 -mtune=pentium4 -mfpmath=sse -msse2 -mstackrealign"
XP_LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--major-subsystem-version,5,--minor-subsystem-version,1"
XP_LDFLAGS+=" -Wl,--major-os-version,5,--minor-os-version,1 -Wl,--large-address-aware"

rm -rf "$BLD/python"
mkdir -p "$BLD/python"
cd "$BLD/python"
export PKG_CONFIG_LIBDIR=/dev/null
unset PKG_CONFIG_PATH
"$SRC/configure" \
  --host=i686-w64-mingw32 \
  --build=x86_64-pc-linux-gnu \
  --prefix=/python \
  --enable-shared \
  --without-ensurepip \
  --without-c-locale-coercion \
  --with-build-python="$HOSTPY/bin/python3.13" \
  CFLAGS="-O2 $ARCH_FLAGS" \
  CPPFLAGS="-I$PREFIX/include" \
  LDFLAGS="-L$PREFIX/lib $XP_LDFLAGS" \
  ZLIB_CFLAGS="-I$PREFIX/include" \
  ZLIB_LIBS="-L$PREFIX/lib -lz"
make -j "$JOBS"
make install DESTDIR="$PREFIX"
log "site-packages"
python3 -I "$HERE/site_packages.py" "$PREFIX/python/lib/python3.13/site-packages"
log "done: $PREFIX/python"
