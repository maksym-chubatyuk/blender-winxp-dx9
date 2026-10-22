#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Build Blender's libraries for the Windows XP target (32-bit, static, MinGW-w64 + UCRT).
# Versions and download hashes come from build_files/build_environment/cmake/versions.cmake.
#
# Usage: build_files/xp/deps.sh            build everything not built yet
#        build_files/xp/deps.sh tiff oiio  (re)build only these
#
# Output: $XP_BUILD_DIR/deps/install (default build_xp/ in the source root).

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${XP_BUILD_DIR:-$ROOT/build_xp}
DL=$OUT/deps/download
SRC=$OUT/deps/src
BLD=$OUT/deps/build
PREFIX=$OUT/deps/install
TOOLCHAIN=$HERE/toolchain-i686-xp.cmake
JOBS=${JOBS:-$(nproc)}
export XP_DEPS_PREFIX=$PREFIX

mkdir -p "$DL" "$SRC" "$BLD" "$PREFIX"

log() { printf '\033[1;36m[deps]\033[0m %s\n' "$*"; }

# fetch NAME -> extracts the pinned source once, prints its directory.
fetch() {
  local name=$1 info uri htype hash file dir
  info=$(cmake -DNAME="$name" -P "$HERE/dep_info.cmake")
  read -r uri htype hash file <<<"$info"
  [ -n "$file" ] || file=$(basename "$uri")
  if [ ! -f "$DL/$file" ]; then
    log "download $file" >&2
    curl -fL --retry 3 -o "$DL/$file.part" "$uri" >&2
    mv "$DL/$file.part" "$DL/$file"
  fi
  if [ -n "$hash" ]; then
    local sum
    case $htype in
      MD5) sum=$(md5sum "$DL/$file" | cut -d' ' -f1) ;;
      SHA256) sum=$(sha256sum "$DL/$file" | cut -d' ' -f1) ;;
      *) sum=$hash ;;
    esac
    [ "$sum" = "$hash" ] || { echo "hash mismatch for $file" >&2; exit 1; }
  fi
  dir=$SRC/$name
  if [ ! -d "$dir" ]; then
    mkdir -p "$dir.tmp"
    tar -xf "$DL/$file" -C "$dir.tmp" --strip-components=1
    # MinGW / XP fixes: patches/<name>_*.diff, applied once on extraction.
    local p
    for p in "$HERE"/patches/"${name,,}"_*.diff; do
      [ -f "$p" ] || continue
      log "patch $(basename "$p")" >&2
      patch -d "$dir.tmp" -p1 -s <"$p"
    done
    mv "$dir.tmp" "$dir"
  fi
  echo "$dir"
}

# cmake_dep NAME SRCDIR [cmake args...]: configure, build and install into $PREFIX.
cmake_dep() {
  local name=$1 src=$2
  shift 2
  rm -rf "${BLD:?}/$name"
  cmake -S "$src" -B "$BLD/$name" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    "$@"
  cmake --build "$BLD/$name" -j "$JOBS"
  cmake --install "$BLD/$name"
}

dep_zlib() {
  local s
  s=$(fetch ZLIB)
  cmake_dep zlib "$s" -DZLIB_BUILD_EXAMPLES=OFF
  # Static only, under the name every FindZLIB looks for.
  rm -f "$PREFIX/bin/libzlib.dll" "$PREFIX/lib/libzlib.dll.a"
  cp "$PREFIX/lib/libzlibstatic.a" "$PREFIX/lib/libz.a"
}

dep_png() {
  local s
  s=$(fetch PNG)
  cmake_dep png "$s" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
    -DPNG_FRAMEWORK=OFF
}

dep_jpeg() {
  local s
  s=$(fetch JPEG)
  # No SIMD: needs NASM. TODO(xp): the SSE2 paths are worth it on the P4.
  cmake_dep jpeg "$s" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
}

dep_zstd() {
  local s
  s=$(fetch ZSTD)
  cmake_dep zstd "$s/build/cmake" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
    -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
}

