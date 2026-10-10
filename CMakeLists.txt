cmake_minimum_required(VERSION 3.16)
project(studyquest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Needed for strcasestr (GNU), strnlen (POSIX.1-2008), and strdup.
add_compile_definitions(_GNU_SOURCE)
add_compile_definitions(_DEFAULT_SOURCE)

find_package(Threads REQUIRED)

# ====================================================================
# raylib — pkg-config, then CMake config, then manual find, then fetch.
# ====================================================================
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
    message(STATUS "raylib not found locally — fetching from GitHub")
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
      "  On NixOS: run inside `nix-shell` (shell.nix provided).\n"
      "  Or configure with -DUSE_FETCH_RAYLIB=ON to clone from GitHub.")
  endif()
endif()

message(STATUS "raylib: ${SQ_RAYLIB_VIA}")

# ====================================================================
# miniz — ZIP reading. Prefer vendored source under third_party/miniz.
# ====================================================================
set(SQ_MINIZ_TARGET "")
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c")
  add_library(sq_miniz STATIC third_party/miniz/miniz.c)
  target_include_directories(sq_miniz PUBLIC third_party/miniz)
  set(SQ_MINIZ_TARGET sq_miniz)
  message(STATUS "miniz: vendored")
else()
  find_library(MINIZ_LIBRARY NAMES miniz minizip)
  find_path(MINIZ_INCLUDE_DIR miniz.h)
  if(MINIZ_LIBRARY AND MINIZ_INCLUDE_DIR)
    add_library(sq_miniz UNKNOWN IMPORTED)
    set_target_properties(sq_miniz PROPERTIES
      IMPORTED_LOCATION "${MINIZ_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${MINIZ_INCLUDE_DIR}")
    set(SQ_MINIZ_TARGET sq_miniz)
    message(STATUS "miniz: system (${MINIZ_LIBRARY})")
  else()
    message(FATAL_ERROR
      "miniz not found. Place miniz.c and miniz.h under third_party/miniz/.")
  endif()
endif()

# ====================================================================
# SQLite — reading collection.anki2/anki21.
# ====================================================================
set(SQ_SQLITE_TARGET "")
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
  add_library(sq_sqlite STATIC third_party/sqlite/sqlite3.c)
  target_include_directories(sq_sqlite PUBLIC third_party/sqlite)
  target_compile_definitions(sq_sqlite PUBLIC
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1)
  # GCC 14+ false-positive inside SQLite itself; harmless in practice.
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
    target_compile_options(sq_sqlite PRIVATE -Wno-stringop-overread)
  endif()
  set(SQ_SQLITE_TARGET sq_sqlite)
  message(STATUS "SQLite: vendored")
else()
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    set(SQ_SQLITE_TARGET SQLite::SQLite3)
    message(STATUS "SQLite: system")
  else()
    message(FATAL_ERROR
      "SQLite not found. Place sqlite3.c and sqlite3.h under third_party/sqlite/.")
  endif()
endif()

