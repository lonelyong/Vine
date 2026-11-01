# VineTranslate.cmake — builds a module's own translation catalogs and embeds them.
#
# A module (the framework, the host, a plugin) owns its .ts files: they live next to the code that writes the strings,
# and the compiled .qm ships INSIDE that module's binary instead of as a loose file beside it. The loader finds it by
# convention — <id>_<locale>.qm beside the module first, then :/i18n/<id>_<locale>.qm — so nothing has to be registered
# or scanned (see .ai/design/appfw-translation.md §2.10 and ADR-2026-11-01-translation-catalog-embedding.md).
#
# Only lrelease runs here, never lupdate: extracting the source strings rewrites a tracked file, which is a deliberate
# developer action, not a build step.

# vn_add_translations(<target> CONTEXT <id> TS_FILES <file>...)
#
# Every .ts must be named <id>_<locale>.ts: the id is the tr() context, and the base name becomes the embedded resource
# name (:/i18n/<id>_<locale>.qm), so a mismatch would produce a catalog the loader can never find.
#
# The target must link Qt6::Core itself: the generated resource code calls Qt's own resource API, and a library that only
# links vn::Appfw gets Qt privately (this is the same rule vn_add_plugin() states for plugins that ship resources).
function(vn_add_translations target)
    cmake_parse_arguments(ARG "" "CONTEXT" "TS_FILES" ${ARGN})

    if(NOT ARG_CONTEXT)
        message(FATAL_ERROR "vn_add_translations(${target}): CONTEXT <id> is required")
    endif()
    if(NOT ARG_TS_FILES)
        message(FATAL_ERROR "vn_add_translations(${target}): TS_FILES is required")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "vn_add_translations: '${target}' is not a target")
    endif()
    if(NOT TARGET Qt6::lrelease)
        # No tools, no catalogs: the module still builds and shows the source strings (P6 in the design doc). This is a
        # warning rather than an error so a developer without LinguistTools can still build the application.
        message(WARNING "vn_add_translations(${target}): Qt6::lrelease is unavailable "
                        "(find_package(Qt6 COMPONENTS LinguistTools)); its catalogs are not embedded")
        return()
    endif()

    set(qm_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n")
    set(qm_files)
    foreach(ts_file IN LISTS ARG_TS_FILES)
        if(NOT IS_ABSOLUTE "${ts_file}")
            set(ts_file "${CMAKE_CURRENT_SOURCE_DIR}/${ts_file}")
        endif()

        get_filename_component(ts_name "${ts_file}" NAME_WE)
        if(NOT ts_name MATCHES "^${ARG_CONTEXT}_")
            message(FATAL_ERROR "vn_add_translations(${target}): '${ts_name}' must be named "
                                "'${ARG_CONTEXT}_<locale>.ts' — the id is the tr() context and the catalog's prefix")
        endif()

        set(qm_file "${qm_dir}/${ts_name}.qm")
        add_custom_command(
            OUTPUT "${qm_file}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${qm_dir}"
            COMMAND Qt6::lrelease "${ts_file}" -qm "${qm_file}"
            DEPENDS "${ts_file}"
            COMMENT "lrelease ${ts_name}.ts"
            VERBATIM)
        list(APPEND qm_files "${qm_file}")
    endforeach()

    # BASE + FILES keeps the file name as the resource alias: :/i18n/<id>_<locale>.qm.
    qt_add_resources(${target} "${target}_translations"
        PREFIX "/i18n"
        BASE "${qm_dir}"
        FILES ${qm_files})
endfunction()
