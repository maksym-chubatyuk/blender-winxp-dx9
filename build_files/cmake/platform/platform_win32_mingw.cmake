# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Libraries configuration for Windows built with MinGW-w64 GCC.
#
# Only used by the 32-bit Windows XP cross-build (see `build_files/xp/`): libraries are static
# and located with `find_package()` under `LIBDIR` (the `deps.sh` install prefix), like on Unix.

add_definitions(-DWIN32)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "MinGW builds require GCC")
endif()

if(NOT DEFINED LIBDIR)
  if(DEFINED ENV{XP_DEPS_PREFIX})
    set(LIBDIR $ENV{XP_DEPS_PREFIX})
  else()
    set(LIBDIR ${CMAKE_SOURCE_DIR}/build_xp/deps/install)
  endif()
endif()
if(NOT EXISTS "${LIBDIR}")
  message(FATAL_ERROR "LIBDIR '${LIBDIR}' not found, build it with build_files/xp/deps.sh")
endif()
message(STATUS "Libraries in: ${LIBDIR}")
list(PREPEND CMAKE_PREFIX_PATH ${LIBDIR})
list(PREPEND CMAKE_FIND_ROOT_PATH ${LIBDIR})
set(Eigen3_ROOT ${LIBDIR})

# Only static libraries are built for this target.
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

# NOTE: must be a macro, forwards `${ARGV}` to `find_package()`
# whose result variables must be visible in the caller's scope.
macro(find_package_wrapper)
  find_package(${ARGV})
endmacro()

# NOTE: must be a macro, forwards to `find_package()`
# whose result variables must be visible in the caller's scope.
macro(windows_find_package package_name)
  find_package(${package_name})
endmacro()

# ----------------------------------------------------------------------------
# Required Libraries

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Zstd REQUIRED)
find_package(fmt REQUIRED)
find_package(Freetype REQUIRED)
# FreeType's Brotli dependency (WOFF2 fonts), linked after it.
set(BROTLI_ROOT_DIR ${LIBDIR})
find_package(Brotli REQUIRED)
find_package(OpenEXR REQUIRED)
# The static TIFF config (pulled in by OpenImageIO) links `CMath::CMath`, which only its own
# build defines. The math functions are in the C runtime here.
if(NOT TARGET CMath::CMath)
  add_library(CMath::CMath INTERFACE IMPORTED)
