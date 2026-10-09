cmake_minimum_required(VERSION 3.20)
project(mira LANGUAGES C CXX VERSION 0.13.2)

# Dependencies are vendored in third_party/ (header-only, except SQLite's one
# C file), so a build needs nothing fetched at configure time, and the only
# external dependency is the C++ standard library itself.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MIRA_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(MIRA_TSAN "Build with ThreadSanitizer" OFF)
option(MIRA_WERROR "Treat warnings as errors" OFF)

# Run on older glibc and libstdc++ than the build machine has, so a binary built on a rolling
# distro still starts on an LTS one. Both only change what the binaries ask of the system.
option(MIRA_GLIBC_COMPAT "Bind strtol and friends to their pre-glibc-2.38 versions" ON)
option(MIRA_STATIC_LIBSTDCXX "Link libstdc++ and libgcc statically into mirad, mira and mira-run" ON)

# A compiler cache, when one is installed, makes every rebuild after the first cheap.
find_program(MIRA_CCACHE NAMES ccache sccache)
# The sloppiness lets it cache files that use the precompiled headers.
if(MIRA_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND}" -E env
      "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime" "${MIRA_CCACHE}")
endif()

find_package(Threads REQUIRED)

add_library(mira_glibc_compat INTERFACE)
if(MIRA_GLIBC_COMPAT AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_options(mira_glibc_compat INTERFACE
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/glibc_compat.h")
endif()

add_library(mira_flags INTERFACE)
target_compile_options(mira_flags INTERFACE -Wall -Wextra -Wpedantic)
if(MIRA_WERROR)
  target_compile_options(mira_flags INTERFACE -Werror)
endif()

# TSan is the sanitizer that matters here: three threads share the in-memory
# game store, settings and event bus. ASan and TSan cannot be combined.
if(MIRA_ASAN AND MIRA_TSAN)
  message(FATAL_ERROR "MIRA_ASAN and MIRA_TSAN are mutually exclusive")
endif()
if(MIRA_ASAN)
  target_compile_options(mira_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(mira_flags INTERFACE -fsanitize=address,undefined)
endif()
if(MIRA_TSAN)
  target_compile_options(mira_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(mira_flags INTERFACE -fsanitize=thread)
endif()

add_library(mira_third_party INTERFACE)
# httplib's default backlog of 5 refuses connections when a client sends a burst of requests.
target_compile_definitions(mira_third_party INTERFACE TOML_EXCEPTIONS=0 CPPHTTPLIB_LISTEN_BACKLOG=128)
target_include_directories(mira_third_party SYSTEM INTERFACE third_party)

# SQLite's amalgamation, built the same for every package rather than linked
# against whichever version (and options) a distro ships.
add_library(mira_sqlite STATIC third_party/sqlite3.c)
target_include_directories(mira_sqlite SYSTEM PUBLIC third_party)
target_compile_definitions(mira_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DQS=0
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED)
target_link_libraries(mira_sqlite PRIVATE mira_glibc_compat)
set_target_properties(mira_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)

# stb's image loader, resizer and writer, for shrinking downloaded art.
add_library(mira_stb STATIC third_party/stb.c)
target_include_directories(mira_stb SYSTEM PUBLIC third_party)
target_link_libraries(mira_stb PRIVATE mira_glibc_compat)
set_target_properties(mira_stb PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB_RECURSE MIRA_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER MIRA_CORE_SOURCES EXCLUDE REGEX "src/mirad_main\\.cpp$")
list(FILTER MIRA_CORE_SOURCES EXCLUDE REGEX "src/cli/.*\\.cpp$")
list(FILTER MIRA_CORE_SOURCES EXCLUDE REGEX "src/wrapper/.*\\.cpp$")

# mira_flags (-Wall -Wextra -Wpedantic, optionally -Werror) is applied PRIVATE
# everywhere rather than propagated via mira_core's public link interface: it
# needs to bind to each target's own compile command, and a downstream
# consumer's own compile options cannot reliably out-order flags inherited
# through a public dependency (mira_tests below needs to locally silence two
# warnings that originate inside the vendored doctest.h, which only works if
# mira_core is not itself broadcasting -Werror to it).
add_library(mira_core STATIC ${MIRA_CORE_SOURCES})
target_include_directories(mira_core PUBLIC src)
target_link_libraries(mira_core PRIVATE mira_flags mira_sqlite mira_stb)
target_link_libraries(mira_core PUBLIC mira_third_party mira_glibc_compat Threads::Threads)

option(MIRA_PCH "Precompile the common headers" ON)

# Clang writes a timestamp into a precompiled header, which changes the file on every build and so
# keeps ccache from ever matching a file that uses it.
if(MIRA_PCH AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Xclang -fno-pch-timestamp)
endif()

# The headers nearly every file pulls in (json.hpp alone is ~900 KB), parsed once for the whole library.
# The same headers for every target that compiles code of its own: mira_core's, and the daemon's, the CLI's, the
# wrapper's and the tests' (which add doctest). Without them each of those files parsed <json.hpp> and the standard
# library again, and they were the slowest files in the build.
set(MIRA_PCH_HEADERS
  <json.hpp> <algorithm> <chrono> <filesystem> <format> <functional> <map> <memory> <optional>
  <string> <string_view> <vector> <expected>)
if(MIRA_PCH)
  target_precompile_headers(mira_core PRIVATE ${MIRA_PCH_HEADERS})
endif()

# The GUI keeps the shared runtime: the Qt libraries it loads use it too, and the AppImage bundles that one.
# Its glibc 2.36+ calls are answered by cmake/glibc_shims.cpp.
set_source_files_properties(cmake/glibc_shims.cpp PROPERTIES COMPILE_DEFINITIONS MIRA_NO_GLIBC_SYMVER)
function(mira_static_runtime target)
  if(MIRA_STATIC_LIBSTDCXX AND NOT MIRA_ASAN AND NOT MIRA_TSAN)
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    target_sources(${target} PRIVATE cmake/glibc_shims.cpp)
  endif()
endfunction()

add_executable(mirad src/mirad_main.cpp)
mira_static_runtime(mirad)
target_link_libraries(mirad PRIVATE mira_core mira_flags)
target_compile_definitions(mirad PRIVATE MIRA_VERSION="${PROJECT_VERSION}")
if(MIRA_PCH)
  # Its own: a PCH can only be shared by targets with the same definitions, and this one adds MIRA_VERSION.
  target_precompile_headers(mirad PRIVATE ${MIRA_PCH_HEADERS})
endif()

add_executable(mira src/cli/main.cpp)
mira_static_runtime(mira)
target_link_libraries(mira PRIVATE mira_core mira_flags)
if(MIRA_PCH)
  target_precompile_headers(mira REUSE_FROM mira_core)
endif()

# The process mirad actually spawns for a game launch (see proc/Session.h,
# docs/architecture.md), a separate binary, not a mirad-internal thread, so
# a launched game's session survives mirad restarting or crashing mid-game.
add_executable(mira-run src/wrapper/main.cpp)
mira_static_runtime(mira-run)
target_link_libraries(mira-run PRIVATE mira_core mira_flags)

# A target's own compile options are always ordered before flags inherited
# through target_link_libraries, on every CMake generator, so a local
# -Wno-... cannot override a dependency's -Werror by appearing "later" in the
# test target's own option list. mira_tests therefore gets a deliberately
# looser flag set (no -Wpedantic, no -Werror) rather than mira_flags: the
# vendored doctest.h trips both (__COUNTER__ as a "C2y extension", and a
# #warning from <ciso646> that -isystem does not suppress, since #warning
# bypasses system-header filtering by design), and none of that reflects on
# our own test code, which mira_core's own -Werror build already covers by
# virtue of exercising the same headers.
add_library(mira_flags_test INTERFACE)
target_compile_options(mira_flags_test INTERFACE -Wall -Wextra)

enable_testing()
file(GLOB MIRA_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp tests/support/*.cpp)
add_executable(mira_tests ${MIRA_TEST_SOURCES})
target_include_directories(mira_tests PRIVATE tests)
target_link_libraries(mira_tests PRIVATE mira_core mira_flags_test)
if(MIRA_PCH)
  target_precompile_headers(mira_tests PRIVATE ${MIRA_PCH_HEADERS} <doctest.h>)
  # It defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN before including doctest, which a precompiled doctest.h would
  # make too late.
  set_source_files_properties(tests/main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
add_test(NAME unit COMMAND mira_tests)

include(GNUInstallDirs)
install(TARGETS mirad mira mira-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/mirad.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES packaging/mira.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(icon_size IN ITEMS 16 32 48 64 128 256)
  install(FILES packaging/icons/hicolor/${icon_size}x${icon_size}/apps/mira.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps)
endforeach()

# The AppImage, .deb and .rpm, built in an Ubuntu 22.04 container (targets appimage, packages); needs docker or podman.
include(cmake/AppImageContainer.cmake)

option(MIRA_BUILD_FRONTEND "Build the Qt frontend skeleton if Qt6 is available" ON)
if(MIRA_BUILD_FRONTEND)
  add_subdirectory(frontend)
endif()

# Dev convenience only, not part of the install story: builds mira-gui (and
# whatever it depends on) then execs it, so trying the GUI is one command.
# Only defined when mira-gui actually exists as a target: MIRA_BUILD_FRONTEND
# off, or Qt6 missing, means frontend/CMakeLists.txt returns before defining
# it (see the "Qt6 not found" branch there).
if(TARGET mira-gui)
  add_custom_target(run-gui
    COMMAND $<TARGET_FILE:mira-gui>
    DEPENDS mira-gui
    USES_TERMINAL
    COMMENT "Launching mira-gui")

  # `cmake --build <preset> --target appimage-host` -> build/<preset>/Mira-x86_64.AppImage, with the host's libraries.
  # Needs mira-gui built (it's the whole point of bundling), so this lives
  # inside the same guard as run-gui above rather than being unconditional.
  include(cmake/AppImage.cmake)
  if(TARGET appimage-host)
    include(cmake/Packages.cmake)
  endif()
endif()
