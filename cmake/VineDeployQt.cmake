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
            COMMAND ${WINDEPLOYQT_EXE} --verbose 0 $<TARGET_FILE:vn::Appfw>
            COMMENT "--------Run:windeployqt")
else ()
    message(STATUS "DeployQt: no Qt deployment needed on ${CMAKE_SYSTEM_NAME} (windeployqt is Windows-only)")
endif ()

# Locates the offscreen platform plugin of the Qt in use.
#
# ctest runs test_gui with QT_QPA_PLATFORM=offscreen while windeployqt only deploys the host
# platform plugin (qwindows*.dll on Windows), so the offscreen plugin has to be copied next to the
# Qt libraries by hand. Both the deploy target below and tests/test_gui/CMakeLists.txt need the same
# answer, so the lookup lives here.
#
# The plugin directory is looked up as: the layout Qt itself reports, then the SDK layout relative
# to Qt6_DIR, then the layouts used by Linux distributions.
#
# @param out_dir Plugin directory holding the offscreen plugin, or empty when it was not found.
function(vn_find_qt_offscreen_plugin_dir out_dir)
    set(_plugin_dirs "")
    if (DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_PLUGINS)
        list(APPEND _plugin_dirs "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/platforms")
    endif()
    if (Qt6_DIR)
        get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
        list(APPEND _plugin_dirs
                "${_qt_prefix}/plugins/platforms"
                "${_qt_prefix}/lib/qt6/plugins/platforms"
                "${_qt_prefix}/qt6/plugins/platforms")
    endif()

    # Both spellings are looked for on disk: the release/debug choice rides in a generator
    # expression ($<$<CONFIG:Debug>:d>), which if(EXISTS) cannot evaluate.
    if (WIN32)
        set(_names "qoffscreen.dll" "qoffscreend.dll")
    elseif (APPLE)
        set(_names "libqoffscreen.dylib" "libqoffscreend.dylib")
    else ()
        set(_names "libqoffscreen.so" "libqoffscreend.so")
    endif()

    set(_plugin_dir "")
    foreach (_candidate IN LISTS _plugin_dirs)
        foreach (_name IN LISTS _names)
            if (NOT _plugin_dir AND EXISTS "${_candidate}/${_name}")
                set(_plugin_dir "${_candidate}")
            endif()
        endforeach()
    endforeach()

    set(${out_dir} "${_plugin_dir}" PARENT_SCOPE)
endfunction()

# The file name to copy, Debug Qt kits being 'd'-suffixed (qoffscreend.dll).
if (WIN32)
    set(VN_QT_OFFSCREEN_PLUGIN_NAME "qoffscreen$<$<CONFIG:Debug>:d>.dll")
elseif (APPLE)
    set(VN_QT_OFFSCREEN_PLUGIN_NAME "libqoffscreen$<$<CONFIG:Debug>:d>.dylib")
else ()
    set(VN_QT_OFFSCREEN_PLUGIN_NAME "libqoffscreen$<$<CONFIG:Debug>:d>.so")
endif()

vn_find_qt_offscreen_plugin_dir(VN_QT_PLATFORM_PLUGIN_DIR)

if (VN_QT_PLATFORM_PLUGIN_DIR)
    add_custom_command(
            TARGET DeployQt
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vn::Appfw>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VN_QT_PLATFORM_PLUGIN_DIR}/${VN_QT_OFFSCREEN_PLUGIN_NAME}"
                    "$<TARGET_FILE_DIR:vn::Appfw>/platforms/"
            COMMENT "--------Deploy the offscreen platform plugin (ctest uses QT_QPA_PLATFORM=offscreen)")
else ()
    message(WARNING "Qt platform plugin directory not found for '${Qt6_DIR}'; "
            "test_gui will not start under ctest (QT_QPA_PLATFORM=offscreen)")
endif ()

set_target_properties(DeployQt PROPERTIES FOLDER CMakeUtilTargets)
set_target_properties(DeployQt PROPERTIES SOURCES ${CMAKE_CURRENT_LIST_DIR}/VineDeployQt.cmake)