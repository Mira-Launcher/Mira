# `cmake --build <preset> --target appimage-container` -> build/<preset>/appimage-container/Mira-x86_64.AppImage
#
# Builds the AppImage inside an Ubuntu 22.04 container (packaging/appimage/Dockerfile), so the Qt and
# other libraries it bundles need glibc 2.35 instead of whatever the host has. The plain `appimage`
# target bundles the host's libraries, which only run on a distribution as new as the host.
#
# Needs docker or podman. Nothing else is needed on the host, not even Qt. The first run builds the
# image (Qt is downloaded into it); later runs reuse it.

find_program(MIRA_CONTAINER_ENGINE NAMES podman docker)
if(NOT MIRA_CONTAINER_ENGINE)
  message(STATUS "docker or podman not found, skipping the 'appimage-container' target")
  return()
endif()

set(MIRA_CONTAINER_IMAGE "mira-appimage-builder")
set(MIRA_CONTAINER_OUT "${CMAKE_BINARY_DIR}/appimage-container")

# The files the container writes belong to the user who ran the build, not root.
execute_process(COMMAND id -u OUTPUT_VARIABLE MIRA_UID OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND id -g OUTPUT_VARIABLE MIRA_GID OUTPUT_STRIP_TRAILING_WHITESPACE)
if(MIRA_CONTAINER_ENGINE MATCHES "podman$")
  set(MIRA_CONTAINER_USER_ARGS --userns=keep-id)
else()
  set(MIRA_CONTAINER_USER_ARGS --user "${MIRA_UID}:${MIRA_GID}")
endif()

add_custom_target(appimage-container
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${MIRA_CONTAINER_OUT}"
  COMMAND "${MIRA_CONTAINER_ENGINE}" build
          -t "${MIRA_CONTAINER_IMAGE}"
          -f "${CMAKE_SOURCE_DIR}/packaging/appimage/Dockerfile"
          "${CMAKE_SOURCE_DIR}/packaging/appimage"
  COMMAND "${MIRA_CONTAINER_ENGINE}" run --rm
          ${MIRA_CONTAINER_USER_ARGS}
          -e HOME=/tmp
          -v "${CMAKE_SOURCE_DIR}:/src:ro"
          -v "${MIRA_CONTAINER_OUT}:/out"
          "${MIRA_CONTAINER_IMAGE}"
          /src/packaging/appimage/build-in-container.sh
  USES_TERMINAL
  COMMENT "Building the Mira AppImage in an Ubuntu 22.04 container"
  VERBATIM)
