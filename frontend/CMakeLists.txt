# The Qt frontend: a cover-art library grid (LibraryWindow). See
# docs/architecture.md for what it still grows into: a tray-capable process
# that can supervise mirad itself when systemd isn't managing it.
# It is a separate binary and a separate CMake project scope
# from mira_core: the frontend talks to the daemon exclusively over the REST
# API in docs/api.md, never by linking backend code directly.
#
# cmake_minimum_required + project() here are what let this directory also
# be configured standalone (cmake -S frontend -B frontend/build), for
# frontend-only iteration without touching the backend build at all, not
# required when pulled in via the root's add_subdirectory(frontend), but
# harmless there too (a nested project() just declares a sub-project).
cmake_minimum_required(VERSION 3.20)
project(mira-gui LANGUAGES CXX)

find_package(Qt6 6.5 QUIET COMPONENTS Widgets DBus Network)

if(NOT Qt6_FOUND)
  message(STATUS "Qt6 (>= 6.5, Widgets) not found, skipping mira-gui. "
                 "The backend (mirad, mira) does not depend on it.")
  return()
endif()

message(STATUS "Qt6 ${Qt6_VERSION} found, building mira-gui")

qt_standard_project_setup()

qt_add_executable(mira-gui
  main.cpp

  # client/: everything that talks to mirad, and nothing that draws.
  client/Async.h
  client/EventHub.cpp
  client/EventHub.h
  client/EventStream.cpp
  client/EventStream.h
  client/Jobs.cpp
  client/Jobs.h
  client/JsonMapping.cpp
  client/JsonMapping.h
  client/MiradClient.cpp
  client/MiradClient.h
  client/Transport.cpp
  client/Transport.h
  client/Types.h

  # ui/: presentation shared by more than one view or dialog.
  ui/AboutPanel.cpp
  ui/AboutPanel.h
  ui/ArtPickerPanel.cpp
  ui/ArtPickerPanel.h
  ui/ArtworkStore.cpp
  ui/ArtworkStore.h
  ui/ContinueRow.cpp
  ui/ContinueRow.h
  ui/CoverArt.cpp
  ui/CoverArt.h
  ui/DaemonSupervisor.cpp
  ui/DaemonSupervisor.h
  ui/DownloadTracker.cpp
  ui/DownloadTracker.h
  ui/DownloadsPanel.cpp
  ui/DownloadsPanel.h
  ui/GameLibraryModel.cpp
  ui/GameLibraryModel.h
  ui/GameActions.cpp
  ui/GameActions.h
  ui/HeroBackdrop.cpp
  ui/HeroBackdrop.h
  ui/HoverCard.cpp
  ui/HoverCard.h
  ui/GamePresentation.h
  ui/LibrarySort.cpp
  ui/LibrarySort.h
  ui/GameTileDelegate.cpp
  ui/GameTileDelegate.h
  ui/Icons.cpp
  ui/Icons.h
  ui/KeyBindings.cpp
  ui/KeyBindings.h
  ui/Notify.cpp
  ui/Notify.h
  ui/PopupDialog.cpp
  ui/PopupDialog.h
  ui/SidebarGames.cpp
  ui/SidebarGames.h
  ui/ManageSourcesCard.cpp
  ui/ManageSourcesCard.h
  ui/InstallPromptCard.cpp
  ui/InstallPromptCard.h
  ui/SidebarStyleCard.cpp
  ui/SidebarStyleCard.h
  ui/Sources.cpp
  ui/Sources.h
  ui/SystemNotifier.cpp
  ui/SystemNotifier.h
  ui/Shortcuts.cpp
  ui/Shortcuts.h
  ui/Theme.cpp
  ui/Theme.h
  ui/TabRow.cpp
  ui/TabRow.h
  ui/TagEdit.cpp
  ui/TagEdit.h
  ui/TileGrid.cpp
  ui/TileGrid.h
  ui/TileView.cpp
  ui/TileView.h
  ui/ToolTip.cpp
  ui/ToolTip.h
  ui/Tray.cpp
  ui/Tray.h
  ui/SettingsNav.cpp
  ui/SettingsNav.h
  ui/SettingsCard.cpp
  ui/SettingsCard.h
  ui/SettingsColumns.cpp
  ui/SettingsColumns.h
  ui/ShortcutEdit.cpp
  ui/ShortcutEdit.h
  ui/AppearancePreviews.cpp
  ui/AppearancePreviews.h
  ui/ListEdit.cpp
  ui/ListEdit.h
  ui/KeyValueEdit.cpp
  ui/KeyValueEdit.h
  ui/SettingsSearch.cpp
  ui/SettingsSearch.h
  ui/ErrorHelp.cpp
  ui/ErrorHelp.h
  ui/SettingEditor.cpp
  ui/SettingEditor.h
  ui/SettingsPanel.cpp
  ui/SettingsPanel.h
  ui/GameEditForm.cpp
  ui/GameEditForm.h

  # views/: the library window and its full-page views.
  views/LibraryWindow.cpp
  views/LibraryWindow.h
  views/RunnersPage.cpp
  views/RunnersPage.h
  views/SourcePage.cpp
  views/SourcePage.h
  views/SourceSettingsCard.cpp
  views/SourceSettingsCard.h

  # dialogs/
  dialogs/AddManualGameDialog.cpp
  dialogs/AddManualGameDialog.h
  dialogs/DeleteGameDialog.cpp
  dialogs/DeleteGameDialog.h
  dialogs/DesktopEntryImportDialog.cpp
  dialogs/DesktopEntryImportDialog.h
  dialogs/GameDetailPageDialog.cpp
  dialogs/GameDetailPageDialog.h
  dialogs/InstallGameDialog.cpp
  dialogs/InstallGameDialog.h
  dialogs/ItchCollectionsDialog.cpp
  dialogs/ItchCollectionsDialog.h
  dialogs/LogViewerDialog.cpp
  dialogs/LogViewerDialog.h
  dialogs/RunInPrefixDialog.cpp
  dialogs/RunInPrefixDialog.h
  dialogs/OverridesEditor.cpp
  dialogs/OverridesEditor.h
  dialogs/WinetricksDialog.cpp
  dialogs/WinetricksDialog.h
)