dep_brotli() {
  local s
  s=$(fetch BROTLI)
  cmake_dep brotli "$s" -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUNDLED_MODE=OFF
  # Static only: the shared libraries are always built, drop them so they're never picked.
  rm -f "$PREFIX"/bin/libbrotli*.dll "$PREFIX"/lib/libbrotli*.dll.a
  for l in common dec enc; do
    [ -f "$PREFIX/lib/libbrotli$l-static.a" ] && cp "$PREFIX/lib/libbrotli$l-static.a" "$PREFIX/lib/libbrotli$l.a"
  done
}

dep_freetype() {
  local s
  s=$(fetch FREETYPE)
  # Brotli: Blender's UI fonts are WOFF2.
  cmake_dep freetype "$s" -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_REQUIRE_BROTLI=ON
}

dep_fmt() {
  local s
  s=$(fetch FMT)
  cmake_dep fmt "$s" -DFMT_DOC=OFF -DFMT_TEST=OFF
}

dep_imath() {
  local s
  s=$(fetch IMATH)
  cmake_dep imath "$s" -DPYTHON=OFF -DIMATH_INSTALL_PKG_CONFIG=ON
}

dep_deflate() {
  local s
  s=$(fetch DEFLATE)
  cmake_dep deflate "$s" -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF
}

dep_openjph() {
  local s
  s=$(fetch OPENJPH)
  cmake_dep openjph "$s" -DOJPH_BUILD_EXECUTABLES=OFF -DOJPH_ENABLE_TIFF_SUPPORT=OFF \
    -DOJPH_BUILD_TESTS=OFF
}

dep_openexr() {
  local s
  s=$(fetch OPENEXR)
  cmake_dep openexr "$s" -DOPENEXR_BUILD_TOOLS=OFF -DOPENEXR_INSTALL_TOOLS=OFF \
    -DOPENEXR_BUILD_EXAMPLES=OFF -DOPENEXR_INSTALL_PKG_CONFIG=ON -DOPENEXR_BUILD_PYTHON=OFF
}

dep_tiff() {
  local s
  s=$(fetch TIFF)
  cmake_dep tiff "$s" -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF -Dtiff-docs=OFF \
    -Djbig=OFF -Dlerc=OFF -Dlzma=OFF -Dzstd=OFF -Dwebp=OFF -Dlibdeflate=OFF -Dsphinx=OFF \
    -Dcxx=OFF
}

dep_robinmap() {
  local s
  s=$(fetch ROBINMAP)
  cmake_dep robinmap "$s"
}

dep_eigen() {
  local s
  s=$(fetch EIGEN)
  cmake_dep eigen "$s" -DEIGEN_BUILD_TESTING=OFF -DEIGEN_BUILD_DOC=OFF -DEIGEN_BUILD_PKGCONFIG=OFF \
    -DEIGEN_BUILD_BLAS=OFF -DEIGEN_BUILD_LAPACK=OFF -DEIGEN_BUILD_DEMOS=OFF
}

dep_oiio() {
  local s
  s=$(fetch OPENIMAGEIO)
  cmake_dep oiio "$s" -DLINKSTATIC=ON -DOIIO_BUILD_TOOLS=OFF -DOIIO_BUILD_TESTS=OFF \
    -DOpenImageIO_BUILD_MISSING_DEPS=none -DUSE_PYTHON=OFF -DUSE_TBB=OFF -DUSE_QT=OFF \
    -DUSE_OPENCOLORIO=ON -DUSE_LIBUHDR=OFF -DUSE_OPENVDB=OFF -DUSE_FREETYPE=OFF -DUSE_DCMTK=OFF -DUSE_GIF=OFF \
    -DUSE_OPENCV=OFF -DUSE_OPENJPEG=OFF -DUSE_FFMPEG=OFF -DUSE_PTEX=OFF -DUSE_LIBHEIF=OFF \
    -DUSE_LIBRAW=OFF -DUSE_JXL=OFF -DUSE_WEBP=OFF -DUSE_NUKE=OFF -DUSE_EXTERNAL_PUGIXML=OFF \
    -DUSE_SIMD=sse2 -DSTOP_ON_WARNING=OFF -DINSTALL_DOCS=OFF -DINSTALL_FONTS=OFF \
    -DOIIO_INSTALL_FONTS=OFF -DEMBEDPLUGINS=ON
}

