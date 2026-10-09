# `cmake --build <preset> --target packages-host` -> build/<preset>/mira-launcher_<version>_amd64.deb and
# mira-launcher-<version>-1.x86_64.rpm
#
# Both packages carry the tree the AppImage is made from (Mira's binaries with Qt and the libraries
# linuxdeploy bundled), under /usr/lib/mira. From the distribution they need its C library, OpenGL,
# Vulkan and 7-Zip, and recommend SDL3, GameMode, MangoHud and Wine. Built in the container that makes the AppImage, they install on every distribution as new as Ubuntu 22.04.
# `mira` and `mira-gui` in /usr/bin are small scripts that start the bundled ones; mira-gui starts
# mirad itself.
# Only defined next to the appimage target, which makes that tree.

set(MIRA_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/package-root")
set(MIRA_PACKAGE_LIB /usr/lib/mira)

set(MIRA_LAUNCHER_DIR "${CMAKE_BINARY_DIR}/package-launchers")
foreach(program IN ITEMS mira mira-gui)
  set(default_platform "")
  if(program STREQUAL "mira-gui")
    # As in the AppImage: the bundled Qt has the xcb platform plugin, which also runs through XWayland.
    set(default_platform "export QT_QPA_PLATFORM=\"\${QT_QPA_PLATFORM:-xcb}\"\n")
  endif()
  file(WRITE "${MIRA_LAUNCHER_DIR}/${program}"
       "#!/bin/sh\n${default_platform}exec ${MIRA_PACKAGE_LIB}/bin/${program} \"$@\"\n")
  file(CHMOD "${MIRA_LAUNCHER_DIR}/${program}" PERMISSIONS
       OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()

set(CPACK_PACKAGE_NAME mira-launcher)
set(CPACK_PACKAGE_VENDOR "Mira")
set(CPACK_PACKAGE_CONTACT "arigood99@gmail.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Mira-Launcher/Mira")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux game launcher for native games and Windows games via Wine/Proton")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX /)
# Only the staged tree: not the project's own install() rules, which would add its binaries a second time.
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INSTALLED_DIRECTORIES "${MIRA_PACKAGE_ROOT};/")
set(CPACK_SET_DESTDIR OFF)
set(CPACK_STRIP_FILES OFF)
set(CPACK_GENERATOR "DEB;RPM")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
set(CPACK_DEBIAN_PACKAGE_SECTION games)
# Qt and what it loads are bundled; the OpenGL front ends and the C library are the distribution's.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libc6, libopengl0, libglx0, libegl1, libfontconfig1, libfreetype6, libgpg-error0, libvulkan1, 7zip | p7zip-full")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libsdl3-0, gamemode, mangohud, wine, winetricks")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "sqlite3")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_ARCHITECTURE x86_64)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
set(CPACK_RPM_PACKAGE_REQUIRES
    "glibc, libglvnd-opengl, libglvnd-glx, libglvnd-egl, fontconfig, freetype, libgpg-error, vulkan-loader, (7zip or p7zip)")
set(CPACK_RPM_PACKAGE_RECOMMENDS "SDL3, gamemode, mangohud, wine, winetricks")
# The libraries are bundled as linuxdeploy left them: no stripping, and no build-id links into /usr/lib.
set(CPACK_RPM_SPEC_MORE_DEFINE "%global __os_install_post %{nil}\n%define _build_id_links none")
set(CPACK_RPM_PACKAGE_SUGGESTS "sqlite")
# The folders the icon theme, the desktop and systemd own.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/bin /usr/lib /usr/share/applications /usr/share/icons
    /usr/share/icons/hicolor)
foreach(icon_size IN ITEMS 16 32 48 64 128 256)
  list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
       /usr/share/icons/hicolor/${icon_size}x${icon_size} /usr/share/icons/hicolor/${icon_size}x${icon_size}/apps)
endforeach()

include(CPack)

add_custom_target(stage-packages
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MIRA_PACKAGE_ROOT}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${MIRA_PACKAGE_ROOT}/usr/bin" "${MIRA_PACKAGE_ROOT}${MIRA_PACKAGE_LIB}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${MIRA_APPDIR}/usr" "${MIRA_PACKAGE_ROOT}${MIRA_PACKAGE_LIB}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${MIRA_LAUNCHER_DIR}" "${MIRA_PACKAGE_ROOT}/usr/bin"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${MIRA_PACKAGE_ROOT}/usr/share/applications"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/packaging/mira.desktop" "${MIRA_PACKAGE_ROOT}/usr/share/applications"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/packaging/icons/hicolor" "${MIRA_PACKAGE_ROOT}/usr/share/icons/hicolor"
  DEPENDS appimage-host
  COMMENT "Staging the package tree"
  VERBATIM)

add_custom_target(packages-host
  COMMAND "${CMAKE_CPACK_COMMAND}" --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  DEPENDS stage-packages
  COMMENT "Building the .deb and .rpm"
  VERBATIM)
