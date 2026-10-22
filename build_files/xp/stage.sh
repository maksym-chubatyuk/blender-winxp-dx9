#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Install the XP build and prepare a stripped copy to push to the XP machine:
# $XP_BUILD_DIR/stage/blender (+ xpcheck.exe next to it).

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${XP_BUILD_DIR:-$ROOT/build_xp}
STAGE=$OUT/stage

cmake --install "$OUT/blender" >/dev/null
mkdir -p "$STAGE"
rsync -a --delete --exclude='__pycache__' "$OUT/install/" "$STAGE/blender/"
find "$STAGE/blender" \( -name '*.exe' -o -name '*.dll' -o -name '*.pyd' \) -print0 |
  xargs -0 i686-w64-mingw32-strip --strip-unneeded

XPCHECK=$STAGE/xpcheck.exe
if [ ! -f "$XPCHECK" ] || [ "$HERE/xpcheck.c" -nt "$XPCHECK" ]; then
  i686-w64-mingw32-gcc -O2 -std=c99 -Wall -mcrtdll=msvcrt-os "$HERE/xpcheck.c" -o "$XPCHECK" \
    -static -Wl,--major-subsystem-version,5,--minor-subsystem-version,1 \
    -Wl,--major-os-version,5,--minor-os-version,1
fi
du -sh "$STAGE/blender"
