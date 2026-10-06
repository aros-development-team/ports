# Minimal SDL2 package for the AROS cross-build.
#
# OpenTTD calls find_package(SDL2) in config mode and links SDL2::SDL2, but the
# AROS SDL2 install ships no SDL2Config.cmake. Without this shim CMake resolves
# the *host* (homebrew) SDL2 instead - the AROS toolchain file does not set
# CMAKE_FIND_ROOT_PATH_MODE_PACKAGE, so config-mode lookups escape the sysroot.
# That silently yields a macOS library, which surfaces as Cocoa FRAMEWORK
# generator expressions in the link line.
#
# Point find_package at this directory with -DSDL2_DIR, and pass the AROS
# header/library locations in AROS_SDL2_INCLUDE_DIR / AROS_SDL2_LIBRARY.

if(NOT AROS_SDL2_INCLUDE_DIR OR NOT AROS_SDL2_LIBRARY)
    message(FATAL_ERROR
        "AROS_SDL2_INCLUDE_DIR and AROS_SDL2_LIBRARY must both be set")
endif()

if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 UNKNOWN IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${AROS_SDL2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${AROS_SDL2_INCLUDE_DIR}")
endif()

set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "${AROS_SDL2_INCLUDE_DIR}")
set(SDL2_LIBRARIES SDL2::SDL2)