dep_expat() {
  local s
  s=$(fetch EXPAT)
  cmake_dep expat "$s/expat" -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF \
    -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF
}

dep_yamlcpp() {
  local s
  s=$(fetch YAMLCPP)
  cmake_dep yamlcpp "$s" -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
    -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_BUILD_SHARED_LIBS=OFF \
    -DCMAKE_CXX_FLAGS="-include cstdint" # 0.8.0 relies on GCC < 13 including it transitively.
}

dep_pystring() {
  local s
  s=$(fetch PYSTRING)
  cp "$ROOT/build_files/build_environment/patches/cmakelists_pystring.txt" "$s/CMakeLists.txt"
  cmake_dep pystring "$s"
}

dep_minizipng() {
  local s
  s=$(fetch MINIZIPNG)
  cmake_dep minizipng "$s" -DMZ_ZLIB=ON -DMZ_BZIP2=OFF -DMZ_LZMA=OFF -DMZ_ZSTD=OFF \
    -DMZ_OPENSSL=OFF -DMZ_PKCRYPT=OFF -DMZ_WZAES=OFF -DMZ_SIGNING=OFF -DMZ_FETCH_LIBS=OFF \
    -DMZ_ICONV=OFF -DMZ_LIBCOMP=OFF -DMZ_LIBBSC=OFF -DMZ_COMPAT=OFF \
    -DCMAKE_INSTALL_INCLUDEDIR="$PREFIX/include/minizip-ng"
}

dep_opensubdiv() {
  local s
  s=$(fetch OPENSUBDIV)
  # CPU evaluation only: Blender's GPU subdivision goes through its own GPU module.
  cmake_dep opensubdiv "$s" -DNO_EXAMPLES=ON -DNO_TUTORIALS=ON -DNO_REGRESSION=ON -DNO_PTEX=ON \
    -DNO_DOC=ON -DNO_OMP=ON -DNO_TBB=ON -DNO_CUDA=ON -DNO_OPENCL=ON -DNO_CLEW=ON -DNO_OPENGL=ON \
    -DNO_METAL=ON -DNO_DX=ON -DNO_TESTS=ON -DNO_GLTESTS=ON -DNO_GLEW=ON -DNO_GLFW=ON \
    -DNO_GLFW_X11=ON -DNO_SHARED_LIBS=ON -DNO_MACOS_FRAMEWORK=ON
}

dep_ocio() {
  local s
  s=$(fetch OPENCOLORIO)
  cmake_dep ocio "$s" -DOCIO_BUILD_APPS=OFF -DOCIO_BUILD_PYTHON=OFF -DOCIO_BUILD_NUKE=OFF \
    -DOCIO_BUILD_JAVA=OFF -DOCIO_BUILD_DOCS=OFF -DOCIO_BUILD_TESTS=OFF -DOCIO_BUILD_GPU_TESTS=OFF \
    -DOCIO_BUILD_OPENFX=OFF -DOCIO_USE_SIMD=ON -DOCIO_USE_AVX=OFF -DOCIO_USE_AVX2=OFF \
    -DOCIO_USE_AVX512=OFF -DOCIO_USE_F16C=OFF -DOCIO_USE_SSE3=OFF -DOCIO_USE_SSSE3=OFF \
    -DOCIO_USE_SSE4=OFF -DOCIO_USE_SSE42=OFF -DOCIO_INSTALL_EXT_PACKAGES=NONE \
    -Dminizip-ng_INCLUDE_DIR="$PREFIX/include/minizip-ng/minizip"
}

ALL_DEPS=(zlib png jpeg zstd brotli freetype fmt imath deflate openjph openexr tiff robinmap eigen expat
  yamlcpp pystring minizipng ocio oiio opensubdiv)

if [ $# -gt 0 ]; then
  for d in "$@"; do
    log "build $d"
    "dep_$d"
    touch "$BLD/$d.done"
  done
else
  for d in "${ALL_DEPS[@]}"; do
    [ -f "$BLD/$d.done" ] && continue
    log "build $d"
    "dep_$d"
    touch "$BLD/$d.done"
  done
fi
log "done: $PREFIX"
