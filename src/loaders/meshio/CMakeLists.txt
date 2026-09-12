# MeshIO — mesh import/export (STL, OBJ, FBX, glTF, ...), backed by assimp.
#
# Split out of the former `modelio`, which held two unrelated halves: mesh I/O
# (assimp-backed) and B-rep I/O (no third-party code at all). Only this half
# needs assimp, so keeping them together meant anything that wanted a B-rep
# loader also had to build assimp.
v_add_library(MESHIO_TARGET_NAME MeshIO)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

if(VINE_USE_FETCHCONTENT)
    # assimp is baked into the MeshIO DLL as a static library. Only the library
    # itself is built: no samples, no tests, no CLI tools, no viewer.
    FetchContent_Declare(assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG        v5.4.3
        GIT_SHALLOW    TRUE
    )
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) # Static lib, baked into the DLL.
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_VIEW OFF CACHE BOOL "" FORCE)
    # Do not build assimp's bundled zlib (contrib/zlib defines zlibstatic, which
    # clashes with the zlib fetched by vi::IOBase). Use that zlibstatic instead.
    set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
    # assimp is baked into the MeshIO DLL and never installed; skip its
    # install(EXPORT) rules, which would fail because the zlibstatic it links
    # is not part of assimp's export set.
    set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(assimp)

    # assimp builds itself with -Wall -Werror (code/CMakeLists.txt); clang flags
    # some memcpy calls as errors (-Wnontrivial-memcall) that GCC does not, so
    # downgrade all warnings back to non-errors for the vendored library.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(assimp PRIVATE -Wno-error)
    endif()
    # MSVC builds assimp with /WX (warnings-as-errors); its vendored headers
    # trigger warnings with the current toolchain (e.g. Exceptional.h), so
    # downgrade them to plain warnings here too.
    if(MSVC)
        target_compile_options(assimp PRIVATE /WX-)
    endif()

    v_group_targets_folder(${assimp_SOURCE_DIR} third_party)
else()
    # Consume assimp from vcpkg or a local install.
    find_package(assimp CONFIG REQUIRED)
endif()

target_link_libraries(${MESHIO_TARGET_NAME} PUBLIC vi::Runtime vi::Geometry vi::Crypto)
target_link_libraries(${MESHIO_TARGET_NAME} PRIVATE assimp::assimp zlibstatic)