# ====================================================================
# zstd — required for modern collection.anki21b.
# System pkg-config first, then vendored source under third_party/zstd/.
# ====================================================================
set(SQ_ZSTD_TARGET "")
pkg_check_modules(ZSTD_PC QUIET libzstd)
if(ZSTD_PC_FOUND)
  add_library(sq_zstd INTERFACE)
  target_include_directories(sq_zstd INTERFACE ${ZSTD_PC_INCLUDE_DIRS})
  target_link_directories   (sq_zstd INTERFACE ${ZSTD_PC_LIBRARY_DIRS})
  target_link_libraries     (sq_zstd INTERFACE ${ZSTD_PC_LIBRARIES})
  set(SQ_ZSTD_TARGET sq_zstd)
  message(STATUS "zstd: system via pkg-config (${ZSTD_PC_VERSION})")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/zstd/lib")
  file(GLOB ZSTD_COMMON  third_party/zstd/lib/common/*.c)
  file(GLOB ZSTD_DECOMP  third_party/zstd/lib/decompress/*.c)
  add_library(sq_zstd STATIC ${ZSTD_COMMON} ${ZSTD_DECOMP})
  target_include_directories(sq_zstd PUBLIC
    third_party/zstd/lib
    third_party/zstd/lib/common)
  target_compile_definitions(sq_zstd PRIVATE
    ZSTD_DISABLE_ASM
    ZSTD_LIB_COMPRESSION=0)
  set(SQ_ZSTD_TARGET sq_zstd)
  message(STATUS "zstd: vendored (decompressor only)")
else()
  message(FATAL_ERROR
    "zstd not found. Either:\n"
    "  (A) add `zstd` to shell.nix buildInputs, then re-enter nix-shell\n"
    "  (B) extract zstd-1.5.6 into third_party/zstd/")
endif()

# ====================================================================
# cJSON — for models/decks/media-map JSON.
# ====================================================================
set(SQ_CJSON_TARGET "")
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/cjson/cJSON.c")
  add_library(sq_cjson STATIC third_party/cjson/cJSON.c)
  target_include_directories(sq_cjson PUBLIC third_party/cjson)
  set(SQ_CJSON_TARGET sq_cjson)
  message(STATUS "cJSON: vendored")
else()
  pkg_check_modules(CJSON_PC QUIET libcjson)
  if(CJSON_PC_FOUND)
    add_library(sq_cjson INTERFACE)
    target_include_directories(sq_cjson INTERFACE ${CJSON_PC_INCLUDE_DIRS})
    target_link_libraries     (sq_cjson INTERFACE ${CJSON_PC_LIBRARIES})
    set(SQ_CJSON_TARGET sq_cjson)
    message(STATUS "cJSON: system via pkg-config")
  else()
    message(FATAL_ERROR
      "cJSON not found. Place cJSON.c and cJSON.h under third_party/cjson/.")
  endif()
endif()

# ====================================================================
# Main application
# ====================================================================
file(GLOB_RECURSE SQ_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/*.c")

add_executable(studyquest ${SQ_SOURCES})
target_include_directories(studyquest PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party)

# ---- raylib linking -------------------------------------------------
if(RAYLIB_PC_FOUND)
  target_include_directories(studyquest PRIVATE ${RAYLIB_PC_INCLUDE_DIRS})
  target_link_directories    (studyquest PRIVATE ${RAYLIB_PC_LIBRARY_DIRS})
  target_link_libraries      (studyquest PRIVATE ${RAYLIB_PC_LIBRARIES})
  target_compile_options     (studyquest PRIVATE ${RAYLIB_PC_CFLAGS_OTHER})
  target_link_options        (studyquest PRIVATE ${RAYLIB_PC_LDFLAGS_OTHER})
elseif(TARGET raylib)
  target_link_libraries(studyquest PRIVATE raylib)
else()
  target_include_directories(studyquest PRIVATE ${RAYLIB_INCLUDE_DIR})
  target_link_libraries      (studyquest PRIVATE ${RAYLIB_LIBRARY})
endif()

# ---- vendored / system dependencies ---------------------------------
target_link_libraries(studyquest PRIVATE
  ${SQ_MINIZ_TARGET}
  ${SQ_SQLITE_TARGET}
  ${SQ_ZSTD_TARGET}
  ${SQ_CJSON_TARGET}
  m
  Threads::Threads)

if(WIN32)
  target_link_libraries(studyquest PRIVATE winmm)
endif()

# ====================================================================
# libwebp — WebP image decoding.
# NOTE: must come AFTER add_executable(studyquest ...) because it
# modifies that target. Sets SQ_HAVE_WEBP=1 only when the library is
# actually linkable, so the compiler and linker agree.
# ====================================================================
pkg_check_modules(WEBP_PC QUIET libwebp)

if(WEBP_PC_FOUND)
  message(STATUS "libwebp: system via pkg-config (${WEBP_PC_VERSION})")
  target_include_directories(studyquest PRIVATE ${WEBP_PC_INCLUDE_DIRS})
  target_link_directories    (studyquest PRIVATE ${WEBP_PC_LIBRARY_DIRS})
  target_link_libraries      (studyquest PRIVATE ${WEBP_PC_LIBRARIES})
  target_compile_definitions (studyquest PRIVATE SQ_HAVE_WEBP=1)
else()
  find_path(WEBP_INCLUDE_DIR webp/decode.h)
  find_library(WEBP_LIBRARY NAMES webp)
  if(WEBP_INCLUDE_DIR AND WEBP_LIBRARY)
    message(STATUS "libwebp: manual find (${WEBP_LIBRARY})")
    target_include_directories(studyquest PRIVATE ${WEBP_INCLUDE_DIR})
    target_link_libraries      (studyquest PRIVATE ${WEBP_LIBRARY})
    target_compile_definitions (studyquest PRIVATE SQ_HAVE_WEBP=1)
  else()
    message(FATAL_ERROR
      "libwebp is required for imported Anki WebP images.\n"
      "  On NixOS: use the provided shell.nix (it includes libwebp).")
  endif()
endif()

# ====================================================================
# Tests — logic and Anki import/media compatibility.
# ====================================================================
add_executable(studyquest_tests
  ${CMAKE_SOURCE_DIR}/tests/test_core.c
  ${CMAKE_SOURCE_DIR}/tests/test_raylib_stubs.c
  ${CMAKE_SOURCE_DIR}/src/cards/cards.c
  ${CMAKE_SOURCE_DIR}/src/storage/save.c
  ${CMAKE_SOURCE_DIR}/src/core/utf8.c
  ${CMAKE_SOURCE_DIR}/src/study/scheduler.c
  ${CMAKE_SOURCE_DIR}/src/player/player.c
  ${CMAKE_SOURCE_DIR}/src/player/achievements.c
  ${CMAKE_SOURCE_DIR}/src/world/world_data.c
  ${CMAKE_SOURCE_DIR}/src/world/world_progress.c
)
target_include_directories(studyquest_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party)

if(RAYLIB_PC_FOUND)
  target_include_directories(studyquest_tests PRIVATE ${RAYLIB_PC_INCLUDE_DIRS})
elseif(RAYLIB_INCLUDE_DIR)
  target_include_directories(studyquest_tests PRIVATE ${RAYLIB_INCLUDE_DIR})
endif()

target_link_libraries(studyquest_tests PRIVATE m Threads::Threads)

# Focused integration/component coverage for APKG, SQLite, UTF-8, HTML/media,
# media extraction, WebP decoding, card conversion and persistence.
add_executable(studyquest_anki_import_tests
  ${CMAKE_SOURCE_DIR}/tests/test_anki_import.c
  ${CMAKE_SOURCE_DIR}/tests/test_raylib_stubs.c
  ${CMAKE_SOURCE_DIR}/src/core/utf8.c
  ${CMAKE_SOURCE_DIR}/src/import/anki_html.c
  ${CMAKE_SOURCE_DIR}/src/import/anki_parser.c
  ${CMAKE_SOURCE_DIR}/src/import/anki_json.c
  ${CMAKE_SOURCE_DIR}/src/import/anki_convert.c
  ${CMAKE_SOURCE_DIR}/src/import/apkg_importer.c
  ${CMAKE_SOURCE_DIR}/src/import/zip_stream.c
  ${CMAKE_SOURCE_DIR}/src/import/media_importer.c
  ${CMAKE_SOURCE_DIR}/src/import/import_manager.c
  ${CMAKE_SOURCE_DIR}/src/cards/cards.c
  ${CMAKE_SOURCE_DIR}/src/storage/save.c
  ${CMAKE_SOURCE_DIR}/src/player/player.c
  ${CMAKE_SOURCE_DIR}/src/player/achievements.c
  ${CMAKE_SOURCE_DIR}/src/study/scheduler.c
  ${CMAKE_SOURCE_DIR}/src/world/world_data.c
  ${CMAKE_SOURCE_DIR}/src/world/world_progress.c
)
target_include_directories(studyquest_anki_import_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/third_party/cjson
  ${CMAKE_SOURCE_DIR}/third_party/miniz
)
if(RAYLIB_PC_FOUND)
  target_include_directories(studyquest_anki_import_tests PRIVATE ${RAYLIB_PC_INCLUDE_DIRS})
elseif(RAYLIB_INCLUDE_DIR)
  target_include_directories(studyquest_anki_import_tests PRIVATE ${RAYLIB_INCLUDE_DIR})
endif()
if(WEBP_PC_FOUND)
  target_include_directories(studyquest_anki_import_tests PRIVATE ${WEBP_PC_INCLUDE_DIRS})
  target_link_directories(studyquest_anki_import_tests PRIVATE ${WEBP_PC_LIBRARY_DIRS})
  target_link_libraries(studyquest_anki_import_tests PRIVATE ${WEBP_PC_LIBRARIES})
elseif(WEBP_INCLUDE_DIR AND WEBP_LIBRARY)
  target_include_directories(studyquest_anki_import_tests PRIVATE ${WEBP_INCLUDE_DIR})
  target_link_libraries(studyquest_anki_import_tests PRIVATE ${WEBP_LIBRARY})
else()
  message(FATAL_ERROR "The Anki importer tests require libwebp.")
endif()
target_link_libraries(studyquest_anki_import_tests PRIVATE
  ${SQ_MINIZ_TARGET}
  ${SQ_SQLITE_TARGET}
  ${SQ_ZSTD_TARGET}
  ${SQ_CJSON_TARGET}
  m Threads::Threads)

# World progression: data model, rules, anti-farm, persistence, v8 migration.
add_executable(studyquest_world_tests
  ${CMAKE_SOURCE_DIR}/tests/test_world.c
  ${CMAKE_SOURCE_DIR}/tests/test_raylib_stubs.c
  ${CMAKE_SOURCE_DIR}/src/cards/cards.c
  ${CMAKE_SOURCE_DIR}/src/storage/save.c
  ${CMAKE_SOURCE_DIR}/src/core/utf8.c
  ${CMAKE_SOURCE_DIR}/src/study/scheduler.c
  ${CMAKE_SOURCE_DIR}/src/player/player.c
  ${CMAKE_SOURCE_DIR}/src/player/achievements.c
  ${CMAKE_SOURCE_DIR}/src/world/world_data.c
  ${CMAKE_SOURCE_DIR}/src/world/world_progress.c
)
target_include_directories(studyquest_world_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party)
if(RAYLIB_PC_FOUND)
  target_include_directories(studyquest_world_tests PRIVATE ${RAYLIB_PC_INCLUDE_DIRS})
elseif(RAYLIB_INCLUDE_DIR)
  target_include_directories(studyquest_world_tests PRIVATE ${RAYLIB_INCLUDE_DIR})
endif()
target_link_libraries(studyquest_world_tests PRIVATE m Threads::Threads)

enable_testing()
add_test(NAME studyquest_core_tests COMMAND studyquest_tests)
add_test(NAME studyquest_world_tests
  COMMAND studyquest_world_tests ${CMAKE_SOURCE_DIR}/tests/fixtures/save_v8_legacy.sav)
add_test(NAME studyquest_anki_import_tests
  COMMAND studyquest_anki_import_tests ${CMAKE_SOURCE_DIR}/tests/fixtures/anki_media.apkg)