endif()
find_package(OpenImageIO REQUIRED)
find_package(OpenColorIO 2.0.0 REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(Threads REQUIRED)

if(WITH_OPENGL_BACKEND)
  find_package(Epoxy REQUIRED)
endif()

if(WITH_PYTHON)
  # Set by `build_files/xp/python.sh`: a static CPython built from the MSYS2 MinGW fork.
  if(NOT DEFINED PYTHON_VERSION)
    set(PYTHON_VERSION 3.13)
  endif()
  string(REPLACE "." "" _PYTHON_VERSION_NO_DOTS ${PYTHON_VERSION})
  set(PYTHON_ROOT ${LIBDIR}/python)
  set(PYTHON_INCLUDE_DIR ${PYTHON_ROOT}/include/python${PYTHON_VERSION})
  set(PYTHON_INCLUDE_DIRS ${PYTHON_INCLUDE_DIR})
  set(PYTHON_LIBRARY ${PYTHON_ROOT}/lib/libpython${PYTHON_VERSION}.dll.a)
  set(PYTHON_LIBRARIES ${PYTHON_LIBRARY})
  set(PYTHON_LIBPATH ${PYTHON_ROOT}/lib)
  find_program(PYTHON_EXECUTABLE "python${PYTHON_VERSION}" "python3")
  unset(_PYTHON_VERSION_NO_DOTS)
else()
  # Python executable is needed as part of the build-process.
  find_program(PYTHON_EXECUTABLE "python3")
endif()

if(WITH_OPENSUBDIV)
  # CPU evaluation only, there is no `osdGPU` library without a GPU API.
  find_path(OPENSUBDIV_INCLUDE_DIR opensubdiv/osd/mesh.h HINTS ${LIBDIR}/include)
  find_library(OPENSUBDIV_OSDCPU_LIBRARY osdCPU HINTS ${LIBDIR}/lib)
  if(OPENSUBDIV_INCLUDE_DIR AND OPENSUBDIV_OSDCPU_LIBRARY)
    set(OPENSUBDIV_FOUND TRUE)
    set(OPENSUBDIV_INCLUDE_DIRS ${OPENSUBDIV_INCLUDE_DIR})
    set(OPENSUBDIV_LIBRARIES ${OPENSUBDIV_OSDCPU_LIBRARY})
  endif()
  set_and_warn_library_found("OpenSubdiv" OPENSUBDIV_FOUND WITH_OPENSUBDIV)
endif()

if(WITH_TBB)
  find_package(TBB)
  set_and_warn_library_found("TBB" TBB_FOUND WITH_TBB)
  if(TBB_FOUND)
    get_target_property(TBB_LIBRARIES TBB::tbb LOCATION)
    get_target_property(TBB_INCLUDE_DIRS TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
  endif()
endif()
set(WITH_TBB_MALLOC_PROXY OFF)

# ----------------------------------------------------------------------------
# Build and Link Flags

# XP thunks first and whole: their import slots must win over the system import libraries.
list(APPEND PLATFORM_LINKLIBS
  -Wl,--whole-archive bf_intern_xpcompat -Wl,--no-whole-archive
)
list(APPEND PLATFORM_LINKLIBS
  ws2_32 vfw32 winmm kernel32 user32 gdi32 comdlg32 comctl32 version
  advapi32 shfolder shell32 ole32 oleaut32 uuid psapi dbghelp shlwapi
  dwmapi crypt32 bcrypt mpr
)
if(WITH_INPUT_IME)
  list(APPEND PLATFORM_LINKLIBS imm32)
endif()
# GNU ld resolves archives in command line order, but static Blender libraries listed after
# `PLATFORM_LINKLIBS` also call into these: repeat them at the end of every link line.
set(_win_system_libs
  -lws2_32 -lvfw32 -lwinmm -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lversion -ladvapi32
  -lshfolder -lshell32 -lole32 -loleaut32 -luuid -lpsapi -ldbghelp -lshlwapi -ldwmapi
  -lcrypt32 -lbcrypt -lmpr -limm32 -lkernel32
)
string(JOIN " " CMAKE_C_STANDARD_LIBRARIES ${CMAKE_C_STANDARD_LIBRARIES} ${_win_system_libs})
string(JOIN " " CMAKE_CXX_STANDARD_LIBRARIES ${CMAKE_CXX_STANDARD_LIBRARIES} ${_win_system_libs})
unset(_win_system_libs)
if(CMAKE_THREAD_LIBS_INIT)
  list(APPEND PLATFORM_LINKLIBS ${CMAKE_THREAD_LIBS_INIT})
  set(PTHREADS_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
endif()

# Windows headers spelled with capitals (`<Windows.h>`), forwarded to MinGW's lower-case ones.
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/build_files/xp/include_case)

add_definitions(
  -D_USE_MATH_DEFINES
  -DWIN32_LEAN_AND_MEAN
  -DNOMINMAX
  # Declarations up to Windows 8.1, like the MSVC build. Imports XP lacks are caught by
  # `build_files/xp/xpcheck.c` and provided by the XP compatibility thunks.
  -D_WIN32_WINNT=0x603
)

# `-funsigned-char` matches MSVC's `/J`.
# `-Wa,-mbig-obj`: some translation units exceed the 32k COFF section limit.
# `-mstackrealign`: Win32 only guarantees 4-byte stack alignment at callbacks and thread entry,
# SSE spills need 16.
set(PLATFORM_CFLAGS
  "-funsigned-char -fno-strict-aliasing -ffp-contract=off -Wa,-mbig-obj -mstackrealign"
)
string(APPEND CMAKE_C_FLAGS " ${PLATFORM_CFLAGS}")
string(APPEND CMAKE_CXX_FLAGS " ${PLATFORM_CFLAGS}")

string(APPEND PLATFORM_LINKFLAGS " -mconsole -Wl,--stack,2097152")

# The creator lists the manifest as a source. GNU ld ignores it, it's embedded via resources.
set(BUNDLECRT "")
configure_file(
  ${CMAKE_SOURCE_DIR}/release/windows/manifest/blender.exe.manifest.in
  ${CMAKE_CURRENT_BINARY_DIR}/blender.exe.manifest
  @ONLY
)
configure_file(
  ${CMAKE_SOURCE_DIR}/release/windows/manifest/blender.exe.manifest.in
  ${CMAKE_CURRENT_BINARY_DIR}/tests.exe.manifest
  @ONLY
)

# Wintab tablet API headers ship with the sources.
set(WINTAB_INC ${CMAKE_SOURCE_DIR}/extern/wintab/include)