# The placeholder app icon, embedded so mira-gui has a window/taskbar icon
# regardless of whether it's installed (packaging/mira.desktop's Icon=mira
# is the separate, install-time path via the hicolor theme; see the root
# CMakeLists.txt's install() rules for that copy).
set(MIRA_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icons/hicolor")
qt_add_resources(mira-gui "icons"
  PREFIX "/icons"
  BASE "${MIRA_ICON_DIR}"
  FILES
    "${MIRA_ICON_DIR}/16x16/apps/mira.png"
    "${MIRA_ICON_DIR}/32x32/apps/mira.png"
    "${MIRA_ICON_DIR}/48x48/apps/mira.png"
    "${MIRA_ICON_DIR}/64x64/apps/mira.png"
    "${MIRA_ICON_DIR}/128x128/apps/mira.png"
    "${MIRA_ICON_DIR}/256x256/apps/mira.png"
)

# The stylesheet and the themes that fill it in (ui/Theme.cpp). Embedded so a
# build tree, an AppImage and an installed copy all theme themselves the same
# way; a user's own themes are read from ~/.config/mira/themes at run time.
qt_add_resources(mira-gui "themes"
  PREFIX "/themes"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/themes"
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/themes/base.qss"
    "${CMAKE_CURRENT_SOURCE_DIR}/themes/mira-dark.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/themes/mira-light.toml"
)

# mira_third_party is header-only (httplib.h, json.hpp) and carries no
# backend code, so linking it does not violate the "never link mira_core"
# rule in docs/architecture.md: it's the same REST-client toolkit `mira`
# uses, not a shortcut past the API.
target_link_libraries(mira-gui PRIVATE Qt6::Widgets Qt6::DBus Qt6::Network mira_third_party Threads::Threads)
target_compile_features(mira-gui PRIVATE cxx_std_20)

# The version the About panel shows. CMAKE_PROJECT_VERSION is the root
# project's, which a standalone frontend configure does not have.
set(MIRA_GUI_VERSION "${CMAKE_PROJECT_VERSION}")
if(NOT MIRA_GUI_VERSION)
  set(MIRA_GUI_VERSION "dev")
endif()
target_compile_definitions(mira-gui PRIVATE MIRA_VERSION="${MIRA_GUI_VERSION}")

set_target_properties(mira-gui PROPERTIES
  WIN32_EXECUTABLE OFF
  MACOSX_BUNDLE OFF
)

# Frontend unit tests. Deliberately its own executable rather than more
# files in the root's tests/: that target links mira_core, and a frontend
# test that could reach backend code would be able to "pass" against an
# implementation the real mira-gui never talks to. This one links only the
# header-only third-party deps the client itself uses.
#
# Only the layers with no Qt event loop and no socket in them are covered
# here: client/JsonMapping (JSON <-> Types.h) and the parts of the client
# that are pure functions. Anything needing a live mirad is exercised
# against the real daemon instead; see docs/architecture.md.
file(GLOB MIRA_GUI_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(mira_gui_tests
  ${MIRA_GUI_TEST_SOURCES}
  client/JsonMapping.cpp
  client/MiradClient.cpp
  client/Jobs.cpp
  client/EventHub.cpp
  client/EventStream.cpp
  ui/LibrarySort.cpp
  ui/GameLibraryModel.cpp
  ui/Theme.cpp
  ui/DaemonSupervisor.cpp
  ui/DownloadTracker.cpp
  ui/SettingsSearch.cpp
  ui/SettingsColumns.cpp
  ui/ErrorHelp.cpp
  client/Transport.cpp
)
target_include_directories(mira_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Widgets only for the game model's headers (its roles are the tile
# delegate's) and theme colors; no test opens a window or runs an event loop.
target_link_libraries(mira_gui_tests PRIVATE mira_third_party Qt6::Widgets)
target_compile_features(mira_gui_tests PRIVATE cxx_std_20)
target_compile_options(mira_gui_tests PRIVATE -Wall -Wextra)
add_test(NAME frontend_unit COMMAND mira_gui_tests)

include(GNUInstallDirs)
install(TARGETS mira-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
