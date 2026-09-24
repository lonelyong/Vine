
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# include_directories(
#     "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>")

if(WIN32)
    #link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>)
endif()

# set(CMAKE_SHARED_LIBRARY_PREFIX vn)
# # Prefix all static libraries with 'vn'.
# set(CMAKE_STATIC_LIBRARY_PREFIX vn)

set(VN_SHARED_LIBRARY_PREFIX vn)

include(VineLibraryHelper)
include(VinePluginHelper)

add_subdirectory(base)
# loaders (assimp) consumes the zlibstatic that base/iobase builds, so base
# must be configured before loaders for the target to exist when assimp runs
# find_package(ZLIB) (assimp's bundled zlib is disabled in loaders/meshio).
add_subdirectory(loaders)
add_subdirectory(viz)
add_subdirectory(fw)
add_subdirectory(robotics)
add_subdirectory(app)
add_subdirectory(plugins)

file(GLOB VN_GLOBAL_HEADER_FILES LIST_DIRECTORIES false ${CMAKE_SOURCE_DIR}/include/vine/*.h)

install(FILES ${VN_GLOBAL_HEADER_FILES} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vine)
