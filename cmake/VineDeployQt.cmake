add_custom_target(DeployQt COMMENT "Deploy Qt runtime using windeployqt")

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

# windeployqt only deploys the platform plugin of the host platform (qwindows*.dll
# on Windows), but test_gui runs from ctest with QT_QPA_PLATFORM=offscreen. Without
# a deployed qoffscreen*.dll Qt aborts before any test runs:
#   qt.qpa.plugin: Could not find the Qt platform plugin "offscreen" in ""
# So copy that plugin next to the one windeployqt placed.
set(VINE_QT_PLATFORM_PLUGIN_DIR "")
if (Qt6_DIR)
    get_filename_component(VINE_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
    if (EXISTS "${VINE_QT_PREFIX}/plugins/platforms")
        set(VINE_QT_PLATFORM_PLUGIN_DIR "${VINE_QT_PREFIX}/plugins/platforms")
    endif()
endif()

# MSVC Debug Qt kits ship 'd'-suffixed plugins (qoffscreend.dll); the other
# configurations link the release Qt, whose plugin is named qoffscreen.dll.
set(VINE_OFFSCREEN_PLUGIN "qoffscreen$<$<CONFIG:Debug>:d>.dll")

if (VINE_QT_PLATFORM_PLUGIN_DIR)
    add_custom_command(
            TARGET DeployQt
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vi::Appfw>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VINE_QT_PLATFORM_PLUGIN_DIR}/${VINE_OFFSCREEN_PLUGIN}"
                    "$<TARGET_FILE_DIR:vi::Appfw>/platforms/"
            COMMENT "--------Deploy the offscreen platform plugin (ctest uses QT_QPA_PLATFORM=offscreen)")
else ()
    message(WARNING "Qt platform plugin directory not found for '${Qt6_DIR}'; "
            "test_gui will not start under ctest (QT_QPA_PLATFORM=offscreen)")
endif ()

set_target_properties(DeployQt PROPERTIES FOLDER CMakeUtilTargets)
set_target_properties(DeployQt PROPERTIES SOURCES ${CMAKE_CURRENT_LIST_DIR}/VineDeployQt.cmake)