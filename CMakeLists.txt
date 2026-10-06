cmake_minimum_required(VERSION 3.16)
project(studyquest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# ------------------------------------------------------------------
# Locate raylib:
#   1. pkg-config (works on NixOS once PKG_CONFIG_PATH is set,
#      and everywhere else by default)
#   2. CMake config package
#   3. Manual find_path / find_library using $ENV{raylib} as a hint
#   4. FetchContent (opt-in with -DUSE_FETCH_RAYLIB=ON)
# ------------------------------------------------------------------
set(SQ_RAYLIB_FOUND FALSE)
set(SQ_RAYLIB_VIA   "unknown")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(RAYLIB_PC QUIET raylib)
  if(RAYLIB_PC_FOUND)
    set(SQ_RAYLIB_FOUND TRUE)
    set(SQ_RAYLIB_VIA   "pkg-config (${RAYLIB_PC_VERSION})")
  endif()
endif()

if(NOT SQ_RAYLIB_FOUND)
  find_package(raylib QUIET)
  if(raylib_FOUND)
    set(SQ_RAYLIB_FOUND TRUE)
    set(SQ_RAYLIB_VIA   "CMake config")
  endif()
endif()

# Manual fallback — useful if pkg-config is unavailable or misconfigured.
# In a NixOS shell you can pass -DRAYLIB_ROOT=$(nix eval --raw nixpkgs#raylib)
if(NOT SQ_RAYLIB_FOUND)
  find_path(RAYLIB_INCLUDE_DIR raylib.h
    HINTS ${RAYLIB_ROOT} $ENV{RAYLIB_ROOT} /usr/local /usr)
  find_library(RAYLIB_LIBRARY raylib
    HINTS ${RAYLIB_ROOT} $ENV{RAYLIB_ROOT} /usr/local /usr)
  if(RAYLIB_INCLUDE_DIR AND RAYLIB_LIBRARY)
    set(SQ_RAYLIB_FOUND TRUE)
    set(SQ_RAYLIB_VIA   "manual find (${RAYLIB_LIBRARY})")
  endif()
endif()

if(NOT SQ_RAYLIB_FOUND)
  option(USE_FETCH_RAYLIB "Fetch raylib from GitHub if not found locally" OFF)
  if(USE_FETCH_RAYLIB)
    message(STATUS "raylib not found locally — fetching from GitHub (may take a while)")
    include(FetchContent)
    FetchContent_Declare(raylib
      GIT_REPOSITORY https://github.com/raysan5/raylib.git
      GIT_TAG 5.5
      GIT_SHALLOW TRUE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
    set(SQ_RAYLIB_FOUND TRUE)
    set(SQ_RAYLIB_VIA   "FetchContent")
  else()
    message(FATAL_ERROR
      "raylib not found.\n"
      "  On NixOS: run `nix-shell` (a shell.nix is provided) or install\n"
      "  raylib via `nix-shell -p raylib pkg-config` and then set\n"
      "  PKG_CONFIG_PATH=\"$(nix eval --raw nixpkgs#raylib)/lib/pkgconfig\".\n"
      "  Or configure with -DUSE_FETCH_RAYLIB=ON to clone from GitHub.")
  endif()
endif()

message(STATUS "Using raylib via: ${SQ_RAYLIB_VIA}")

# ------------------------------------------------------------------
# Main executable
# ------------------------------------------------------------------
file(GLOB_RECURSE SQ_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/*.c")

add_executable(studyquest ${SQ_SOURCES})
target_include_directories(studyquest PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(RAYLIB_PC_FOUND)
  target_include_directories(studyquest PRIVATE ${RAYLIB_PC_INCLUDE_DIRS})
  target_link_directories   (studyquest PRIVATE ${RAYLIB_PC_LIBRARY_DIRS})
  target_link_libraries    (studyquest PRIVATE ${RAYLIB_PC_LIBRARIES})
  target_compile_options   (studyquest PRIVATE ${RAYLIB_PC_CFLAGS_OTHER})
  target_link_options      (studyquest PRIVATE ${RAYLIB_PC_LDFLAGS_OTHER})
elseif(TARGET raylib)
  target_link_libraries(studyquest PRIVATE raylib)
else()
  target_include_directories(studyquest PRIVATE ${RAYLIB_INCLUDE_DIR})
  target_link_libraries    (studyquest PRIVATE ${RAYLIB_LIBRARY})
endif()

target_link_libraries(studyquest PRIVATE m Threads::Threads)

if(WIN32)
  target_link_libraries(studyquest PRIVATE winmm)
endif()

# ------------------------------------------------------------------
# Tests — pure logic, no raylib calls
# ------------------------------------------------------------------
add_executable(studyquest_tests
  ${CMAKE_SOURCE_DIR}/tests/test_core.c
  ${CMAKE_SOURCE_DIR}/src/cards/cards.c
  ${CMAKE_SOURCE_DIR}/src/study/scheduler.c
  ${CMAKE_SOURCE_DIR}/src/player/player.c
  ${CMAKE_SOURCE_DIR}/src/player/achievements.c
)
target_include_directories(studyquest_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(RAYLIB_PC_FOUND)
  target_include_directories(studyquest_tests PRIVATE ${RAYLIB_PC_INCLUDE_DIRS})
elseif(RAYLIB_INCLUDE_DIR)
  target_include_directories(studyquest_tests PRIVATE ${RAYLIB_INCLUDE_DIR})
endif()

target_link_libraries(studyquest_tests PRIVATE m Threads::Threads)
