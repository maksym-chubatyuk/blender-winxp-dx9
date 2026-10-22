# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Print "<uri> <hash-type> <hash> <file>" for dependency NAME, from the versions Blender pins.
# Usage: cmake -DNAME=ZLIB -P dep_info.cmake

set(BUILD_MODE Release)
include(${CMAKE_CURRENT_LIST_DIR}/../build_environment/cmake/versions.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo
  "${${NAME}_URI} ${${NAME}_HASH_TYPE} ${${NAME}_HASH} ${${NAME}_FILE}")
