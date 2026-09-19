# RetroBug — RetroBug contributors. SPDX-License-Identifier: Apache-2.0
#
# Stub find module that satisfies `find_package(Boost ...)` without a Boost installation.
#
# dynarmic calls find_package(Boost 1.57 REQUIRED) at configure time and links Boost::boost, but
# it uses only eight Boost names, all header-only, and those are implemented in ../compat
# (see compat/README.md for why this is a shim rather than a patch — short version: AD-14 keeps
# the vendored tree byte-identical). This module exists so the *configure* step agrees with that.
#
# This shadows CMake's bundled FindBoost module only because it appears first in
# CMAKE_MODULE_PATH, and only when RB_USE_REAL_BOOST is OFF. Set that option ON and a real Boost
# installation is found normally, with compat/ left out of the include path.

if(RB_USE_REAL_BOOST)
  # Not our business: let CMake's own module run.
  return()
endif()

set(Boost_FOUND TRUE)
set(Boost_VERSION "1.83.0")
set(Boost_VERSION_STRING "1.83.0")
set(Boost_MAJOR_VERSION 1)
set(Boost_MINOR_VERSION 83)
set(Boost_SUBMINOR_VERSION 0)
set(Boost_FIND_VERSION_EXACT TRUE)

if(NOT TARGET Boost::boost)
  add_library(Boost::boost INTERFACE IMPORTED)
  set_target_properties(Boost::boost PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../compat")
endif()

# Every component dynarmic or a consumer might name resolves to the same header-only set.
set(Boost_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../compat")
set(Boost_LIBRARIES "")

message(STATUS "RetroBug: satisfying find_package(Boost) with compat/ shims "
               "(configure with -DRB_USE_REAL_BOOST=ON to use real Boost)")
