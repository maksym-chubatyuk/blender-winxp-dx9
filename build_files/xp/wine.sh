#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Run a Windows program from the XP build under Wine, in a prefix private to this build.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export WINEPREFIX=${XP_WINEPREFIX:-$HERE/../../build_xp/wineprefix}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=}
exec wine "$@"
