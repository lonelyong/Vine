function(vn_add_library target_name_var short_name)
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
        add_library(${target_name} SHARED ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
    else()
        add_library(${target_name} INTERFACE ${sdk_file_list} ${header_file_list} ${src_file_list} ${rc_file_list})
    endif()

    # MSVC options for the project's own code only. This helper is not used by
    # FetchContent dependencies, so a global add_compile_options() would leak
    # into them (e.g. assimp sets its own /source-charset:utf-8, conflicting
    # with /utf-8). UTF-8 sources, C++ exceptions, conforming preprocessor for
    # __VA_OPT__ macros, and the real __cplusplus value.
    if(src_file_list AND MSVC)
        target_compile_options(${target_name}
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

    set_target_properties(${target_name} PROPERTIES PREFIX "")
    # 设置源文件分组
    source_group(TREE ${sdk_dir} PREFIX sdk FILES ${sdk_file_list})
    source_group(TREE ${inc_dir} PREFIX headers FILES ${header_file_list})
    source_group(TREE ${src_dir} PREFIX src FILES ${src_file_list})
    # source_group(res FILES ${rc_file_list})

    string(TOUPPER VN_${short_name}_LIB lib_compile_def)

    if(src_file_list)
        target_compile_definitions(${target_name} PRIVATE ${lib_compile_def})
        # PRIVATE   仅作用与目标自身
        # PUBLIC    同时作用于目标自身与依赖该目标的目标
        # INTERFACE 仅作用于依赖该目标的目标
        target_include_directories(${target_name}
            PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
            "$<INSTALL_INTERFACE:include>")

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
    # install(FILES ${sdk_file_list} DESTINATION ${CMAKE_INSTALL_PREFIX}/include/${src_rel_dir})
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
