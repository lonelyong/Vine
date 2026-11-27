# vn_add_library — one module: its sources, and the image(s) they are built into.
#
# Usage:
#
#     vn_add_library(<target_name_var> <ShortName> [OBJECT_TARGET <objects_var>])
#
# @param target_name_var  Name of the variable that receives the shared library's target name. `vn::<ShortName>` is
#                         an alias of the same target, and that target is the module's installed name.
# @param short_name       The module's name (`Graphics`): the target's name, the library file's, and the prefix of the
#                         object set (`<ShortName>Objects`).
# @param OBJECT_TARGET    Optional keyword. It takes the NAME OF A VARIABLE, not a target name: the helper derives
#                         the object set's target name itself and sets that variable in the caller's scope, so the
#                         module, its test and any loop over both images cannot disagree about it. Asking for it
#                         makes the object set the target that COMPILES the module; the shared library becomes a
#                         shell around it, and both are configured from the same glob and the same includes.
#
# The object set has ONE consumer: the module's own test, when it drives the private half. Such a test may not link the
# library AND compile its translation units - two definitions of one class in one process is an out-of-line ODR
# violation, and AddressSanitizer aborts on the duplicate typeinfo before a single case runs. Because the object set
# is what compiles, whatever the shared image states for itself (compile definitions, compile options, link
# libraries) must be stated on the object set too; the helper compares the two at the end of the caller's directory
# and stops the configure when one is missing.
#
# The object set is a BUILD-TREE target: not installed, not exported, no `vn::` alias - that namespace is the
# installed package, and a name there that `find_package` cannot resolve is a promise the build cannot keep.
#
# Rationale and the rejected alternatives: ADR-2026-11-27-a-module-test-links-the-modules-objects.
function(vn_add_library target_name_var short_name)
    cmake_parse_arguments(ARG "" "OBJECT_TARGET" "" ${ARGN})
    set(has_object_set FALSE)
    if(DEFINED ARG_OBJECT_TARGET)
        if("${ARG_OBJECT_TARGET}" STREQUAL "")
            message(SEND_ERROR
                "vn_add_library(${short_name}): OBJECT_TARGET needs the name of the variable that receives the object target")
        else()
            set(has_object_set TRUE)
        endif()
    endif()
    # CMAKE_SOURCE_DIR顶级当前CMakeLists.txt文件所在目录
    # CMAKE_CURRENT_SOURCE_DIR 当前CMakeLists.txt所在目录
    # file(RELATIVE_PATH src_rel_dir ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    # # SDK头文件所在目录
    # set(sdk_inc_root_dir ${CMAKE_SOURCE_DIR}/include)
    # set(sdk_dir ${sdk_inc_root_dir}/${src_rel_dir})

    set(sdk_dir "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
    set(inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

    # SDK头文件
    file(GLOB_RECURSE sdk_file_list ${sdk_dir}/*.hpp ${sdk_dir}/*.h)
    # 头文件
    file(GLOB_RECURSE header_file_list LIST_DIRECTORIES false ${inc_dir}/*.h ${inc_dir}/*.hh ${inc_dir}/*.hxx ${inc_dir}/*.hpp)
    # CPP文件
    file(GLOB_RECURSE src_file_list LIST_DIRECTORIES false ${src_dir}/*.c ${src_dir}/*.cc ${src_dir}/*.cpp ${src_dir}/*.cxx)
    # 资源文件
    file(GLOB_RECURSE rc_file_list LIST_DIRECTORIES false *.rc *.qrc)
    # 目标名称
    set(shared_target ${short_name})
    # 目标别名
    set(target_alias ${VN_SHARED_LIBRARY_PREFIX}::${short_name})
    # 库文件名称
    set(lib_file_name $<$<PLATFORM_ID:Linux>:lib>${VN_SHARED_LIBRARY_PREFIX}${short_name})

    # 创建目标
    if(src_file_list)
        set(object_set "")
        set(compiling_target ${shared_target})
        if(has_object_set)
            # THE SOURCES ARE THE MODULE'S OBJECTS; THE SHARED LIBRARY IS ONE IMAGE OF THEM (see the note above).
            set(object_set ${short_name}Objects)
            add_library(${object_set} OBJECT ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
            # PIC is not implied for an object set (a shared library would imply it for its own compilation), and
            # these objects go into one: a non-PIC object with a local-exec TLS reference cannot be linked into a
            # shared object at all.
            set_target_properties(${object_set} PROPERTIES POSITION_INDEPENDENT_CODE ON)
            add_library(${shared_target} SHARED)
            # PRIVATE: the objects are part of this image, not of the library's interface - an exported target may
            # not name a target that is never installed, and a test that wants the objects asks for them by name.
            target_link_libraries(${shared_target} PRIVATE ${object_set})
            set(compiling_target ${object_set})
        else()
            add_library(${shared_target} SHARED ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
        endif()
    else()
        if(has_object_set)
            message(SEND_ERROR
                "vn_add_library(${short_name}): OBJECT_TARGET needs sources to compile; src/ holds none")
        endif()
        set(object_set "")
        set(compiling_target "")
        add_library(${shared_target} INTERFACE ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
    endif()

    # MSVC options for the project's own code only. This helper is not used by
    # FetchContent dependencies, so a global add_compile_options() would leak
    # into them (e.g. assimp sets its own /source-charset:utf-8, conflicting
    # with /utf-8). UTF-8 sources, C++ exceptions, conforming preprocessor for
    # __VA_OPT__ macros, and the real __cplusplus value.
    if(src_file_list AND MSVC)
        target_compile_options(${compiling_target}
            PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
    endif()
    # 设置目标别名，仅影响build阶段，不影响install
    add_library(${target_alias} ALIAS ${shared_target})
    # 设置输出文件名
    set_target_properties(${shared_target} PROPERTIES OUTPUT_NAME ${lib_file_name})
    # 设置输出分组：按 src 下一级目录自动归组（src/base/logging -> vine/base）
    set(target_folder "vine")
    file(RELATIVE_PATH src_rel_dir ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    string(REPLACE "\\" "/" src_rel_dir "${src_rel_dir}")
    if(NOT src_rel_dir STREQUAL "" AND NOT src_rel_dir MATCHES "^[.][.]/")
        string(REPLACE "/" ";" src_rel_parts "${src_rel_dir}")
        list(GET src_rel_parts 0 group_level)
        set(target_folder "vine/${group_level}")
    endif()
    set_target_properties(${shared_target} PROPERTIES FOLDER ${target_folder})
    if(object_set)
        set_target_properties(${object_set} PROPERTIES FOLDER ${target_folder})
    endif()

    set_target_properties(${shared_target} PROPERTIES PREFIX "")
    # 设置源文件分组
    source_group(TREE ${sdk_dir} PREFIX sdk FILES ${sdk_file_list})
    source_group(TREE ${inc_dir} PREFIX headers FILES ${header_file_list})
    source_group(TREE ${src_dir} PREFIX src FILES ${src_file_list})
    # source_group(res FILES ${rc_file_list})

    string(TOUPPER VN_${short_name}_LIB lib_compile_def)

    if(src_file_list)
        # PRIVATE   仅作用与目标自身
        # PUBLIC    同时作用于目标自身与依赖该目标的目标
        # INTERFACE 仅作用于依赖该目标的目标
        target_compile_definitions(${compiling_target} PRIVATE ${lib_compile_def})
        target_include_directories(${compiling_target}
            PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>")
        if(object_set)
            # The shell states the same build-tree roots for its consumers, because it does not compile and so
            # cannot forward the object set's requirements (its link to them is PRIVATE).
            target_include_directories(${shared_target}
                PUBLIC
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>")
        endif()
        target_include_directories(${shared_target} PUBLIC "$<INSTALL_INTERFACE:include>")

    else()
        target_include_directories(${shared_target}
            INTERFACE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
            "$<INSTALL_INTERFACE:include>")
    endif()


    if("${target_name_var}" STREQUAL "")
        message(SEND_ERROR "输出的目标名称变量名不能为空")
    else()
        set(${target_name_var} ${shared_target} PARENT_SCOPE)
    endif()
    if(object_set)
        # The caller's name for the image that compiles; the target's name stays the helper's to choose, so a
        # test, the module and the `foreach` over both images cannot disagree about it.
        set(${ARG_OBJECT_TARGET} ${object_set} PARENT_SCOPE)
        # The caller configures the two images AFTER this call, so they are compared at the end of its directory.
        # The names are expanded HERE: `DEFER` records its arguments for expansion when the call runs, and this
        # function's locals are gone by then.
        cmake_language(EVAL CODE
            "cmake_language(DEFER CALL _vn_check_module_images \"${shared_target}\" \"${object_set}\")")
    endif()


    # string(TOLOWER ${short_name} ShortName_lowercase)
    # string(TOLOWER ${CMAKE_PROJECT_NAME} proj_name_lowercase)

    # 安装头文件（相对路径，尊重 CMAKE_INSTALL_PREFIX / --prefix）
    install(DIRECTORY ${sdk_dir}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    # message(----------${sdk_dir})
    # message(----------${CMAKE_INSTALL_PREFIX}/include/${src_rel_dir})
    # foreach(sdk_file ${sdk_file_list})
    #     get_filename_component(sdk_file_dir ${sdk_file} DIRECTORY)
    #     file(RELATIVE_PATH sdk_file_rel_dir ${sdk_inc_root_dir} ${sdk_file_dir})
    #     install(FILES ${sdk_file} DESTINATION ${CMAKE_INSTALL_PREFIX}/include/${sdk_file_rel_dir})
    # endforeach()

    # 安装目标文件
    install(
        TARGETS ${shared_target}
        EXPORT ${CMAKE_PROJECT_NAME}Targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # for executables(including .dll on Windows)
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # for shared libraries(.so for Linux)
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}) # for static libraries(.a .lib)(including dll import library)



    message(--------AddLib:${target_alias})

endfunction()

# _vn_check_module_images(shared_target object_target) — the one thing a caller can forget.
#
# The object set is what COMPILES the module, so every compile definition, compile option and link library the
# shared image states for itself must also be stated on the object set. A missing INCLUDE directory or link library
# fails loudly in the build; a definition that only changes an `#ifdef` path does not fail at all - the shell carries
# it, compiles nothing, and the image that compiles was built without it. This runs at the end of the caller's
# directory, where both images are configured.
function(_vn_check_module_images shared_target object_target)
    set(missing "")
    foreach(property COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
        get_target_property(stated ${shared_target} ${property})
        get_target_property(carried ${object_target} ${property})
        if("${stated}" STREQUAL "${property}-NOTFOUND")
            continue()
        endif()
        if("${carried}" STREQUAL "${property}-NOTFOUND")
            set(carried "")
        endif()
        if(property STREQUAL "LINK_LIBRARIES")
            # How the image is MADE (the shell links the objects), not a dependency to mirror.
            list(REMOVE_ITEM stated ${object_target})
        endif()
        foreach(item IN LISTS stated)
            if(NOT item IN_LIST carried)
                list(APPEND missing "${item} (${property})")
            endif()
        endforeach()
    endforeach()
    if(missing)
        string(REPLACE ";" ", " missing_text "${missing}")
        message(FATAL_ERROR
            "${shared_target} states ${missing_text}, which ${object_target} does not.\n"
            "The object set is what compiles the module, so both images take the same settings - apply them in one "
            "loop over the two targets (src/viz/graphics/CMakeLists.txt is the shape).")
    endif()
endfunction()

# vn_group_targets_folder(dir folder)
# Recursively sets the FOLDER property on every target created in the given
# directory and all its subdirectories. FetchContent third-party projects may
# declare targets in nested subdirectories (e.g. mbedtls library/, 3rdparty/),
# so a single-level BUILDSYSTEM_TARGETS query misses them. This groups all of
# them under one solution folder, e.g. third_party.
function(vn_group_targets_folder dir folder)
    get_property(_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_tgt IN LISTS _targets)
        set_target_properties(${_tgt} PROPERTIES FOLDER ${folder})
    endforeach()
    get_property(_subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        vn_group_targets_folder(${_sub} ${folder})
    endforeach()
endfunction()
