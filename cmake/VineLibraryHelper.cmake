# vn_add_library — one module, one set of objects, one or two images.
#
# The shared library is the module's image for its CLIENTS: it exports the SDK and nothing else. A module's own
# test that drives the PRIVATE half is not such a client - it has to call the same definitions, and it may not
# hold a second copy of them: two definitions of one class in one process is an out-of-line ODR violation, and
# AddressSanitizer aborts on the duplicate typeinfo before a single case runs.
#
# `OBJECT_TARGET <name>` answers that need once, at the module: the translation units are compiled into an object
# set, and the shared library is built from it. The module's test then links the object set (no file list in
# tests/, no source compiled twice) and does not load the library, so exactly one definition exists per process on
# every platform. Without the keyword the function behaves as before (the shared target compiles its own sources).
#
# A caller that asks for it owns the consequences: the object set is now what COMPILES the module, so the module's
# compile-time configuration (its public definitions and its link libraries, the dependencies' include directories)
# has to be applied to the object target as well as to the shared one - see src/viz/graphics/CMakeLists.txt for the
# one-loop form. The shared target stays the module's public name (`vn::<ShortName>`) and the only installed one.
#
# Rationale and the rejected alternatives: ADR-2026-11-27-a-module-test-links-the-modules-objects.
function(vn_add_library target_name_var short_name)
    cmake_parse_arguments(VN_LIB "" "OBJECT_TARGET" "" ${ARGN})
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
    set(target_name ${short_name})
    # 目标别名
    set(target_alias ${VN_SHARED_LIBRARY_PREFIX}::${short_name})
    # 库文件名称
    set(lib_file_name $<$<PLATFORM_ID:Linux>:lib>${VN_SHARED_LIBRARY_PREFIX}${short_name})

    # 创建目标
    if(src_file_list)
        if(VN_LIB_OBJECT_TARGET)
            # THE SOURCES ARE THE MODULE'S OBJECTS; THE SHARED LIBRARY IS ONE IMAGE OF THEM (see the note above).
            set(compile_target ${VN_LIB_OBJECT_TARGET})
            add_library(${compile_target} OBJECT ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
            # PIC is not implied for an object set (a shared library would imply it for its own compilation), and
            # these objects go into one: a non-PIC object with a local-exec TLS reference cannot be linked into a
            # shared object at all.
            set_target_properties(${compile_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
            add_library(${target_name} SHARED)
            # PRIVATE: the objects are part of this image, not of the library's interface - an exported target may
            # not name a target that is never installed, and a test that wants the objects asks for them by name.
            target_link_libraries(${target_name} PRIVATE ${compile_target})
        else()
            set(compile_target ${target_name})
            add_library(${target_name} SHARED ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
        endif()
    else()
        set(compile_target "")
        add_library(${target_name} INTERFACE ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
    endif()

    # MSVC options for the project's own code only. This helper is not used by
    # FetchContent dependencies, so a global add_compile_options() would leak
    # into them (e.g. assimp sets its own /source-charset:utf-8, conflicting
    # with /utf-8). UTF-8 sources, C++ exceptions, conforming preprocessor for
    # __VA_OPT__ macros, and the real __cplusplus value.
    if(src_file_list AND MSVC)
        target_compile_options(${compile_target}
            PRIVATE /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus)
    endif()
    # 设置目标别名，仅影响build阶段，不影响install
    add_library(${target_alias} ALIAS ${target_name})
    # 设置输出文件名
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${lib_file_name})
    # 设置输出分组：按 src 下一级目录自动归组（src/base/logging -> vine/base）
    set(target_folder "vine")
    file(RELATIVE_PATH src_rel_dir ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    string(REPLACE "\\" "/" src_rel_dir "${src_rel_dir}")
    if(NOT src_rel_dir STREQUAL "" AND NOT src_rel_dir MATCHES "^[.][.]/")
        string(REPLACE "/" ";" src_rel_parts "${src_rel_dir}")
        list(GET src_rel_parts 0 group_level)
        set(target_folder "vine/${group_level}")
    endif()
    set_target_properties(${target_name} PROPERTIES FOLDER ${target_folder})
    if(VN_LIB_OBJECT_TARGET)
        # The object set is where the code now lives, so it is also the module's public name for the images that
        # compile it in (its own tests); the shelves follow it in the IDE.
        set_target_properties(${compile_target} PROPERTIES FOLDER ${target_folder})
        add_library(${VN_SHARED_LIBRARY_PREFIX}::${short_name}Objects ALIAS ${compile_target})
    endif()

    set_target_properties(${target_name} PROPERTIES PREFIX "")
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
        target_compile_definitions(${compile_target} PRIVATE ${lib_compile_def})
        target_include_directories(${compile_target}
            PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>")
        if(NOT "${compile_target}" STREQUAL "${target_name}")
            # The shell states the same build-tree roots for its consumers, because it does not compile and so
            # cannot forward the object set's requirements (its link to them is PRIVATE).
            target_include_directories(${target_name}
                PUBLIC
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>")
        endif()
        target_include_directories(${target_name} PUBLIC "$<INSTALL_INTERFACE:include>")

    else()
        target_include_directories(${target_name}
            INTERFACE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
            "$<INSTALL_INTERFACE:include>")
    endif()


    if("${target_name_var}" STREQUAL "")
        message(SEND_ERROR "输出的目标名称变量名不能为空")
    else()
        set(${target_name_var} ${target_name} PARENT_SCOPE)
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
        TARGETS ${target_name}
        EXPORT ${CMAKE_PROJECT_NAME}Targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # for executables(including .dll on Windows)
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # for shared libraries(.so for Linux)
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}) # for static libraries(.a .lib)(including dll import library)



    message(--------AddLib:${target_alias})

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
