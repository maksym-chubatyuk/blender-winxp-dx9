# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Windows XP thunks, linked into every executable of the 32-bit MinGW build as a whole archive
# (`PLATFORM_LINKLIBS`, see `platform_win32_mingw.cmake`).

set(INC
)

set(INC_SYS
)

set(SRC
  xpcompat.c
)

set(LIB
)

blender_add_lib(bf_intern_xpcompat "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
