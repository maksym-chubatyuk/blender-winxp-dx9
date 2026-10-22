#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Configure the Windows XP cross-build of Blender (run deps.sh first).
#
# Usage: build_files/xp/configure.sh [extra cmake args...]
# Then:  cmake --build build_xp/blender && cmake --install build_xp/blender

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${XP_BUILD_DIR:-$ROOT/build_xp}
export XP_DEPS_PREFIX=$OUT/deps/install

cmake -S "$ROOT" -B "$OUT/blender" -G Ninja -C "$HERE/config_xp.cmake" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-i686-xp.cmake" \
  -DCMAKE_INSTALL_PREFIX="$OUT/install" \
  -DLIBDIR="$XP_DEPS_PREFIX" \
  "$@"
