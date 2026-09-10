# Prepares the Qt runtime next to the framework library so a build tree can be
# used without an install step.
#
# Windows: windeployqt copies the Qt DLLs and the host platform plugin.
# Linux/macOS: the Qt runtime is resolved through the system installation (the
# libraries are linked by absolute path and the plugin prefix is compiled into
# Qt), so there is nothing to deploy - only the offscreen plugin is copied below
# for the ctest runs. The target still exists on those platforms so scripts and
# presets can depend on it unconditionally.
add_custom_target(DeployQt COMMENT "Deploy the Qt runtime next to the framework library")

if (WIN32)
    if (TARGET Qt6::windeployqt)
        get_target_property(WINDEPLOYQT_EXE Qt6::windeployqt IMPORTED_LOCATION_RELEASE)
        if (NOT WINDEPLOYQT_EXE)
            get_target_property(WINDEPLOYQT_EXE Qt6::windeployqt IMPORTED_LOCATION)
        endif()
    endif()

    if (NOT WINDEPLOYQT_EXE)
        message(FATAL_ERROR "Qt6::windeployqt not found")
    endif()

    add_custom_command(
            TARGET DeployQt
            POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE} --verbose 0 $<TARGET_FILE:vi::Appfw>
            COMMENT "--------Run:windeployqt")
else ()
    message(STATUS "DeployQt: no Qt deployment needed on ${CMAKE_SYSTEM_NAME} (windeployqt is Windows-only)")
endif ()

# windeployqt only deploys the platform plugin of the host platform (qwindows*.dll
# on Windows), but test_gui runs from ctest with QT_QPA_PLATFORM=offscreen. Without
# a deployed offscreen plugin Qt aborts before any test runs:
#   qt.qpa.plugin: Could not find the Qt platform plugin "offscreen" in ""
# So copy that plugin next to the one windeployqt placed (Windows) or next to the
# framework library (Linux/macOS, where Qt additionally looks for plugins).
#
# The plugin directory is looked up as: the layout Qt itself reports, then the
# SDK layout relative to Qt6_DIR, then the layouts used by Linux distributions.
set(VINE_QT_PLATFORM_PLUGIN_DIR "")

set(VINE_QT_PLUGIN_CANDIDATES "")
if (DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_PLUGINS)
    list(APPEND VINE_QT_PLUGIN_CANDIDATES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/platforms")
endif()
if (Qt6_DIR)
    get_filename_component(VINE_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
    list(APPEND VINE_QT_PLUGIN_CANDIDATES
            "${VINE_QT_PREFIX}/plugins/platforms"
            "${VINE_QT_PREFIX}/lib/qt6/plugins/platforms"
            "${VINE_QT_PREFIX}/qt6/plugins/platforms")
    unset(VINE_QT_PREFIX)
endif()

foreach (_candidate IN LISTS VINE_QT_PLUGIN_CANDIDATES)
    if (NOT VINE_QT_PLATFORM_PLUGIN_DIR AND EXISTS "${_candidate}")
        set(VINE_QT_PLATFORM_PLUGIN_DIR "${_candidate}")
    endif()
endforeach()
unset(VINE_QT_PLUGIN_CANDIDATES)
unset(_candidate)

if (WIN32)
    # MSVC Debug Qt kits ship 'd'-suffixed plugins (qoffscreend.dll); the other
    # configurations link the release Qt, whose plugin is named qoffscreen.dll.
    set(VINE_OFFSCREEN_PLUGIN "qoffscreen$<$<CONFIG:Debug>:d>.dll")
elseif (APPLE)
    set(VINE_OFFSCREEN_PLUGIN "libqoffscreen.dylib")
else ()
    set(VINE_OFFSCREEN_PLUGIN "libqoffscreen.so")
endif ()

if (VINE_QT_PLATFORM_PLUGIN_DIR AND (WIN32 OR EXISTS "${VINE_QT_PLATFORM_PLUGIN_DIR}/${VINE_OFFSCREEN_PLUGIN}"))
    add_custom_command(
            TARGET DeployQt
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vi::Appfw>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VINE_QT_PLATFORM_PLUGIN_DIR}/${VINE_OFFSCREEN_PLUGIN}"
                    "$<TARGET_FILE_DIR:vi::Appfw>/platforms/"
            COMMENT "--------Deploy the offscreen platform plugin (ctest uses QT_QPA_PLATFORM=offscreen)")
elseif (VINE_QT_PLATFORM_PLUGIN_DIR)
    message(WARNING "No offscreen platform plugin ('${VINE_OFFSCREEN_PLUGIN}') in "
            "'${VINE_QT_PLATFORM_PLUGIN_DIR}'; ctest runs with QT_QPA_PLATFORM=offscreen "
            "will fall back to the plugin Qt finds by itself")
else ()
    message(WARNING "Qt platform plugin directory not found for '${Qt6_DIR}'; "
            "test_gui will not start under ctest (QT_QPA_PLATFORM=offscreen)")
endif ()

set_target_properties(DeployQt PROPERTIES FOLDER CMakeUtilTargets)
set_target_properties(DeployQt PROPERTIES SOURCES ${CMAKE_CURRENT_LIST_DIR}/VineDeployQt.cmake)