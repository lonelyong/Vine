# Vine plugin helper: creates a plugin DLL that the PluginManager can load.
#
# A plugin is a loadable module living in the plugin directory
# (<exe>/plugins/vine) that exports vinePluginQuery()/vinePluginCreate() via
# V_DECLARE_PLUGIN. Plugins are not linked against each other; dependencies are
# declared in PluginInfo.dependencies and resolved by PluginManager. This
# helper only builds the module and deploys it next to the application; the
# caller still adds find_package(Qt6) and target_link_libraries as needed.

# Main application target that plugins are deployed beside.
set(VI_APP_TARGET "Vine" CACHE STRING "Main application target (plugin deploy directory root)")

function(v_add_plugin target_name_var short_name)
    set(sdk_dir "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
    set(inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

    # SDK headers
    file(GLOB_RECURSE sdk_file_list ${sdk_dir}/*.hpp ${sdk_dir}/*.h)
    # Headers (include dir only)
    file(GLOB_RECURSE header_file_list LIST_DIRECTORIES false
        ${inc_dir}/*.h ${inc_dir}/*.hh ${inc_dir}/*.hxx ${inc_dir}/*.hpp)
    # CPP + headers under src
    file(GLOB_RECURSE src_file_list LIST_DIRECTORIES false
        ${src_dir}/*.c ${src_dir}/*.cc ${src_dir}/*.cpp ${src_dir}/*.cxx
        ${src_dir}/*.h ${src_dir}/*.hh ${src_dir}/*.hxx ${src_dir}/*.hpp)
    # Resource files
    file(GLOB_RECURSE rc_file_list LIST_DIRECTORIES false *.rc *.qrc)

    set(target_name ${short_name})

    # MODULE: a loadable DLL that is not linked against (no import library).
    add_library(${target_name} MODULE ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})

    # MSVC options for the project's own plugin code only (see v_add_library).
    if(MSVC)
        target_compile_options(${target_name}
            PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
    endif()

    # No lib prefix, grouped under vine/plugins in the solution.
    # Mirror PluginManager::defaultBuiltInPluginDirectory() so plugins are found when
    # the app runs from the build tree: Windows/macOS use <exe>/plugins/vine,
    # Linux uses <exe>/../plugins/vine (sibling of bin/, matching install).
    if(WIN32 OR APPLE)
        set(_plugin_output_dir "$<TARGET_FILE_DIR:${VI_APP_TARGET}>/plugins/vine")
    else()
        set(_plugin_output_dir "$<TARGET_FILE_DIR:${VI_APP_TARGET}>/../plugins/vine")
    endif()

    set_target_properties(${target_name} PROPERTIES
        PREFIX ""
        FOLDER vine/plugins
        RUNTIME_OUTPUT_DIRECTORY "${_plugin_output_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${_plugin_output_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${_plugin_output_dir}")

    target_include_directories(${target_name}
        PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

    source_group(TREE ${sdk_dir} PREFIX sdk FILES ${sdk_file_list})
    source_group(TREE ${inc_dir} PREFIX headers FILES ${header_file_list})
    source_group(TREE ${src_dir} PREFIX src FILES ${src_file_list})

    if(NOT "${target_name_var}" STREQUAL "")
        set(${target_name_var} ${target_name} PARENT_SCOPE)
    endif()

    # Runtime resolves plugins at <exe>/plugins/vine on Windows and
    # <prefix>/plugins/vine on Linux (PluginManager::defaultBuiltInPluginDirectory).
    # Match that layout so installed plugins are found next to the installed app:
    # Windows installs the plugin DLL under bin/ (RUNTIME), while Linux/macOS
    # install the shared object directly under the prefix (LIBRARY). CMake still
    # requires a LIBRARY DESTINATION on Windows because the target is a MODULE
    # library (the DLL itself still goes to RUNTIME_DESTINATION).
    if(WIN32)
        install(TARGETS ${target_name}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins/vine
            LIBRARY DESTINATION plugins/vine)
    else()
        install(TARGETS ${target_name}
            LIBRARY DESTINATION plugins/vine)
    endif()

    message(--------AddPlugin:${target_name})
endfunction()
