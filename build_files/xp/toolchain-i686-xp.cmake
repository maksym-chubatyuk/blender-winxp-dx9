# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Cross-compile from Linux to 32-bit Windows XP (Pentium 4 / SSE2, no SSE3) with MinGW-w64 GCC.
#
# The C/C++ runtime is the UCRT (the XP target has the VC++ 2015+ redistributable installed):
# libstdc++ does not build against XP's own msvcrt.dll. Everything is linked statically and every
# image is stamped subsystem/OS version 5.1, otherwise XP refuses to load it.
# Windows headers are left at their default (newer) version: imports XP lacks are found by
# build_files/xp/xpcheck.c on the real machine and resolved by the xpcompat thunks.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 5.1)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(XP_TRIPLE i686-w64-mingw32)
set(CMAKE_C_COMPILER ${XP_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${XP_TRIPLE}-g++)
set(CMAKE_RC_COMPILER ${XP_TRIPLE}-windres)
set(CMAKE_AR ${XP_TRIPLE}-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${XP_TRIPLE}-ranlib CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH /usr/${XP_TRIPLE})
if(DEFINED ENV{XP_DEPS_PREFIX})
  list(APPEND CMAKE_FIND_ROOT_PATH $ENV{XP_DEPS_PREFIX})
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Pentium 4 (Northwood): SSE2 is the ceiling, never SSE3.
set(XP_ARCH_FLAGS "-march=pentium4 -mtune=pentium4 -mfpmath=sse -msse2")
set(CMAKE_C_FLAGS_INIT "${XP_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${XP_ARCH_FLAGS}")

set(XP_LINK_FLAGS
  "-static -static-libgcc -static-libstdc++ \
-Wl,--major-subsystem-version,5,--minor-subsystem-version,1 \
-Wl,--major-os-version,5,--minor-os-version,1 \
-Wl,--large-address-aware"
)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${XP_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${XP_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${XP_LINK_FLAGS}")

# Run Windows test executables (try_run etc.) through Wine.
set(CMAKE_CROSSCOMPILING_EMULATOR ${CMAKE_CURRENT_LIST_DIR}/wine.sh)
