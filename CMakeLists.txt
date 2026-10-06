cmake_minimum_required(VERSION 3.20)
project(mira LANGUAGES CXX VERSION 0.12.3)

# Dependencies are header-only and vendored in third_party/, so a build needs
# nothing fetched at configure time, and the only external dependency is the
# C++ standard library itself.
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

find_package(Threads REQUIRED)

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
target_link_libraries(mira_core PRIVATE mira_flags)
target_link_libraries(mira_core PUBLIC mira_third_party Threads::Threads)

add_executable(mirad src/mirad_main.cpp)
target_link_libraries(mirad PRIVATE mira_core mira_flags)
target_compile_definitions(mirad PRIVATE MIRA_VERSION="${PROJECT_VERSION}")

add_executable(mira src/cli/main.cpp)
target_link_libraries(mira PRIVATE mira_core mira_flags)

# The process mirad actually spawns for a game launch (see proc/Session.h,
# docs/architecture.md), a separate binary, not a mirad-internal thread, so
# a launched game's session survives mirad restarting or crashing mid-game.
add_executable(mira-run src/wrapper/main.cpp)
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

  # `cmake --build <preset> --target appimage` -> build/<preset>/Mira-x86_64.AppImage.
  # Needs mira-gui built (it's the whole point of bundling), so this lives
  # inside the same guard as run-gui above rather than being unconditional.
  include(cmake/AppImage.cmake)
endif()
